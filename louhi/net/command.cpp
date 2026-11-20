//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/net/command.h"

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <boost/asio/local/connect_pair.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/write.hpp>
#include <cerrno>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace louhi::net {

namespace {

namespace asio = boost::asio;
using Socket = asio::local::stream_protocol::socket;

// Where a program is looked for: the system's own directories, never the
// caller's PATH.
constexpr std::array<const char*, 4> search_path{"/usr/sbin", "/usr/bin", "/sbin", "/bin"};
constexpr std::chrono::milliseconds reap_every{10};

std::string resolve(const std::string& program) {
  if (program.find('/') != std::string::npos) return ::access(program.c_str(), X_OK) == 0 ? program : std::string();
  for (const char* directory : search_path) {
    const std::string candidate = std::string(directory) + "/" + program;
    if (::access(candidate.c_str(), X_OK) == 0) return candidate;
  }
  return {};
}

// One run: its child, its two sockets and its two timers, on one strand.
class Run : public std::enable_shared_from_this<Run> {
 public:
  Run(asio::io_context& io, runtime::Executor executor, runtime::Completion<CommandResult> done)
      : _strand(asio::make_strand(io)),
        _input(_strand),
        _output(_strand),
        _deadline(_strand),
        _reap(_strand),
        _executor(std::move(executor)),
        _done(std::move(done)) {}

  void start(Invocation invocation, runtime::Deadline deadline) {
    const std::string path = resolve(invocation.argv.front());
    if (path.empty()) return fail({runtime::Error::Kind::unavailable, "starting " + invocation.argv.front() + ": no such program"});

    Socket child_input(_strand), child_output(_strand);
    boost::system::error_code failed;
    asio::local::connect_pair(_input, child_input, failed);
    if (!failed) asio::local::connect_pair(_output, child_output, failed);
    if (failed) return fail({runtime::Error::Kind::unavailable, "starting " + invocation.argv.front() + ": " + failed.message()});

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, child_input.native_handle(), STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, child_output.native_handle(), STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, child_output.native_handle(), STDERR_FILENO);
    std::vector<char*> argv;
    for (auto& argument : invocation.argv) argv.push_back(argument.data());
    argv.push_back(nullptr);
    std::string path_variable = "PATH=/usr/sbin:/usr/bin:/sbin:/bin";
    std::string locale = "LC_ALL=C";
    std::array<char*, 3> environment{path_variable.data(), locale.data(), nullptr};
    const int spawned = ::posix_spawn(&_pid, path.c_str(), &actions, nullptr, argv.data(), environment.data());
    posix_spawn_file_actions_destroy(&actions);
    // The child holds its ends now; the parent's copies would keep its
    // output open past its exit.
    child_input.close();
    child_output.close();
    if (spawned != 0) return fail({runtime::Error::Kind::unavailable, "starting " + invocation.argv.front() + ": " + std::strerror(spawned)});
    _running = true;

    _deadline.expires_after(deadline);
    _deadline.async_wait([self = shared_from_this()](const boost::system::error_code& ec) {
      if (ec || !self->_running) return;
      self->_timed_out = true;
      ::kill(self->_pid, SIGKILL);
    });
    write(std::move(invocation.input));
    read();
  }

  void fail(runtime::Error error) {
    if (!_done) return;
    asio::post(_executor, [done = std::move(_done), error = std::move(error)]() mutable { done(runtime::Result<CommandResult>(std::move(error))); });
    _done = nullptr;
  }

 private:
  void write(std::string input) {
    if (input.empty()) return close_input();
    auto held = std::make_shared<std::string>(std::move(input));
    // A child that exits without reading ends the write with an error, never a
    // signal: sockets are written without SIGPIPE.
    asio::async_write(_input, asio::buffer(*held), [self = shared_from_this(), held](const boost::system::error_code&, size_t) { self->close_input(); });
  }

  void close_input() {
    boost::system::error_code ignored;
    _input.shutdown(Socket::shutdown_send, ignored);
    _input.close(ignored);
  }

  void read() {
    _output.async_read_some(asio::buffer(_chunk), [self = shared_from_this()](const boost::system::error_code& ec, size_t got) {
      if (got > 0) {
        const size_t room = ProcessCommands::max_output - self->_result.output.size();
        self->_result.output.append(self->_chunk.data(), std::min(room, got));
        if (got > room) self->_result.truncated = true;
      }
      if (ec) {
        boost::system::error_code ignored;
        self->_output.close(ignored);
        return self->reap();
      }
      self->read();
    });
  }

  // Waits for the child's exit without blocking the io context.
  void reap() {
    int status = 0;
    const pid_t gone = ::waitpid(_pid, &status, WNOHANG);
    if (gone == 0) {
      _reap.expires_after(reap_every);
      _reap.async_wait([self = shared_from_this()](const boost::system::error_code&) { self->reap(); });
      return;
    }
    _running = false;
    _deadline.cancel();
    if (gone < 0) return fail({runtime::Error::Kind::unavailable, std::string("waiting for the program: ") + std::strerror(errno)});
    if (_timed_out) return fail({runtime::Error::Kind::timed_out, "the program did not end within its deadline, and was killed"});
    _result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    asio::post(_executor, [done = std::move(_done), result = std::move(_result)]() mutable { done(runtime::Result<CommandResult>(std::move(result))); });
    _done = nullptr;
  }

  asio::strand<asio::io_context::executor_type> _strand;
  Socket _input;
  Socket _output;
  asio::steady_timer _deadline;
  asio::steady_timer _reap;
  runtime::Executor _executor;
  runtime::Completion<CommandResult> _done;
  std::array<char, 4096> _chunk{};
  CommandResult _result;
  pid_t _pid = 0;
  bool _running = false;
  bool _timed_out = false;
};

}  // namespace

void ProcessCommands::run(Invocation invocation, runtime::Deadline deadline, runtime::Executor executor, runtime::Completion<CommandResult> done) {
  if (invocation.argv.empty() || invocation.argv.front().empty()) throw std::invalid_argument("a command needs a program to run");
  // Started here, on the caller's thread: a spawn waits on nothing, and every
  // completion is posted, never called inline.
  auto run = std::make_shared<Run>(_io, std::move(executor), std::move(done));
  run->start(std::move(invocation), deadline);
}

}  // namespace louhi::net
