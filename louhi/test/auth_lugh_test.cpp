//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The Lugh plugin (auth/lugh.h) with real tokens, signed as a Lugh issuer
// signs them (lugh/client/testing.h): a token for "louhi" from a trusted
// issuer signs in with its scopes; one for another audience, from a key the
// issuer does not hold, past its exp, or bound to a key or certificate, does
// not.

#include <gtest/gtest.h>

#include <string>

#include "dannan/test/fixtures/stepped_time.h"
#include "louhi/api/auth.h"
#include "louhi/auth/lugh.h"
#include "lugh/client/testing.h"
#include "lugh/client/verifier.h"

using namespace louhi;
// GoogleTest has a ::testing of its own.
namespace lt = lugh::client::testing;

namespace {

constexpr uint64_t now_unix = 1'800'000'000;
constexpr const char* issuer = "https://lugh.example";

struct LughSignin : ::testing::Test {
  dannan::test::SteppedWall wall{now_unix * 1000};
  lugh::client::Verifier lugh_verifier{{issuer}, wall};
  auth::LughVerifier verifier{lugh_verifier};
  api::Authenticator authenticator{verifier, wall};
  dannan::identity::SigningKey key = dannan::identity::SigningKey::generate(dannan::identity::SigningAlgorithm::es256);

  LughSignin() { lugh_verifier.set_keys(issuer, lt::jwks(key, "k1")); }

  boost::json::object claims(boost::json::value audience = "louhi") {
    return {{"iss", issuer},
            {"sub", "alice"},
            {"aud", std::move(audience)},
            {"client_id", "louhi-ctl"},
            {"exp", now_unix + 300},
            {"jti", "j1"},
            {"scope", "openid louhi:web:run louhi:web:read lir:sub:x"}};
  }

  std::variant<api::Grant, std::string> sign_in(const std::string& token) { return authenticator.authenticate("Bearer " + token); }
};

}  // namespace

TEST_F(LughSignin, ATokenForLouhiSignsInWithItsScopes) {
  auto r = sign_in(lt::sign_access_token(key, "k1", claims()));
  ASSERT_TRUE(std::holds_alternative<api::Grant>(r)) << std::get<std::string>(r);
  const auto& grant = std::get<api::Grant>(r);
  EXPECT_EQ(grant.subject(), "alice");
  EXPECT_TRUE(grant.may("web", api::Action::run));
  EXPECT_FALSE(grant.may("web", api::Action::register_definitions));
  EXPECT_EQ(grant.expires_unix(), now_unix + 300);
}

TEST_F(LughSignin, OneAudienceAmongSeveralIsEnough) {
  auto r = sign_in(lt::sign_access_token(key, "k1", claims(boost::json::array{"lir", "louhi"})));
  EXPECT_TRUE(std::holds_alternative<api::Grant>(r));
}

TEST_F(LughSignin, ATokenForAnotherAudienceIsRefused) {
  EXPECT_TRUE(std::holds_alternative<std::string>(sign_in(lt::sign_access_token(key, "k1", claims("lir")))));
}

TEST_F(LughSignin, ATokenSignedByAKeyTheIssuerDoesNotHoldIsRefused) {
  const auto other = dannan::identity::SigningKey::generate(dannan::identity::SigningAlgorithm::es256);
  EXPECT_TRUE(std::holds_alternative<std::string>(sign_in(lt::sign_access_token(other, "k1", claims()))));
  EXPECT_TRUE(std::holds_alternative<std::string>(sign_in(lt::sign_access_token(other, "k2", claims()))));
}

TEST_F(LughSignin, ATokenPastItsExpIsRefused) {
  const auto token = lt::sign_access_token(key, "k1", claims());
  wall.advance(std::chrono::seconds(300));
  EXPECT_TRUE(std::holds_alternative<std::string>(sign_in(token))) << "Lugh's skew passes it; Louhi's grant has ended";
}

TEST_F(LughSignin, ABoundTokenIsRefused) {
  auto bound = claims();
  bound["cnf"] = boost::json::object{{"jkt", "0ZcOCORZNYy-DWpqq30jZyJGHTN0d2HglBV3uiguA4I"}};
  EXPECT_TRUE(std::holds_alternative<std::string>(sign_in(lt::sign_access_token(key, "k1", bound))));
}

TEST_F(LughSignin, ATamperedTokenIsRefused) {
  auto token = lt::sign_access_token(key, "k1", claims());
  token[token.size() / 2] = token[token.size() / 2] == 'A' ? 'B' : 'A';
  EXPECT_TRUE(std::holds_alternative<std::string>(sign_in(token)));
}
