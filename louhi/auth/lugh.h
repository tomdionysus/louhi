//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/io_context.hpp>
#include <memory>
#include <string>
#include <vector>

#include "dannan/contract/time_source.h"
#include "louhi/api/auth.h"
#include "lugh/client/key_source.h"
#include "lugh/client/relying_party.h"
#include "lugh/client/verifier.h"

// The Lugh plugin (design, "API and sign-on"; the maintainer, 2026-11-15: sign-in by the
// Lugh plugin only): access tokens checked offline by lugh_client's
// RelyingParty against the keys it fetches from each trusted issuer. Whether
// a token is genuine is Lugh's code; what it grants is Louhi's (api/auth.h).
// Built only with LOUHI_BUILD_LUGH.
namespace louhi::auth {

// api::TokenVerifier over lugh_client's Verifier, asking for Louhi's
// audience. Thread-safe and waits on nothing, as the Verifier is.
class LughVerifier final : public api::TokenVerifier {
 public:
  // The verifier outlives this.
  explicit LughVerifier(const lugh::client::Verifier& verifier, std::string audience = "louhi") : _verifier(verifier), _audience(std::move(audience)) {}
  std::variant<api::Claims, std::string> verify(std::string_view token) const override;

 private:
  const lugh::client::Verifier& _verifier;
  const std::string _audience;
};

struct LughConfig {
  // Each trusted Lugh, as its tokens name it (iss).
  std::vector<std::string> issuers;
  // The roots the issuers' HTTPS certificates chain to; empty: the system's.
  std::string ca_path;
  lugh::client::HttpsKeySource::Settings keys;
};

// The plugin whole: Lugh's RelyingParty for Louhi's audience, and the
// api::TokenVerifier over its Verifier.
class LughPlugin {
 public:
  // The io context and the clocks outlive this. Throws std::invalid_argument
  // for no issuers, std::runtime_error for roots that cannot be read.
  LughPlugin(boost::asio::io_context& io, LughConfig config, const dannan::contract::TimeSource& time, const dannan::contract::WallClock& wall);
  ~LughPlugin();
  LughPlugin(const LughPlugin&) = delete;
  LughPlugin& operator=(const LughPlugin&) = delete;

  // Fetches every issuer's keys now and keeps them fresh until stop(). Until
  // an issuer's keys come, its tokens are refused. Waits on nothing.
  void start();
  void stop();
  const api::TokenVerifier& verifier() const noexcept { return _adapter; }
  std::vector<lugh::client::HttpsKeySource::IssuerStatus> status() const { return _party.status(); }

 private:
  lugh::client::RelyingParty _party;
  LughVerifier _adapter;
};

}  // namespace louhi::auth
