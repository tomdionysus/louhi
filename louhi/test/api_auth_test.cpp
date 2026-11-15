//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Sign-in and the scope check (design, "API and sign-on"): a Bearer token the
// verifier accepts, with "louhi" among its audiences, not bound, before its
// exp; "louhi:admin" grants everything, "louhi:<project>:<action>" one action
// in one project and "louhi:<project>:*" every action there; a scope that does
// not parse, or is another application's, grants nothing.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "dannan/test/fixtures/stepped_time.h"
#include "louhi/api/auth.h"

using namespace louhi;
using api::Action;

namespace {

constexpr uint64_t now_unix = 1'800'000'000;

// Accepts the token "good" with the claims it holds; refuses any other.
class FakeVerifier final : public api::TokenVerifier {
 public:
  api::Claims claims{"alice", {"louhi"}, {"louhi:web:read"}, now_unix + 300, false};
  mutable std::string last;
  std::variant<api::Claims, std::string> verify(std::string_view token) const override {
    last = std::string(token);
    if (token == "good") return claims;
    return std::string("not signed by a trusted issuer");
  }
};

struct Signin : ::testing::Test {
  dannan::test::SteppedWall wall{now_unix * 1000};
  FakeVerifier verifier;
  api::Authenticator authenticator{verifier, wall};

  std::string refusal(std::string_view header) {
    auto r = authenticator.authenticate(header);
    if (std::holds_alternative<api::Grant>(r)) return "";
    return std::get<std::string>(r);
  }
};

}  // namespace

TEST_F(Signin, AGoodBearerTokenGivesItsGrant) {
  auto r = authenticator.authenticate("Bearer good");
  ASSERT_TRUE(std::holds_alternative<api::Grant>(r));
  const auto& grant = std::get<api::Grant>(r);
  EXPECT_EQ(grant.subject(), "alice");
  EXPECT_TRUE(grant.may("web", Action::read));
  EXPECT_FALSE(grant.may("web", Action::run));
  EXPECT_EQ(grant.expires_unix(), now_unix + 300);
}

TEST_F(Signin, TheSchemeIsReadInAnyCase) {
  EXPECT_EQ(refusal("bearer good"), "");
  EXPECT_EQ(refusal("BEARER good"), "");
}

TEST_F(Signin, NoAnonymousRequestIsAnswered) {
  EXPECT_NE(refusal(""), "");
  EXPECT_NE(refusal("Basic YWxpY2U6cHc="), "") << "only a Bearer token signs in";
  EXPECT_NE(refusal("Bearer"), "");
  EXPECT_NE(refusal("Bearer    "), "");
}

TEST_F(Signin, TheVerifiersRefusalIsTheAnswer) { EXPECT_EQ(refusal("Bearer forged"), "not signed by a trusted issuer"); }

TEST_F(Signin, ATokenForAnotherAudienceIsRefused) {
  verifier.claims.audiences = {"lir"};
  EXPECT_NE(refusal("Bearer good"), "");
}

TEST_F(Signin, ABoundTokenIsRefusedSinceItsBindingCannotBeChecked) {
  verifier.claims.bound = true;
  EXPECT_NE(refusal("Bearer good"), "");
}

TEST_F(Signin, ATokenAtOrPastItsExpIsRefused) {
  wall.advance(std::chrono::seconds(299));
  EXPECT_EQ(refusal("Bearer good"), "");
  wall.advance(std::chrono::seconds(1));
  EXPECT_NE(refusal("Bearer good"), "") << "a grant lasts until exp, not past it";
}

TEST_F(Signin, AnOverlongTokenIsNotHandedToTheVerifier) {
  EXPECT_NE(refusal("Bearer " + std::string(20000, 'x')), "");
  EXPECT_EQ(verifier.last, "");
}

TEST(Grant, AdminGrantsEverything) {
  const std::vector<std::string> scopes{"louhi:admin"};
  api::Grant grant("root", scopes, 0);
  EXPECT_TRUE(grant.admin());
  EXPECT_TRUE(grant.may("any", Action::run));
  EXPECT_TRUE(grant.holds("any"));
}

TEST(Grant, EachActionGrantsOnlyItselfInItsProject) {
  const std::vector<std::string> scopes{"louhi:web:register", "louhi:batch:run"};
  api::Grant grant("bob", scopes, 0);
  EXPECT_FALSE(grant.admin());
  EXPECT_TRUE(grant.may("web", Action::register_definitions));
  EXPECT_FALSE(grant.may("web", Action::read)) << "register does not imply read";
  EXPECT_FALSE(grant.may("web", Action::run));
  EXPECT_TRUE(grant.may("batch", Action::run));
  EXPECT_FALSE(grant.may("other", Action::read));
  EXPECT_EQ(grant.projects(), (std::vector<std::string>{"batch", "web"}));
}

TEST(Grant, AStarGrantsEveryActionInOneProject) {
  const std::vector<std::string> scopes{"louhi:web:*"};
  api::Grant grant("bob", scopes, 0);
  EXPECT_TRUE(grant.may("web", Action::read));
  EXPECT_TRUE(grant.may("web", Action::register_definitions));
  EXPECT_TRUE(grant.may("web", Action::run));
  EXPECT_FALSE(grant.may("batch", Action::read));
  EXPECT_FALSE(grant.admin());
}

TEST(Grant, ScopesThatDoNotParseGrantNothing) {
  const std::vector<std::string> scopes{"louhi:Web:read",  "louhi:web:delete", "louhi:web",           "louhi:*:read", "louhi:admin:x",
                                        "lir:pub:louhi/#", "LOUHI:admin",      "louhi:web:read:more", "louhi::read",  "louhi:-web:read"};
  api::Grant grant("eve", scopes, 0);
  EXPECT_FALSE(grant.admin());
  EXPECT_TRUE(grant.projects().empty());
}
