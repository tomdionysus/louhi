//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/api/http.h"

#include <atomic>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <stdexcept>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/encoding/percent.h"
#include "dannan/listen/listener.h"

namespace louhi::api {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = beast::http;

const char* status_text(unsigned status) {
  switch (status) {
    case 200:
      return "OK";
    case 400:
      return "Bad Request";
    case 401:
      return "Unauthorized";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 409:
      return "Conflict";
    case 413:
      return "Payload Too Large";
    case 431:
      return "Request Header Fields Too Large";
    case 500:
      return "Internal Server Error";
    default:
      return "Service Unavailable";
  }
}

std::string decoded(std::string_view text) {
  try {
    return dannan::encoding::percent_decode(text);
  } catch (const std::invalid_argument&) {
    throw Invalid("the query holds a malformed escape");
  }
}

}  // namespace

Target parse_target(std::string_view target) {
  Target out;
  const size_t question = target.find('?');
  out.path = std::string(target.substr(0, question));
  if (question == std::string_view::npos) return out;
  std::string_view rest = target.substr(question + 1);
  while (!rest.empty()) {
    const size_t amp = rest.find('&');
    const auto pair = rest.substr(0, amp);
    if (!pair.empty()) {
      const size_t equals = pair.find('=');
      std::string name = decoded(pair.substr(0, equals));
      std::string value = equals == std::string_view::npos ? "" : decoded(pair.substr(equals + 1));
      if (!out.query.emplace(std::move(name), std::move(value)).second) throw Invalid("the query names a parameter twice");
    }
    if (amp == std::string_view::npos) break;
    rest = rest.substr(amp + 1);
  }
  return out;
}

class Exchange;

struct HttpServer::State : std::enable_shared_from_this<HttpServer::State> {
  State(asio::io_context& io_context, Api& a, const Authenticator& auth, HttpConfig cfg, dannan::status::ScopedLogger log)
      : io(io_context), api(a), authenticator(auth), config(std::move(cfg)), logger(std::move(log)) {}

  void begin(std::shared_ptr<dannan::listen::Stream> stream);
  std::unique_ptr<dannan::listen::StreamListener> listen(const std::string& name, const HttpListen& where,
                                                         std::shared_ptr<dannan::listen::Certificates> certificates);
  void forget(Exchange* gone) {
    dannan::contract::Lock lock(mutex);
    exchanges.erase(gone);
  }

  asio::io_context& io;
  Api& api;
  const Authenticator& authenticator;
  const HttpConfig config;
  dannan::status::ScopedLogger logger;
  std::unique_ptr<dannan::listen::StreamListener> plain;
  std::unique_ptr<dannan::listen::StreamListener> tls;
  std::atomic<uint16_t> port{0};
  std::atomic<uint16_t> tls_port{0};
  bool started = false;

  dannan::contract::Mutex mutex;
  bool stopped DANNAN_GUARDED_BY(mutex) = false;
  std::map<Exchange*, std::weak_ptr<Exchange>> exchanges DANNAN_GUARDED_BY(mutex);
};

// One connection: one request read whole within the deadline, answered, and
// the connection closed once the answer is written. Everything on the
// stream's strand; the Api's answer is brought back to it.
class Exchange final : public std::enable_shared_from_this<Exchange> {
 public:
  Exchange(std::shared_ptr<dannan::listen::Stream> stream, std::weak_ptr<HttpServer::State> state, HttpConfig config)
      : _stream(std::move(stream)), _state(std::move(state)), _config(config), _timer(_stream->get_executor()) {}

  void start() {
    _parser.emplace();
    _parser->body_limit(_config.maximum_body);
    _parser->header_limit(static_cast<uint32_t>(_config.maximum_header));
    _timer.expires_after(_config.read_timeout);
    _timer.async_wait([weak = weak_from_this()](const boost::system::error_code& ec) {
      if (const auto self = weak.lock(); self && !ec && !self->_read) self->terminate();
    });
    bhttp::async_read(*_stream, _buffer, *_parser, [self = shared_from_this()](const boost::system::error_code& ec, size_t) {
      self->_read = true;
      self->_timer.cancel();
      if (ec == bhttp::error::body_limit) return self->respond(error_response(413, "too_large", "the body is larger than the limit"));
      if (ec == bhttp::error::header_limit) return self->respond(error_response(431, "too_large", "the headers are larger than the limit"));
      if (ec) return self->terminate();
      self->handle(self->_parser->release());
    });
  }

  // Ends it at once, unanswered. On the stream's strand.
  void terminate() {
    if (_done) return;
    _done = true;
    _timer.cancel();
    _stream->abort();
    if (const auto state = _state.lock()) state->forget(this);
  }

