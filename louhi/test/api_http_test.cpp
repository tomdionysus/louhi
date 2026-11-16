//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The API over HTTP (api/http.h): no request is answered without a Bearer
// token the authenticator accepts; a signed-in request is the Api's to
// answer, as JSON, and the connection closes after it; a body over the limit
// is refused whole; a request not read whole within the deadline is closed
// unanswered. The query is %XX-decoded strictly and names each parameter
// once.

#include <gtest/gtest.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/json.hpp>
#include <future>
#include <string>
#include <thread>

#include "dannan/test/fixtures/stepped_time.h"
#include "louhi/api/client.h"
#include "louhi/api/http.h"
#include "louhi/store/memory_store.h"
#include "louhi/test/fixtures/fake_driver.h"
#include "louhi/test/fixtures/records.h"

using namespace louhi;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace {

class OneTokenVerifier final : public api::TokenVerifier {
 public:
  std::variant<api::Claims, std::string> verify(std::string_view token) const override {
    if (token == "admin") return api::Claims{"root", {"louhi"}, {"louhi:admin"}, 4'000'000'000, false};
    return std::string("not a token this test knows");
  }
};

struct HttpTest : ::testing::Test {
  asio::io_context io;
  asio::executor_work_guard<asio::io_context::executor_type> work = asio::make_work_guard(io);
  std::thread runner{[this] { io.run(); }};
  store::MemoryStore store;
  dannan::test::SteppedWall wall;
  OneTokenVerifier verifier;
  api::Authenticator authenticator{verifier, wall};
  test::FakeDriver images;
  api::Api api{store, wall, test::node_id(1), images, io.get_executor(), dannan::status::ScopedLogger(dannan::status::null_sink(), "test")};
  std::unique_ptr<api::HttpServer> server;

  void start(std::chrono::milliseconds read_timeout = std::chrono::milliseconds(30000)) {
    api::HttpConfig config;
    config.plain = api::HttpListen{"127.0.0.1", 0};
    config.maximum_body = 1024;
    config.read_timeout = read_timeout;
    server = std::make_unique<api::HttpServer>(io, api, authenticator, config, dannan::status::ScopedLogger(dannan::status::null_sink(), "test"));
    server->start();
  }

  ~HttpTest() override {
    if (server) server->stop();
    server.reset();
    work.reset();
    io.stop();
    runner.join();
  }

  // Everything the server writes until it closes.
  std::string exchange(const std::string& request) {
    asio::io_context client;
    tcp::socket socket(client);
    socket.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), server->port()));
    asio::write(socket, asio::buffer(request));
    std::string out;
    boost::system::error_code ec;
    asio::read(socket, asio::dynamic_buffer(out), ec);
    return out;
  }

  static std::string get(const std::string& path, const std::string& token) {
    return "GET " + path + " HTTP/1.1\r\nHost: louhi\r\n" + (token.empty() ? "" : "Authorization: Bearer " + token + "\r\n") + "\r\n";
  }

  static unsigned status_of(const std::string& reply) { return reply.size() > 12 ? static_cast<unsigned>(std::stoi(reply.substr(9, 3))) : 0; }

  static boost::json::value body_of(const std::string& reply) { return boost::json::parse(reply.substr(reply.find("\r\n\r\n") + 4)); }
};

}  // namespace

TEST(HttpTarget, TheQueryIsDecodedStrictlyAndNamesEachParameterOnce) {
  const auto t = api::parse_target("/v1/projects/web/tasks?limit=3&next=a%2Bb+c");
  EXPECT_EQ(t.path, "/v1/projects/web/tasks");
  EXPECT_EQ(t.query.at("limit"), "3");
  EXPECT_EQ(t.query.at("next"), "a+b+c") << "'+' stays '+'";
  EXPECT_THROW(api::parse_target("/x?a=%zz"), api::Invalid);
  EXPECT_THROW(api::parse_target("/x?a=1&a=2"), api::Invalid);
  EXPECT_TRUE(api::parse_target("/x").query.empty());
}

TEST_F(HttpTest, NoRequestIsAnsweredWithoutAToken) {
  start();
  for (const std::string token : {"", "forged"}) {
    const auto reply = exchange(get("/v1/projects", token));
    EXPECT_EQ(status_of(reply), 401u) << reply;
    EXPECT_NE(reply.find("WWW-Authenticate: Bearer"), std::string::npos);
    EXPECT_EQ(body_of(reply).at("error"), "unauthenticated");
  }
}

