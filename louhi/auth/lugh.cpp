//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/auth/lugh.h"

namespace louhi::auth {

std::variant<api::Claims, std::string> LughVerifier::verify(std::string_view token) const {
  // Lugh checks the audience too; the Authenticator checks it again, so a
  // verifier of another kind is held to the same.
  auto verified = _verifier.verify_access(token, _audience);
  if (auto* why = std::get_if<std::string>(&verified)) return std::move(*why);
  auto& access = std::get<lugh::client::AccessToken>(verified);
  api::Claims claims;
  claims.subject = std::move(access.subject);
  claims.audiences = std::move(access.audiences);
  claims.scopes = std::move(access.scopes);
  claims.expires_unix = access.expires;
  claims.bound = access.jkt.has_value() || access.x5t.has_value();
  return claims;
}

LughPlugin::LughPlugin(boost::asio::io_context& io, LughConfig config, const dannan::contract::TimeSource& time, const dannan::contract::WallClock& wall)
    : _https(io, config.ca_path),
      _verifier(std::move(config.issuers), wall),
      _keys(lugh::client::HttpsKeySource::make(_verifier, _https, io.get_executor(), time, config.keys)),
      _adapter(_verifier) {}

LughPlugin::~LughPlugin() { stop(); }

void LughPlugin::start() { _keys->start(); }

void LughPlugin::stop() { _keys->stop(); }

}  // namespace louhi::auth
