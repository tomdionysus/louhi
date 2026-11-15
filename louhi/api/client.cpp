//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/api/client.h"

#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <chrono>
#include <optional>

#include "louhi/api/json.h"

namespace louhi::api {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;
namespace json = boost::json;
using Clock = std::chrono::steady_clock;

// One request and its answer over one connection. On the stream's strand.
class Call : public std::enable_shared_from_this<Call> {
 public:
  Call(std::shared_ptr<dannan::listen::Stream> stream, Clock::time_point until, runtime::Executor executor, runtime::Completion<Response> done)
      : _stream(std::move(stream)), _timer(_stream->get_executor()), _until(until), _executor(std::move(executor)), _done(std::move(done)) {}

  void start(std::string request) {
    _timer.expires_at(_until);
    _timer.async_wait([weak = weak_from_this()](const boost::system::error_code& ec) {
      if (const auto self = weak.lock(); self && !ec) self->fail({runtime::Error::Kind::timed_out, "no whole answer within the deadline"});
    });
    _stream->send(std::move(request));
    _parser.emplace();
    _parser->body_limit(Client::max_answer);
    bhttp::async_read(*_stream, _buffer, *_parser, [self = shared_from_this()](const boost::system::error_code& ec, size_t) {
      if (ec) return self->fail({runtime::Error::Kind::malformed, "the answer is not HTTP within its limits: " + ec.message()});
      auto message = self->_parser->release();
      Response response;
      response.status = message.result_int();
      boost::system::error_code failed;
      json::parse_options options;
      options.max_depth = 64;
      response.body = json::parse(message.body(), failed, json::storage_ptr(), options);
      if (failed) return self->fail({runtime::Error::Kind::malformed, "the answer's body is not JSON"});
      self->finish(std::move(response));
    });
  }

  void fail(runtime::Error error) {
    if (!_done) return;
    _stream->abort();
    tell(runtime::Result<Response>(std::move(error)));
  }

 private:
  void finish(Response response) {
    if (!_done) return;
    _stream->close();
    tell(runtime::Result<Response>(std::move(response)));
  }

  void tell(runtime::Result<Response> result) {
    _timer.cancel();
    asio::post(_executor, [done = std::move(_done), result = std::move(result)]() mutable { done(std::move(result)); });
    _done = nullptr;
  }

  std::shared_ptr<dannan::listen::Stream> _stream;
  asio::steady_timer _timer;
  Clock::time_point _until;
  runtime::Executor _executor;
  runtime::Completion<Response> _done;
  beast::flat_buffer _buffer;
  std::optional<bhttp::response_parser<bhttp::string_body>> _parser;
};

uint16_t port_of(std::string_view text) {
  if (text.empty() || text.size() > 5) throw Invalid("the endpoint's port is not 1 to 65535");
  uint32_t n = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') throw Invalid("the endpoint's port is not 1 to 65535");
    n = n * 10 + static_cast<uint32_t>(c - '0');
  }
  if (n == 0 || n > 65535) throw Invalid("the endpoint's port is not 1 to 65535");
  return static_cast<uint16_t>(n);
}

dannan::listen::ConnectorConfig connector_config() {
  dannan::listen::ConnectorConfig config;
  config.name = "louhi-api";
  return config;
}

}  // namespace

Endpoint parse_endpoint(std::string_view url) {
  Endpoint out;
  if (url.substr(0, 8) == "https://") {
    out.tls = true;
    url.remove_prefix(8);
  } else if (url.substr(0, 7) == "http://") {
    url.remove_prefix(7);
  } else {
    throw Invalid("the endpoint is not an http:// or https:// URL");
  }
  if (!url.empty() && url.back() == '/') url.remove_suffix(1);
  if (url.find('/') != std::string_view::npos || url.find('?') != std::string_view::npos || url.find('@') != std::string_view::npos)
    throw Invalid("the endpoint has a path, a query or a user; it is a host and a port only");
  std::string_view port;
  if (!url.empty() && url.front() == '[') {
    const auto close = url.find(']');
    if (close == std::string_view::npos) throw Invalid("the endpoint's IPv6 host has no closing bracket");
    out.host = std::string(url.substr(1, close - 1));
    url.remove_prefix(close + 1);
    if (!url.empty()) {
      if (url.front() != ':') throw Invalid("the endpoint has something after its host that is not a port");
      port = url.substr(1);
    }
  } else {
    const auto colon = url.rfind(':');
    out.host = std::string(url.substr(0, colon));
    if (colon != std::string_view::npos) port = url.substr(colon + 1);
  }
  if (out.host.empty()) throw Invalid("the endpoint has no host");
  out.port = port.empty() ? (out.tls ? 443 : 80) : port_of(port);
  return out;
}

Client::Client(asio::io_context& io, Endpoint endpoint, std::shared_ptr<const dannan::listen::ClientTrust> trust)
    : _io(io), _endpoint(std::move(endpoint)), _trust(std::move(trust)), _connector(io, connector_config()) {
  if (_endpoint.tls && !_trust) throw std::invalid_argument("an https endpoint needs the roots its certificate chains to");
}

Client::~Client() { _connector.close_all(); }

void Client::request(std::string method, std::string target, std::string body, std::string token, runtime::Deadline deadline, runtime::Executor executor,
                     runtime::Completion<Response> done) {
  const auto until = Clock::now() + deadline;
  std::string host_header = _endpoint.host.find(':') != std::string::npos ? "[" + _endpoint.host + "]" : _endpoint.host;
  std::string request = method + " " + target + " HTTP/1.1\r\nHost: " + host_header + ":" + std::to_string(_endpoint.port) + "\r\nAuthorization: Bearer " +
                        token + "\r\nAccept: application/json\r\nConnection: close\r\n";
  if (!body.empty()) request += "Content-Type: application/json\r\n";
  request += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;

  std::optional<dannan::listen::ClientTls> tls;
  if (_endpoint.tls) {
    tls.emplace();
    tls->trust = _trust;
    tls->alpn = {"http/1.1"};
  }
  _connector.connect(_io.get_executor(), _endpoint.host, _endpoint.port, std::move(tls), deadline,
                     [until, executor = std::move(executor), done = std::move(done), request = std::move(request)](
                         std::shared_ptr<dannan::listen::Stream> stream, std::string why) mutable {
                       if (!stream) {
                         return asio::post(executor, [done = std::move(done), why = std::move(why)] {
                           done(runtime::Error{runtime::Error::Kind::unavailable, "connecting: " + why});
                         });
                       }
                       auto call = std::make_shared<Call>(stream, until, std::move(executor), std::move(done));
                       asio::post(stream->get_executor(), [call, request = std::move(request)]() mutable { call->start(std::move(request)); });
                     });
}

}  // namespace louhi::api
