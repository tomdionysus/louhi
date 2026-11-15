//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// HTTP over a local socket (runtime/docker/local_http.h): a request's whole
// reply comes back with its status; a deadline that passes ends it as
// timed_out; a socket that is not there is unavailable; a reply over its bound
// or not HTTP is malformed; a stream's lines arrive in order, chunked or not,
// the last one without its newline included; a line over its bound ends the
// stream; cancel ends a stream at once. Every completion comes once, posted to
// the caller's executor. Run against a fake daemon on a socket in the build
// tree.

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/http/verb.hpp>
#include <chrono>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "dannan/test/fixtures/temp_dir.h"
#include "louhi/runtime/docker/api.h"
#include "louhi/runtime/docker/local_http.h"

using namespace louhi::runtime;
using namespace louhi::runtime::docker;
namespace asio = boost::asio;
using local = asio::local::stream_protocol;
using verb = boost::beast::http::verb;

namespace {

constexpr auto wait_limit = std::chrono::seconds(10);

// Accepts connections on a socket and answers each with the next scripted
// reply, written as given, then closes its side. An empty script entry means
// "never answer"; one starting "hold:" is written without the prefix and the
// connection is then held open, as the event stream is.
class FakeDaemon {
 public:
  explicit FakeDaemon(const std::string& path) : _acceptor(_io, local::endpoint(path)) {
    accept();
    _thread = std::thread([this] { _io.run(); });
  }
  ~FakeDaemon() {
    _io.stop();
    _thread.join();
  }

  void script(std::vector<std::string> replies) {
    std::lock_guard<std::mutex> lock(_mutex);
    _replies = std::move(replies);
  }
  std::vector<std::string> requests() {
    std::lock_guard<std::mutex> lock(_mutex);
    return _requests;
  }

 private:
  void accept() {
    _acceptor.async_accept([this](const boost::system::error_code& failed, local::socket socket) {
      if (failed) return;
      auto shared = std::make_shared<local::socket>(std::move(socket));
      auto buffer = std::make_shared<std::string>();
      asio::async_read_until(*shared, asio::dynamic_buffer(*buffer), "\r\n\r\n", [this, shared, buffer](const boost::system::error_code& read, size_t) {
        if (read) return;
        std::string reply;
        {
          std::lock_guard<std::mutex> lock(_mutex);
          _requests.push_back(*buffer);
          if (!_replies.empty()) {
            reply = _replies.front();
            _replies.erase(_replies.begin());
          }
        }
        if (reply.empty()) {
          _held.push_back(shared);
          return;
        }
        const bool hold = reply.rfind("hold:", 0) == 0;
        if (hold) {
          reply.erase(0, 5);
          _held.push_back(shared);
        }
        auto text = std::make_shared<std::string>(std::move(reply));
        asio::async_write(*shared, asio::buffer(*text), [shared, text, hold](const boost::system::error_code&, size_t) {
          if (hold) return;
          boost::system::error_code ignored;
          shared->shutdown(local::socket::shutdown_send, ignored);
        });
      });
      accept();
    });
  }

  asio::io_context _io;
  local::acceptor _acceptor;
  std::thread _thread;
  std::mutex _mutex;
  std::vector<std::string> _replies;
  std::vector<std::string> _requests;
  std::vector<std::shared_ptr<local::socket>> _held;
};

// The client's own io, run on a thread for the test's life.
class Client {
 public:
  Client() : _work(asio::make_work_guard(_io)), _thread([this] { _io.run(); }) {}
  ~Client() {
    _work.reset();
    _io.stop();
    _thread.join();
  }
  asio::any_io_executor executor() { return _io.get_executor(); }
  bool on_io_thread() const { return std::this_thread::get_id() == _thread.get_id(); }

