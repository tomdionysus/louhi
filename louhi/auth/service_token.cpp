//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/auth/service_token.h"

#include <algorithm>
#include <boost/asio/post.hpp>
#include <boost/json.hpp>

namespace louhi::auth {

namespace {

namespace asio = boost::asio;
namespace json = boost::json;
using namespace std::chrono_literals;

constexpr std::chrono::milliseconds first_backoff = 5s;
constexpr std::chrono::milliseconds most_backoff = 5min;
constexpr std::chrono::milliseconds least_refresh = 10s;
constexpr std::chrono::milliseconds step_deadline = 30s;

}  // namespace

std::shared_ptr<ServiceToken> ServiceToken::make(asio::io_context& io, ServiceTokenConfig config, const dannan::contract::WallClock& wall,
                                                 dannan::status::ScopedLogger logger) {
  if (config.identity.client_id.empty()) throw std::invalid_argument("the service token needs a client id");
  if (config.identity.method == lugh::client::ClientIdentity::Method::none)
    throw std::invalid_argument("the service token needs a key (private_key_jwt) or a secret (client_secret_basic)");
  return std::shared_ptr<ServiceToken>(new ServiceToken(io, std::move(config), wall, std::move(logger)));
}

ServiceToken::ServiceToken(asio::io_context& io, ServiceTokenConfig config, const dannan::contract::WallClock& wall, dannan::status::ScopedLogger logger)
    : _config(std::move(config)),
      _wall(wall),
      _logger(std::move(logger)),
      _https(io, _config.ca_path),
      _verifier({_config.issuer}, wall),
      _strand(asio::make_strand(io)),
      _timer(_strand) {}

void ServiceToken::start() {
  asio::post(_strand, [self = shared_from_this()] { self->fetch(); });
}

void ServiceToken::stop() {
  asio::post(_strand, [self = shared_from_this()] {
    self->_stopped = true;
    self->_timer.cancel();
  });
}

std::optional<std::string> ServiceToken::current() const {
  dannan::contract::Lock lock(_mutex);
  return _token;
}

void ServiceToken::after(std::chrono::milliseconds delay) {
  if (_stopped) return;
  _timer.expires_after(delay);
  _timer.async_wait([self = shared_from_this()](const boost::system::error_code& ec) {
    if (!ec) self->fetch();
  });
}

void ServiceToken::retry() {
  _backoff = _backoff.count() == 0 ? first_backoff : std::min(_backoff * 2, most_backoff);
  after(_backoff);
}

void ServiceToken::fetch() {
  if (_stopped) return;
  if (!_oauth) {
    lugh::client::HttpsRequest request;
    request.url = _config.issuer + "/.well-known/openid-configuration";
    _https.send(request, step_deadline, _strand, [self = shared_from_this()](lugh::Outcome<lugh::client::HttpsResponse> got) {
      if (self->_stopped) return;
      if (!got.ok() || got.value.status != 200) {
        self->_logger.warn("reading " + self->_config.issuer + "'s discovery: " + (got.ok() ? "it answered " + std::to_string(got.value.status) : got.message));
        return self->retry();
      }
      boost::system::error_code failed;
      const auto document = json::parse(got.value.body, failed);
      if (failed || !document.is_object()) {
        self->_logger.warn(self->_config.issuer + "'s discovery is not a JSON object");
        return self->retry();
      }
      auto endpoints = lugh::client::Endpoints::from_discovery(document.as_object(), self->_config.issuer);
      if (auto* why = std::get_if<std::string>(&endpoints)) {
        self->_logger.warn(self->_config.issuer + "'s discovery: " + *why);
        return self->retry();
      }
      self->_oauth = std::make_unique<lugh::client::OAuthClient>(std::get<lugh::client::Endpoints>(endpoints), self->_config.identity, self->_https,
                                                                 self->_verifier, self->_wall);
      self->fetch();
    });
    return;
  }
  _oauth->client_credentials(_config.scopes, step_deadline, _strand, [self = shared_from_this()](lugh::Outcome<lugh::client::TokenSet> got) {
    if (self->_stopped) return;
    if (!got.ok() || !got.value.access_token) {
      self->_logger.warn("getting this node's token from " + self->_config.issuer + ": " + (got.ok() ? "no access token in the answer" : got.message));
      return self->retry();
    }
    const auto bytes = got.value.access_token->reveal();
    {
      dannan::contract::Lock lock(self->_mutex);
      self->_token = std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    self->_backoff = std::chrono::milliseconds(0);
    const auto life = std::chrono::milliseconds(std::chrono::seconds(got.value.expires_in));
    self->after(std::max(least_refresh, life * 2 / 3));
  });
}

}  // namespace louhi::auth