TEST_F(HttpTest, ASignedInRequestIsTheApisToAnswerAndTheConnectionThenCloses) {
  start();
  const std::string body = R"({"description":"The public site"})";
  auto reply = exchange("PUT /v1/projects/web HTTP/1.1\r\nHost: louhi\r\nAuthorization: Bearer admin\r\nContent-Type: application/json\r\nContent-Length: " +
                        std::to_string(body.size()) + "\r\n\r\n" + body);
  EXPECT_EQ(status_of(reply), 200u) << reply;
  EXPECT_NE(reply.find("Connection: close"), std::string::npos);
  EXPECT_NE(reply.find("Content-Type: application/json"), std::string::npos);

  reply = exchange(get("/v1/projects/web", "admin"));
  EXPECT_EQ(status_of(reply), 200u) << reply;
  EXPECT_EQ(body_of(reply).at("description"), "The public site");
  EXPECT_EQ(status_of(exchange(get("/v1/projects/none", "admin"))), 404u);
}

TEST_F(HttpTest, ABodyOverTheLimitIsRefusedWhole) {
  start();
  const std::string body(2048, ' ');
  const auto reply = exchange("PUT /v1/projects/web HTTP/1.1\r\nHost: louhi\r\nAuthorization: Bearer admin\r\nContent-Length: " + std::to_string(body.size()) +
                              "\r\n\r\n" + body);
  EXPECT_EQ(status_of(reply), 413u) << reply;
}

TEST_F(HttpTest, AMalformedQueryIsInvalid) {
  start();
  EXPECT_EQ(status_of(exchange(get("/v1/projects?next=%zz", "admin"))), 400u);
}

TEST_F(HttpTest, ARequestNotReadWholeWithinTheDeadlineIsClosedUnanswered) {
  start(std::chrono::milliseconds(100));
  EXPECT_EQ(exchange("GET /v1/projects HTTP/1.1\r\nHost: lou"), "");
}

TEST(ApiEndpoint, AnEndpointIsASchemeAHostAndAPort) {
  auto e = api::parse_endpoint("https://louhi.example:7443");
  EXPECT_TRUE(e.tls);
  EXPECT_EQ(e.host, "louhi.example");
  EXPECT_EQ(e.port, 7443);
  e = api::parse_endpoint("http://[::1]:80/");
  EXPECT_FALSE(e.tls);
  EXPECT_EQ(e.host, "::1");
  EXPECT_EQ(api::parse_endpoint("https://louhi.example").port, 443);
  for (const char* bad : {"ftp://x", "louhi.example", "http://", "http://x:0", "http://x:65536", "http://x/v1", "http://u@x", "http://[::1", "http://x:p"})
    EXPECT_THROW(api::parse_endpoint(bad), api::Invalid) << bad;
}

TEST_F(HttpTest, TheClientRoundTripsThroughTheServer) {
  start();
  api::Client client(io, api::Endpoint{false, "127.0.0.1", server->port()});
  std::promise<runtime::Result<api::Response>> got;
  client.request("PUT", "/v1/projects/web", R"({"description":"d"})", "admin", runtime::Deadline(10000), io.get_executor(),
                 [&](runtime::Result<api::Response> r) { got.set_value(std::move(r)); });
  auto r = got.get_future().get();
  ASSERT_TRUE(r.ok()) << r.error().message;
  EXPECT_EQ(r.value().status, 200u);
  EXPECT_EQ(r.value().body.at("name"), "web");

  std::promise<runtime::Result<api::Response>> refused;
  client.request("GET", "/v1/projects", "", "forged", runtime::Deadline(10000), io.get_executor(),
                 [&](runtime::Result<api::Response> answer) { refused.set_value(std::move(answer)); });
  r = refused.get_future().get();
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.value().status, 401u);
}

TEST_F(HttpTest, AClientThatCannotConnectSaysSo) {
  start();
  const uint16_t port = server->port();
  server->stop();
  api::Client client(io, api::Endpoint{false, "127.0.0.1", port});
  std::promise<runtime::Result<api::Response>> got;
  client.request("GET", "/v1/projects", "", "admin", runtime::Deadline(10000), io.get_executor(),
                 [&](runtime::Result<api::Response> r) { got.set_value(std::move(r)); });
  auto r = got.get_future().get();
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().kind, runtime::Error::Kind::unavailable);
}

TEST_F(HttpTest, ServingNeedsAListener) {
  api::HttpServer none(io, api, authenticator, api::HttpConfig{}, dannan::status::ScopedLogger(dannan::status::null_sink(), "test"));
  EXPECT_THROW(none.start(), std::invalid_argument);
}