 private:
  asio::io_context _io;
  asio::executor_work_guard<asio::io_context::executor_type> _work;
  std::thread _thread;
};

std::string ok(const std::string& body, unsigned status = 200) {
  return "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

struct LocalHttpTest : ::testing::Test {
  dannan::test::TempDir dir;
  std::string path = (dir.path() / "d.sock").string();
  FakeDaemon daemon{path};
  Client client;
  LocalHttp http{client.executor(), path};

  Result<Reply> request(verb v, const std::string& target, const std::string& body, Deadline deadline) {
    std::promise<Result<Reply>> promise;
    auto future = promise.get_future();
    int calls = 0;
    http.request(v, target, body, deadline, client.executor(), [&](Result<Reply> result) {
      EXPECT_TRUE(client.on_io_thread());
      if (++calls == 1) promise.set_value(std::move(result));
    });
    EXPECT_EQ(future.wait_for(wait_limit), std::future_status::ready);
    return future.get();
  }
};

struct Streamed {
  std::vector<std::string> lines;
  std::optional<Result<Reply>> end;
};

}  // namespace

TEST_F(LocalHttpTest, AWholeReplyWithItsStatus) {
  daemon.script({ok(R"({"Id":"x"})", 201)});
  const auto got = request(verb::post, "/v1.41/containers/create?name=a", R"({"Image":"busybox"})", Deadline(5000));
  ASSERT_TRUE(got.ok()) << got.error().message;
  EXPECT_EQ(got.value().status, 201u);
  EXPECT_EQ(got.value().body, R"({"Id":"x"})");
  const auto seen = daemon.requests();
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0].rfind("POST /v1.41/containers/create?name=a HTTP/1.1\r\n", 0), 0u);
  EXPECT_NE(seen[0].find("Content-Type: application/json"), std::string::npos);
}

TEST_F(LocalHttpTest, AnyStatusCompletesTheRequest) {
  daemon.script({ok(R"({"message":"No such container"})", 404)});
  const auto got = request(verb::get, "/v1.41/containers/x/json", "", Deadline(5000));
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got.value().status, 404u);
}

TEST_F(LocalHttpTest, TheDeadlineEndsARequest) {
  daemon.script({""});
  const auto got = request(verb::get, "/v1.41/version", "", Deadline(50));
  ASSERT_FALSE(got.ok());
  EXPECT_EQ(got.error().kind, Error::Kind::timed_out);
}

TEST_F(LocalHttpTest, AMissingSocketIsUnavailable) {
  LocalHttp nowhere(client.executor(), (dir.path() / "none.sock").string());
  std::promise<Result<Reply>> promise;
  nowhere.request(verb::get, "/v1.41/version", "", Deadline(5000), client.executor(), [&](Result<Reply> r) { promise.set_value(std::move(r)); });
  auto future = promise.get_future();
  ASSERT_EQ(future.wait_for(wait_limit), std::future_status::ready);
  const auto got = future.get();
  ASSERT_FALSE(got.ok());
  EXPECT_EQ(got.error().kind, Error::Kind::unavailable);
}

