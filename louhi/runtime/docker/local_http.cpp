//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/runtime/docker/local_http.h"

#include <array>
#include <atomic>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <utility>

#include "louhi/runtime/docker/api.h"

namespace louhi::runtime::docker {

namespace asio = boost::asio;
namespace http = boost::beast::http;
using Socket = asio::local::stream_protocol::socket;

namespace {

constexpr uint32_t max_header_bytes = 64 * 1024;

http::request<http::string_body> make_request(http::verb verb, std::string target, std::string body, const LocalHttp::Headers& headers) {
  http::request<http::string_body> request{verb, std::move(target), 11};
  // The daemon ignores the host; HTTP/1.1 requires one.
  request.set(http::field::host, "docker");
  request.set(http::field::connection, "close");
  for (const auto& [name, value] : headers) request.set(name, value);
  if (!body.empty()) {
    request.set(http::field::content_type, "application/json");
    request.body() = std::move(body);
  }
  request.prepare_payload();
  return request;
}

// What every exchange shares: the strand its handlers run on, the socket, the
// deadline, and the once-only completion.
struct Exchange {
  Exchange(asio::any_io_executor io, Deadline deadline_, Executor executor_, Completion<Reply> done_)
      : strand(asio::make_strand(io)), socket(strand), timer(strand), deadline(deadline_), executor(std::move(executor_)), done(std::move(done_)) {}

  asio::strand<asio::any_io_executor> strand;
  Socket socket;
  asio::steady_timer timer;
  Deadline deadline;
  Executor executor;
  Completion<Reply> done;
  boost::beast::flat_buffer buffer;
  bool timed_out = false;
  bool finished = false;

  // On the strand.
  void finish(Result<Reply> result) {
    if (finished) return;
    finished = true;
    timer.cancel();
    boost::system::error_code ignored;
    socket.close(ignored);
    if (timed_out) result = Error{Error::Kind::timed_out, "no reply from the runtime by the deadline"};
    asio::post(executor, [done_ = std::move(done), result_ = std::move(result)]() mutable { done_(std::move(result_)); });
  }

  void fail(Error::Kind kind, std::string message) { finish(Error{kind, std::move(message)}); }
};

template <class Self>
void arm_deadline(const std::shared_ptr<Self>& self) {
  if (self->deadline.count() <= 0) return;
  self->timer.expires_after(self->deadline);
  self->timer.async_wait([self](const boost::system::error_code& failed) {
    if (failed || self->finished) return;
    self->timed_out = true;
    boost::system::error_code ignored;
    self->socket.close(ignored);
  });
}

struct Whole : Exchange, std::enable_shared_from_this<Whole> {
  using Exchange::Exchange;
  http::request<http::string_body> request;
  http::response_parser<http::string_body> parser;

  void start(const std::string& path) {
    parser.header_limit(max_header_bytes);
    parser.body_limit(max_reply_bytes);
    auto self = shared_from_this();
    asio::dispatch(strand, [self, path] {
      arm_deadline(self);
      self->socket.async_connect(asio::local::stream_protocol::endpoint(path), [self](const boost::system::error_code& failed) {
        if (failed) return self->fail(Error::Kind::unavailable, "cannot reach the runtime's socket: " + failed.message());
        http::async_write(self->socket, self->request, [self](const boost::system::error_code& wrote, size_t) {
          if (wrote) return self->fail(Error::Kind::unavailable, "the runtime closed the connection: " + wrote.message());
          http::async_read(self->socket, self->buffer, self->parser, [self](const boost::system::error_code& read, size_t) {
            if (read == http::error::body_limit || read == http::error::header_limit)
              return self->fail(Error::Kind::malformed, "the runtime's reply is over its bound");
            // Beast's own errors are a reply that is not HTTP; the rest are
            // the connection.
            const bool not_http = read.category() == http::make_error_code(http::error::bad_version).category();
            if (read) return self->fail(not_http ? Error::Kind::malformed : Error::Kind::unavailable, "reading the runtime's reply: " + read.message());
            Reply reply;
            reply.status = self->parser.get().result_int();
            reply.body = std::move(self->parser.get().body());
            self->finish(std::move(reply));
          });
        });
      });
    });
  }
};

struct Lines : Exchange, LocalHttp::Stream, std::enable_shared_from_this<Lines> {
  Lines(asio::any_io_executor io, Deadline deadline_, Executor executor_, std::function<void(std::string)> line_, Completion<Reply> done_)
      : Exchange(std::move(io), deadline_, std::move(executor_), std::move(done_)), line(std::move(line_)) {}

  std::function<void(std::string)> line;
  http::request<http::string_body> request;
  http::response_parser<http::buffer_body> parser;
  std::array<char, 16 * 1024> chunk{};
  std::string pending;
  std::string error_body;
  bool ok = false;
  std::atomic<bool> cancelled{false};