 private:
  void handle(bhttp::request<bhttp::string_body> message) {
    std::shared_ptr<HttpServer::State> state = _state.lock();
    if (!state) return terminate();
    Target target;
    try {
      target = parse_target(std::string_view(message.target().data(), message.target().size()));
    } catch (const Invalid& error) {
      return respond(error_response(400, "invalid", error.what()));
    }
    const auto authorization = message[bhttp::field::authorization];
    auto signed_in = state->authenticator.authenticate(std::string_view(authorization.data(), authorization.size()));
    if (auto* why = std::get_if<std::string>(&signed_in)) return respond(error_response(401, "unauthenticated", *why));

    Request request;
    request.method = std::string(message.method_string());
    request.path = std::move(target.path);
    request.query = std::move(target.query);
    request.body = std::move(message.body());
    state->api.handle(std::get<Grant>(signed_in), request, [self = shared_from_this()](Response response) {
      asio::post(self->_stream->get_executor(), [self, response = std::move(response)]() mutable { self->respond(std::move(response)); });
    });
  }

  void respond(Response response) {
    if (_done || _ending) return;
    _ending = true;
    const std::string body = boost::json::serialize(response.body);
    std::string head = "HTTP/1.1 " + std::to_string(response.status) + " " + status_text(response.status) +
                       "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\nCache-Control: no-store\r\n";
    if (response.status == 401) head += "WWW-Authenticate: Bearer realm=\"louhi\"\r\n";
    _stream->send(head + "Connection: close\r\n\r\n" + body);
    // The answer is written, then over TLS close_notify, within the close
    // bound; then the exchange is forgotten.
    _stream->when_finished([self = shared_from_this()] {
      self->_done = true;
      if (const auto state = self->_state.lock()) state->forget(self.get());
    });
    _stream->close();
  }

  std::shared_ptr<dannan::listen::Stream> _stream;
  std::weak_ptr<HttpServer::State> _state;
  // A copy: the server's state may go first.
  const HttpConfig _config;
  asio::steady_timer _timer;
  beast::flat_buffer _buffer;
  std::optional<bhttp::request_parser<bhttp::string_body>> _parser;
  bool _read = false;
  bool _ending = false;
  bool _done = false;
};

void HttpServer::State::begin(std::shared_ptr<dannan::listen::Stream> stream) {
  auto exchange = std::make_shared<Exchange>(std::move(stream), weak_from_this(), config);
  bool refused = false;
  {
    dannan::contract::Lock lock(mutex);
    refused = stopped;
    if (!refused) exchanges.emplace(exchange.get(), exchange);
  }
  if (refused) return exchange->terminate();
  exchange->start();
}

std::unique_ptr<dannan::listen::StreamListener> HttpServer::State::listen(const std::string& name, const HttpListen& where,
                                                                          std::shared_ptr<dannan::listen::Certificates> certificates) {
  dannan::listen::ListenerConfig settings;
  settings.name = name;
  settings.host = where.host;
  settings.port = where.port;
  settings.certificates = std::move(certificates);
  settings.tls.alpn = {"http/1.1"};
  settings.max_connections = config.maximum_connections;
  auto listener = std::make_unique<dannan::listen::StreamListener>(io, std::move(settings));
  listener->start(io.get_executor(), [weak = weak_from_this()](std::shared_ptr<dannan::listen::Stream> stream) {
    if (const auto s = weak.lock()) return s->begin(std::move(stream));
    stream->abort();
  });
  return listener;
}

HttpServer::HttpServer(asio::io_context& io, Api& api, const Authenticator& authenticator, HttpConfig config, dannan::status::ScopedLogger logger)
    : _state(std::make_shared<State>(io, api, authenticator, std::move(config), std::move(logger))) {}

HttpServer::~HttpServer() {
  try {
    stop();
  } catch (const std::exception& e) {
    _state->logger.error(std::string("stopping the API: ") + e.what());
  }
}

void HttpServer::start() {
  auto& s = *_state;
  if (!s.config.plain && !s.config.tls) throw std::invalid_argument("the API has neither an HTTP nor an HTTPS listener");
  if (const auto& plain = s.config.plain) {
    s.plain = s.listen("api-http", *plain, nullptr);
    s.port = s.plain->port();
    s.logger.info("HTTP listening on " + plain->host + ":" + std::to_string(s.port.load()));
  }
  if (const auto& tls = s.config.tls) {
    s.tls = s.listen("api-https", *tls, std::make_shared<dannan::listen::Certificates>(tls->certificates));
    s.tls_port = s.tls->port();
    s.logger.info("HTTPS listening on " + tls->host + ":" + std::to_string(s.tls_port.load()));
  }
  s.started = true;
}

void HttpServer::stop() {
  if (!_state->started) return;
  {
    dannan::contract::Lock lock(_state->mutex);
    if (_state->stopped) return;
    _state->stopped = true;
  }
  for (auto* listener : {_state->plain.get(), _state->tls.get()})
    if (listener) listener->stop_accepting();
  for (auto* listener : {_state->plain.get(), _state->tls.get()})
    if (listener) listener->close_all();
}

uint16_t HttpServer::port() const noexcept { return _state->port.load(); }

uint16_t HttpServer::tls_port() const noexcept { return _state->tls_port.load(); }

}  // namespace louhi::api