TEST_F(LocalHttpTest, AReplyOverItsBoundIsMalformed) {
  daemon.script({"HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(max_reply_bytes + 1) + "\r\n\r\n" + std::string(max_reply_bytes + 1, 'x')});
  const auto got = request(verb::get, "/v1.41/containers/json", "", Deadline(10000));
  ASSERT_FALSE(got.ok());
  EXPECT_EQ(got.error().kind, Error::Kind::malformed);
}

TEST_F(LocalHttpTest, AReplyThatIsNotHttpIsMalformed) {
  daemon.script({"SSH-2.0-OpenSSH\r\n\r\n"});
  const auto got = request(verb::get, "/v1.41/version", "", Deadline(5000));
  ASSERT_FALSE(got.ok());
  EXPECT_EQ(got.error().kind, Error::Kind::malformed);
}

namespace {

// Runs a stream to its end. When `cancel_after_first` is set, the test thread
// cancels it once the first line is in.
Streamed run_stream(LocalHttp& http, Client& client, Deadline deadline, bool cancel_after_first = false) {
  Streamed out;
  std::mutex mutex;
  std::promise<void> ended;
  std::promise<void> first_line;
  bool signalled = false;
  auto stream = http.stream(
      verb::get, "/v1.41/events", "", deadline, client.executor(),
      [&](std::string line) {
        std::lock_guard<std::mutex> lock(mutex);
        out.lines.push_back(std::move(line));
        if (!signalled) {
          signalled = true;
          first_line.set_value();
        }
      },
      [&](Result<Reply> end) {
        std::lock_guard<std::mutex> lock(mutex);
        EXPECT_FALSE(out.end.has_value()) << "ended twice";
        out.end = std::move(end);
        ended.set_value();
      });
  if (cancel_after_first) {
    EXPECT_EQ(first_line.get_future().wait_for(wait_limit), std::future_status::ready);
    stream->cancel();
  }
  EXPECT_EQ(ended.get_future().wait_for(wait_limit), std::future_status::ready);
  std::lock_guard<std::mutex> lock(mutex);
  return out;
}

}  // namespace

TEST_F(LocalHttpTest, StreamLinesArriveInOrderChunked) {
  // Chunk boundaries fall inside lines; one line ends "\r\n"; the last line
  // has no newline.
  daemon.script(
      {"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
       "5\r\n{\"a\":\r\n"
       "9\r\n1}\n{\"b\":2\r\n"
       "3\r\n}\r\n\r\n"
       "6\r\n{\"c\":3\r\n"
       "1\r\n}\r\n"
       "0\r\n\r\n"});
  const Streamed got = run_stream(http, client, Deadline(5000));
  EXPECT_EQ(got.lines, (std::vector<std::string>{R"({"a":1})", R"({"b":2})", R"({"c":3})"}));
  ASSERT_TRUE(got.end && got.end->ok());
  EXPECT_EQ(got.end->value().status, 200u);
}

TEST_F(LocalHttpTest, StreamFailureStatusCarriesTheBody) {
  daemon.script({ok(R"({"message":"denied"})", 403)});
  const Streamed got = run_stream(http, client, Deadline(5000));
  EXPECT_TRUE(got.lines.empty());
  ASSERT_TRUE(got.end && got.end->ok());
  EXPECT_EQ(got.end->value().status, 403u);
  EXPECT_EQ(got.end->value().body, R"({"message":"denied"})");
}

TEST_F(LocalHttpTest, AStreamLineOverItsBoundEndsIt) {
  daemon.script({"HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(max_stream_line + 2) + "\r\n\r\n" + std::string(max_stream_line + 1, 'x') + "\n"});
  const Streamed got = run_stream(http, client, Deadline(5000));
  ASSERT_TRUE(got.end);
  ASSERT_FALSE(got.end->ok());
  EXPECT_EQ(got.end->error().kind, Error::Kind::malformed);
}

TEST_F(LocalHttpTest, CancelEndsAnOpenStreamOnce) {
  // Headers and one line, then the connection held open with no deadline: only
  // cancel can end it.
  daemon.script({"hold:HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\none\n\r\n"});
  const Streamed got = run_stream(http, client, Deadline(0), true);
  EXPECT_EQ(got.lines, (std::vector<std::string>{"one"}));
  ASSERT_TRUE(got.end);
  ASSERT_FALSE(got.end->ok());
  EXPECT_EQ(got.end->error().kind, Error::Kind::unavailable);
  EXPECT_EQ(got.end->error().message, "cancelled");
}

TEST_F(LocalHttpTest, AStreamDeadlineEndsIt) {
  daemon.script({"hold:HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"});
  const Streamed got = run_stream(http, client, Deadline(50));
  ASSERT_TRUE(got.end);
  ASSERT_FALSE(got.end->ok());
  EXPECT_EQ(got.end->error().kind, Error::Kind::timed_out);
}
