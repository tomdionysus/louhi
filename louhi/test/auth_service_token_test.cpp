//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// This node's own token from Lugh (auth/service_token.h; design, "Events"):
// - an identity that cannot sign in by private_key_jwt or client_secret_basic
//   is refused when the token is made, not later inside a handler;
// - the issuer's discovery is read once, then the token fetched by the
//   client-credentials grant, proven by a Basic secret or a signed assertion;
// - nothing is held before the first token, and the newest one after;
// - it is fetched again at two thirds of its life, never sooner than ten
//   seconds;
// - a failure of any kind keeps the token held and is retried after 5 s,
//   doubling to 5 min, and a success starts the backoff over;
// - after stop() nothing is asked of the issuer and a late answer is
//   dropped; a token gone is not fetched by a timer still pending.
// Driven by a fake issuer and stepped time: no sleep anywhere.

#include <gtest/gtest.h>

#include <algorithm>
#include <boost/asio/io_context.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "dannan/test/fixtures/stepped_time.h"
#include "louhi/auth/service_token.h"
#include "louhi/test/fixtures/fake_https.h"
#include "louhi/test/fixtures/timers.h"

using namespace louhi;
namespace asio = boost::asio;
namespace json = boost::json;
using std::chrono::milliseconds;
using std::chrono::seconds;
using Method = lugh::client::ClientIdentity::Method;

