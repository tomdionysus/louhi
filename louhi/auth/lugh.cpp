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

namespace {

lugh::client::RelyingParty::Settings settings(LughConfig config) {
  lugh::client::RelyingParty::Settings out;
  out.issuers = std::move(config.issuers);
  out.audience = "louhi";
  out.ca_path = std::move(config.ca_path);
  out.keys = config.keys;
  return out;
}

}  // namespace

LughPlugin::LughPlugin(boost::asio::io_context& io, LughConfig config, const dannan::contract::TimeSource& time, const dannan::contract::WallClock& wall)
    : _party(io, settings(std::move(config)), time, wall), _adapter(_party.verifier()) {}

LughPlugin::~LughPlugin() { stop(); }

void LughPlugin::start() { _party.start(); }

void LughPlugin::stop() { _party.stop(); }

}  // namespace louhi::auth