  void cancel() override {
    cancelled = true;
    auto self = shared_from_this();
    asio::dispatch(strand, [self] { self->fail(Error::Kind::unavailable, "cancelled"); });
  }

  void start(const std::string& path) {
    parser.header_limit(max_header_bytes);
    parser.body_limit(boost::none);
    auto self = shared_from_this();
    asio::dispatch(strand, [self, path] {
      arm_deadline(self);
      self->socket.async_connect(asio::local::stream_protocol::endpoint(path), [self](const boost::system::error_code& failed) {
        if (failed) return self->fail(Error::Kind::unavailable, "cannot reach the runtime's socket: " + failed.message());
        http::async_write(self->socket, self->request, [self](const boost::system::error_code& wrote, size_t) {
          if (wrote) return self->fail(Error::Kind::unavailable, "the runtime closed the connection: " + wrote.message());
          http::async_read_header(self->socket, self->buffer, self->parser, [self](const boost::system::error_code& read, size_t) {
            if (read) return self->fail(Error::Kind::unavailable, "reading the runtime's reply: " + read.message());
            self->ok = self->parser.get().result_int() == 200;
            self->read_more();
          });
        });
      });
    });
  }

  void read_more() {
    if (finished) return;
    if (parser.is_done()) return end();
    parser.get().body().data = chunk.data();
    parser.get().body().size = chunk.size();
    auto self = shared_from_this();
    http::async_read_some(socket, buffer, parser, [self](boost::system::error_code read, size_t) {
      if (read == http::error::need_buffer) read = {};
      if (read && read != http::error::end_of_stream) return self->fail(Error::Kind::unavailable, "reading the runtime's stream: " + read.message());
      const size_t got = self->chunk.size() - self->parser.get().body().size;
      if (!self->take(std::string_view(self->chunk.data(), got))) return;
      if (read == http::error::end_of_stream) return self->end();
      self->read_more();
    });
  }

  // Splits what came into lines; false when it ended the stream.
  bool take(std::string_view data) {
    if (!ok) {
      if (error_body.size() + data.size() > max_reply_bytes) {
        fail(Error::Kind::malformed, "the runtime's reply is over its bound");
        return false;
      }
      error_body.append(data);
      return true;
    }
    pending.append(data);
    size_t start = 0;
    for (;;) {
      const size_t newline = pending.find('\n', start);
      if (newline == std::string::npos) break;
      if (newline - start > max_stream_line) {
        fail(Error::Kind::malformed, "a line of the runtime's stream is over its bound");
        return false;
      }
      std::string text = pending.substr(start, newline - start);
      if (!text.empty() && text.back() == '\r') text.pop_back();
      if (!text.empty() && !cancelled) asio::post(executor, [line_ = line, text_ = std::move(text)]() mutable { line_(std::move(text_)); });
      start = newline + 1;
    }
    pending.erase(0, start);
    if (pending.size() > max_stream_line) {
      fail(Error::Kind::malformed, "a line of the runtime's stream is over its bound");
      return false;
    }
    return true;
  }

  void end() {
    if (ok && !pending.empty() && !cancelled) asio::post(executor, [line_ = line, text_ = std::move(pending)]() mutable { line_(std::move(text_)); });
    Reply reply;
    reply.status = parser.get().result_int();
    reply.body = std::move(error_body);
    finish(std::move(reply));
  }
};

}  // namespace

LocalHttp::LocalHttp(asio::any_io_executor io, std::string path) : _io(std::move(io)), _path(std::move(path)) {}

void LocalHttp::request(http::verb verb, std::string target, std::string body, Deadline deadline, Executor executor, Completion<Reply> done) {
  request(verb, std::move(target), std::move(body), {}, deadline, std::move(executor), std::move(done));
}

void LocalHttp::request(http::verb verb, std::string target, std::string body, Headers headers, Deadline deadline, Executor executor, Completion<Reply> done) {
  auto exchange = std::make_shared<Whole>(_io, deadline, std::move(executor), std::move(done));
  exchange->request = make_request(verb, std::move(target), std::move(body), headers);
  exchange->start(_path);
}

std::shared_ptr<LocalHttp::Stream> LocalHttp::stream(http::verb verb, std::string target, std::string body, Deadline deadline, Executor executor,
                                                     std::function<void(std::string)> line, Completion<Reply> done) {
  return stream(verb, std::move(target), std::move(body), {}, deadline, std::move(executor), std::move(line), std::move(done));
}

std::shared_ptr<LocalHttp::Stream> LocalHttp::stream(http::verb verb, std::string target, std::string body, Headers headers, Deadline deadline,
                                                     Executor executor, std::function<void(std::string)> line, Completion<Reply> done) {
  auto exchange = std::make_shared<Lines>(_io, deadline, std::move(executor), std::move(line), std::move(done));
  exchange->request = make_request(verb, std::move(target), std::move(body), headers);
  exchange->start(_path);
  return exchange;
}

}  // namespace louhi::runtime::docker
