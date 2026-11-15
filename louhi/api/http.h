//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/io_context.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "dannan/listen/certificates.h"
#include "dannan/status/log.h"
#include "louhi/api/api.h"
#include "louhi/api/auth.h"
#include "louhi/api/json.h"

// The API over HTTP/1.1 (design, "API and sign-on"), on Dannan's public
// listeners: plain HTTP, and HTTPS on its own port, each the operator's choice
// (the maintainer, 2026-11-15). One request per connection: the request is read whole
// within the read deadline and the body limit, signed in from its
// Authorization header, answered by the Api, and the connection closed once
// the answer is written. HTTP is Louhi's own, as the stack settled.
namespace louhi::api {

struct HttpListen {
  std::string host = "0.0.0.0";
  // Zero: any; the server's port() says which.
  uint16_t port = 0;
};

struct HttpsListen : HttpListen {
  dannan::listen::CertificatePaths certificates;
};

struct HttpConfig {
  // Each optional; at least one.
  std::optional<HttpListen> plain;
  std::optional<HttpsListen> tls;
  size_t maximum_body = max_body;
  size_t maximum_header = 16 * 1024;
  // A request not read whole within this is closed unanswered.
  std::chrono::milliseconds read_timeout{30000};
  // At most this many connections open at once on each listener.
  size_t maximum_connections = 256;
};

// The path and its query, the query's names and values %XX-decoded (Dannan's
// strict decoder: '+' stays '+'). Throws Invalid for a malformed escape or a
// name given twice.
struct Target {
  std::string path;
  std::map<std::string, std::string> query;
};
Target parse_target(std::string_view target);

class HttpServer {
 public:
  // The Api and the authenticator outlive the server.
  HttpServer(boost::asio::io_context& io, Api& api, const Authenticator& authenticator, HttpConfig config, dannan::status::ScopedLogger logger);
  ~HttpServer();
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  // Binds and listens. Throws std::invalid_argument for no listener,
  // std::system_error when it cannot bind, std::runtime_error for certificates
  // it cannot load.
  void start();
  // Stops accepting and closes every connection; a request in flight is not
  // answered. Never from an io thread.
  void stop();
  uint16_t port() const noexcept;
  // Zero without HTTPS.
  uint16_t tls_port() const noexcept;

 private:
  friend class Exchange;
  struct State;
  std::shared_ptr<State> _state;
};

}  // namespace louhi::api