namespace {

constexpr const char* issuer = "https://lugh.example";
const std::string discovery_url = std::string(issuer) + "/.well-known/openid-configuration";
const std::string token_url = std::string(issuer) + "/token";

std::shared_ptr<const dannan::identity::Secret> secret(const std::string& text) {
  return std::make_shared<const dannan::identity::Secret>(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

lugh::client::ClientIdentity basic_identity() {
  lugh::client::ClientIdentity identity;
  identity.client_id = "louhi-node";
  identity.method = Method::client_secret_basic;
  identity.secret = secret("s3cret");
  return identity;
}

json::object token_answer(const std::string& token, uint32_t expires_in) {
  return {{"access_token", token}, {"token_type", "Bearer"}, {"expires_in", expires_in}};
}

struct ServiceTokenTest : ::testing::Test {
  asio::io_context io;
  test::FakeHttps https;
  dannan::test::SteppedTime time;
  dannan::test::SteppedWall wall;
  test::Timers timers{time};
  std::shared_ptr<auth::ServiceToken> token;

  std::shared_ptr<auth::ServiceToken> make(lugh::client::ClientIdentity identity, std::vector<std::string> scopes = {}) {
    auth::ServiceTokenConfig config;
    config.issuer = issuer;
    config.identity = std::move(identity);
    config.scopes = std::move(scopes);
    return auth::ServiceToken::make(
        https, io.get_executor(), [this](milliseconds delay, std::function<void()> fn) { timers.after(delay, std::move(fn)); }, std::move(config), wall,
        dannan::status::ScopedLogger(dannan::status::null_sink(), "token"));
  }
  void settle() {
    io.restart();
    while (io.poll() > 0) {
    }
  }
  void start(lugh::client::ClientIdentity identity = basic_identity(), std::vector<std::string> scopes = {}) {
    token = make(std::move(identity), std::move(scopes));
    token->start();
    settle();
  }
  void advance(milliseconds by) {
    time.advance(by);
    wall.advance(by);
    timers.fire_due();
    settle();
  }
  // The request waiting now is for `url`; answered with `body`.
  void answer(const std::string& url, unsigned status, const json::object& body) {
    const auto next = https.next();
    ASSERT_TRUE(next) << "nothing was asked of the issuer";
    EXPECT_EQ(next->url, url);
    https.answer_json(status, body);
    settle();
  }
  // Started, its discovery read and its first token taken.
  void first_token(const std::string& value = "t1", uint32_t expires_in = 300) {
    start();
    answer(discovery_url, 200, test::discovery(issuer));
    answer(token_url, 200, token_answer(value, expires_in));
    ASSERT_EQ(token->current(), value);
  }
  size_t asked(const std::string& url) {
    size_t n = 0;
    for (const auto& sent : https.sent())
      if (sent.request.url == url) ++n;
    return n;
  }
  // Nothing waits until `delay` has passed, and then the token is asked for.
  void asks_after(milliseconds delay) {
    advance(delay - milliseconds(1));
    EXPECT_EQ(https.pending(), 0u) << "asked before " << delay.count() << " ms";
    advance(milliseconds(1));
    ASSERT_EQ(https.pending(), 1u) << "not asked at " << delay.count() << " ms";
  }
};

}  // namespace

TEST_F(ServiceTokenTest, AnIdentityThatCannotSignInIsRefusedWhenMade) {
  auto no_id = basic_identity();
  no_id.client_id.clear();
  EXPECT_THROW(make(no_id), std::invalid_argument);

  auto none = basic_identity();
  none.method = Method::none;
  EXPECT_THROW(make(none), std::invalid_argument);

  auto certificate = basic_identity();
  certificate.method = Method::tls_client_auth;
  EXPECT_THROW(make(certificate), std::invalid_argument) << "the token's HTTPS presents no certificate";

  auto no_secret = basic_identity();
  no_secret.secret.reset();
  EXPECT_THROW(make(no_secret), std::invalid_argument);

  lugh::client::ClientIdentity jwt;
  jwt.client_id = "louhi-node";
  jwt.method = Method::private_key_jwt;
  EXPECT_THROW(make(jwt), std::invalid_argument) << "no key";
  jwt.key = std::make_shared<const dannan::identity::SigningKey>(dannan::identity::SigningKey::generate(dannan::identity::SigningAlgorithm::es256));
  EXPECT_THROW(make(jwt), std::invalid_argument) << "no kid";
  jwt.kid = "k1";
  EXPECT_NO_THROW(make(jwt));
  EXPECT_NO_THROW(make(basic_identity()));
}

TEST_F(ServiceTokenTest, ReadsDiscoveryThenTakesATokenByClientCredentials) {
  start(basic_identity(), {"lir:pub:louhi/#", "lir:sub:louhi/#"});
  EXPECT_FALSE(token->current()) << "nothing before the first token";

  const auto discovery = https.next();
  ASSERT_TRUE(discovery);
  EXPECT_EQ(discovery->method, "GET");
  answer(discovery_url, 200, test::discovery(issuer));
  EXPECT_FALSE(token->current());

  const auto request = https.next();
  ASSERT_TRUE(request);
  EXPECT_EQ(request->method, "POST");
  EXPECT_EQ(request->url, token_url);
  EXPECT_EQ(test::header(*request, "Authorization"), "Basic bG91aGktbm9kZTpzM2NyZXQ=");
  const auto form = test::form_fields(request->body);
  EXPECT_EQ(form.at("grant_type"), "client_credentials");
  EXPECT_EQ(form.at("scope"), "lir:pub:louhi/# lir:sub:louhi/#");
  EXPECT_EQ(form.count("client_secret"), 0u) << "the secret goes only in the header";

  answer(token_url, 200, token_answer("t1", 300));
  EXPECT_EQ(token->current(), "t1");
}

TEST_F(ServiceTokenTest, NoScopesAsksForEveryScopeTheClientIsGranted) {
  start();
  answer(discovery_url, 200, test::discovery(issuer));
  const auto request = https.next();
  ASSERT_TRUE(request);
  EXPECT_EQ(test::form_fields(request->body).count("scope"), 0u);
}

TEST_F(ServiceTokenTest, PrivateKeyJwtProvesItselfWithASignedAssertion) {
  lugh::client::ClientIdentity identity;
  identity.client_id = "louhi-node";
  identity.method = Method::private_key_jwt;
  identity.key = std::make_shared<const dannan::identity::SigningKey>(dannan::identity::SigningKey::generate(dannan::identity::SigningAlgorithm::es256));
  identity.kid = "k1";
  start(identity);
  answer(discovery_url, 200, test::discovery(issuer));

  const auto request = https.next();
  ASSERT_TRUE(request);
  EXPECT_EQ(test::header(*request, "Authorization"), "");
  const auto form = test::form_fields(request->body);
  EXPECT_EQ(form.at("client_id"), "louhi-node");
  EXPECT_EQ(form.at("client_assertion_type"), "urn:ietf:params:oauth:client-assertion-type:jwt-bearer");
  const auto& assertion = form.at("client_assertion");
  EXPECT_EQ(std::count(assertion.begin(), assertion.end(), '.'), 2) << "a compact JWS";
  answer(token_url, 200, token_answer("t1", 300));
  EXPECT_EQ(token->current(), "t1");
}

TEST_F(ServiceTokenTest, IsFetchedAgainAtTwoThirdsOfItsLifeWithoutReadingDiscoveryAgain) {
  first_token("t1", 300);
  asks_after(seconds(200));
  EXPECT_EQ(token->current(), "t1") << "the token held stands while the next is fetched";
  answer(token_url, 200, token_answer("t2", 600));
  EXPECT_EQ(token->current(), "t2");
  asks_after(seconds(400));
  EXPECT_EQ(asked(discovery_url), 1u);
}

TEST_F(ServiceTokenTest, IsNeverFetchedAgainSoonerThanTenSeconds) {
  first_token("t1", 3);
  asks_after(seconds(10));
  answer(token_url, 200, token_answer("t2", 0));
  asks_after(seconds(10));
}

TEST_F(ServiceTokenTest, AFailureKeepsTheTokenAndBacksOffDoublingToFiveMinutes) {
  first_token("t1", 300);
  asks_after(seconds(200));
  // Every kind of failure: no answer, a refusal, an answer with no token.
  const auto fail = [&](int which) {
    const auto held = token->current();
    switch (which % 3) {
      case 0:
        https.fail("connection refused");
        break;
      case 1:
        https.answer_json(400, {{"error", "invalid_client"}});
        break;
      default:
        https.answer_json(200, {{"token_type", "Bearer"}, {"expires_in", 300}});
        break;
    }
    settle();
    EXPECT_EQ(token->current(), held) << "a failure keeps the token held";
  };
  fail(0);
  int n = 1;
  for (const int s : {5, 10, 20, 40, 80, 160, 300, 300}) {
    asks_after(seconds(s));
    fail(n++);
  }
  // A success starts the backoff over.
  asks_after(seconds(300));
  answer(token_url, 200, token_answer("t2", 300));
  EXPECT_EQ(token->current(), "t2");
  asks_after(seconds(200));
  fail(0);
  asks_after(seconds(5));
}

TEST_F(ServiceTokenTest, ADiscoveryThatFailsIsRetriedUntilItIsTheIssuers) {
  start();
  answer(discovery_url, 500, {});
  asks_after(seconds(5));
  https.answer(200, "not json");
  settle();
  asks_after(seconds(10));
  answer(discovery_url, 200, test::discovery("https://another.example"));
  asks_after(seconds(20));
  answer(discovery_url, 200, test::discovery(issuer));
  answer(token_url, 200, token_answer("t1", 300));
  EXPECT_EQ(token->current(), "t1");
  EXPECT_EQ(asked(discovery_url), 4u);
}

TEST_F(ServiceTokenTest, AfterStopNothingIsAskedAndALateAnswerIsDropped) {
  start();
  answer(discovery_url, 200, test::discovery(issuer));
  ASSERT_EQ(https.pending(), 1u);
  token->stop();
  settle();
  https.answer_json(200, token_answer("t1", 300));
  settle();
  EXPECT_FALSE(token->current()) << "an answer after stop() is dropped";
  advance(std::chrono::hours(1));
  EXPECT_EQ(https.sent().size(), 2u);
}

TEST_F(ServiceTokenTest, AfterStopAPendingRefreshAsksNothing) {
  first_token("t1", 300);
  token->stop();
  settle();
  advance(std::chrono::hours(1));
  EXPECT_EQ(https.pending(), 0u);
  EXPECT_EQ(token->current(), "t1");
}

TEST_F(ServiceTokenTest, ATokenGoneIsNotFetchedByATimerStillPending) {
  first_token("t1", 300);
  std::weak_ptr<auth::ServiceToken> weak = token;
  token.reset();
  settle();
  EXPECT_TRUE(weak.expired()) << "a pending timer keeps nothing alive";
  advance(std::chrono::hours(1));
  EXPECT_EQ(https.pending(), 0u);
}
