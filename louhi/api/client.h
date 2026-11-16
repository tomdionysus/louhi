//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/io_context.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "dannan/listen/connector.h"
#include "louhi/api/api.h"
#include "louhi/runtime/driver.h"

// A client of the API (design, "API and sign-on"), for louhi-ctl and tests:
// one request per connection over Dannan's connector, plain HTTP or HTTPS
// with the server's certificate and name always checked. The answer is
// untrusted input: bounded, and JSON or refused.
namespace louhi::api {

struct Endpoint {
  bool tls = false;
  std::string host;
  uint16_t port = 0;
};

// "http://HOST:PORT" or "https://HOST:PORT" (an IPv6 host in brackets), with
// no path; the port defaults to 80 or 443. Throws Invalid otherwise.
Endpoint parse_endpoint(std::string_view url);

// What asks the API, so what calls it is tested against a fake.
class Requester {
 public:
  virtual ~Requester() = default;
  // Sends `method target` with `body` (JSON, or empty) and the token as a
  // Bearer, and completes with the server's status and its JSON body: on
  // `executor`, once, never inline, by `deadline` (the whole exchange).
  // Fails unavailable when the server cannot be reached, timed_out, or
  // malformed for an answer that is not HTTP with a JSON body.
  virtual void request(std::string method, std::string target, std::string body, std::string token, runtime::Deadline deadline, runtime::Executor executor,
                       runtime::Completion<Response> done) = 0;
};

class Client final : public Requester {
 public:
  // The largest answer read.
  static constexpr size_t max_answer = 16u << 20;

  // `trust` is required for an https endpoint. The io context outlives this.
  Client(boost::asio::io_context& io, Endpoint endpoint, std::shared_ptr<const dannan::listen::ClientTrust> trust = nullptr);
  ~Client() override;
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  void request(std::string method, std::string target, std::string body, std::string token, runtime::Deadline deadline, runtime::Executor executor,
               runtime::Completion<Response> done) override;

 private:
  boost::asio::io_context& _io;
  const Endpoint _endpoint;
  std::shared_ptr<const dannan::listen::ClientTrust> _trust;
  dannan::listen::StreamConnector _connector;
};

}  // namespace louhi::api
