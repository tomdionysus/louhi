//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// A container runtime's daemon on a local socket, for the tests of what speaks
// HTTP to one: it accepts connections and answers each with the next scripted
// reply, written as given, then closes its side. An empty script entry means
// "never answer"; one starting "hold:" is written without the prefix and the
// connection is then held open, as the event stream is. Every request's head
// is kept, in order. Thread-safe.
namespace louhi::test {

class FakeDaemon {
 public:
  using local = boost::asio::local::stream_protocol;

  explicit FakeDaemon(const std::string& path) : _acceptor(_io, local::endpoint(path)) {
    accept();
    _thread = std::thread([this] { _io.run(); });
  }
  ~FakeDaemon() {
    _io.stop();
    _thread.join();
  }
  FakeDaemon(const FakeDaemon&) = delete;
  FakeDaemon& operator=(const FakeDaemon&) = delete;

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
      boost::asio::async_read_until(
          *shared, boost::asio::dynamic_buffer(*buffer), "\r\n\r\n", [this, shared, buffer](const boost::system::error_code& read, size_t) {
            if (read) return;
            std::string reply;
            {
              std::lock_guard<std::mutex> lock(_mutex);
              _requests.push_back(*buffer);
              if (!_replies.empty()) {
                reply = _replies.front();
                _replies.erase(_replies.begin());
              }
              // Kept open on this side until the daemon goes, so a body it never
              // read cannot reset the connection before its reply is read.
              _held.push_back(shared);
            }
            if (reply.empty()) return;
            const bool hold = reply.rfind("hold:", 0) == 0;
            if (hold) reply.erase(0, 5);
            auto text = std::make_shared<std::string>(std::move(reply));
            boost::asio::async_write(*shared, boost::asio::buffer(*text), [shared, text, hold](const boost::system::error_code&, size_t) {
              if (hold) return;
              boost::system::error_code ignored;
              shared->shutdown(local::socket::shutdown_send, ignored);
            });
          });
      accept();
    });
  }

  boost::asio::io_context _io;
  local::acceptor _acceptor;
  std::thread _thread;
  std::mutex _mutex;
  std::vector<std::string> _replies;
  std::vector<std::string> _requests;
  std::vector<std::shared_ptr<local::socket>> _held;
};

}  // namespace louhi::test
