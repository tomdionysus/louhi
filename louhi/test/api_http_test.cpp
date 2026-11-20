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
// once. Every answer's status line carries its reason phrase. Over HTTPS the
// same, the client checking the server's chain and name; a client given an
// answer that is not HTTP, a body that is not JSON, or no whole answer within
// its deadline, says so.

#include <gtest/gtest.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

#include <array>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/json.hpp>
#include <chrono>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include "dannan/identity/certificate.h"
#include "dannan/identity/signing.h"
#include "dannan/listen/certificates.h"
#include "dannan/listen/connector.h"
#include "dannan/test/fixtures/stepped_time.h"
#include "dannan/test/fixtures/temp_dir.h"
#include "louhi/api/client.h"
#include "louhi/api/http.h"
#include "louhi/model/codec.h"
#include "louhi/model/keys.h"
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
    if (token == "reader") return api::Claims{"bob", {"louhi"}, {"louhi:web:read"}, 4'000'000'000, false};
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

  static std::string status_line(const std::string& reply) { return reply.substr(0, reply.find("\r\n")); }

  static std::string with_body(const std::string& method, const std::string& path, const std::string& token, const std::string& body) {
    return method + " " + path + " HTTP/1.1\r\nHost: louhi\r\nAuthorization: Bearer " + token + "\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\n\r\n" + body;
  }

  // Bytes put in the store as they are.
  void put_raw(const std::string& key, store::Bytes bytes) {
    std::promise<bool> written;
    store.put(key, std::move(bytes), store::Deadline(1000), io.get_executor(), [&](auto r) { written.set_value(r.ok()); });
    ASSERT_TRUE(written.get_future().get());
  }

  runtime::Result<api::Response> ask(api::Client& client, const std::string& token, runtime::Deadline deadline = runtime::Deadline(10000)) {
    std::promise<runtime::Result<api::Response>> got;
    client.request("GET", "/v1/projects", "", token, deadline, io.get_executor(), [&](runtime::Result<api::Response> r) { got.set_value(std::move(r)); });
    auto future = got.get_future();
    if (future.wait_for(std::chrono::seconds(30)) != std::future_status::ready) return runtime::Error{runtime::Error::Kind::timed_out, "not told"};
    return future.get();
  }
};

uint64_t now_ms() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

void write_file(const std::filesystem::path& path, const std::string& text) { std::ofstream(path) << text; }

std::string pem_of(BIO* bio) {
  char* data = nullptr;
  const auto size = BIO_get_mem_data(bio, &data);
  return std::string(data, static_cast<size_t>(size));
}

// A CA made here, and a server certificate under it for 127.0.0.1: the client
// checks the server's name, which a Dannan node certificate does not carry.
struct Pki {
  dannan::test::TempDir dir;
  dannan::identity::CertificateAuthority ca = dannan::identity::CertificateAuthority::create("louhi test ca", now_ms() - 60000);

  dannan::listen::CertificatePaths server() {
    const auto key = dannan::identity::SigningKey::generate(dannan::identity::SigningAlgorithm::es256);
    const auto ca_der = ca.certificate().der();
    const unsigned char* cursor = ca_der.data();
    std::unique_ptr<X509, decltype(&X509_free)> issuer(d2i_X509(nullptr, &cursor, static_cast<long>(ca_der.size())), X509_free);
    std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), X509_free);
    X509_set_version(certificate.get(), 2);
    ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 7);
    X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60);
    X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600);
    X509_NAME_add_entry_by_txt(X509_get_subject_name(certificate.get()), "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("louhi test"), -1, -1, 0);
    X509_set_issuer_name(certificate.get(), X509_get_subject_name(issuer.get()));
    X509_set_pubkey(certificate.get(), key.native_handle());
    X509V3_CTX context;
    X509V3_set_ctx_nodb(&context);
    X509V3_set_ctx(&context, issuer.get(), certificate.get(), nullptr, nullptr, 0);
    X509_EXTENSION* names = X509V3_EXT_conf_nid(nullptr, &context, NID_subject_alt_name, "IP:127.0.0.1");
    X509_add_ext(certificate.get(), names, -1);
    X509_EXTENSION_free(names);
    X509_sign(certificate.get(), ca.key().native_handle(), EVP_sha256());

    std::unique_ptr<BIO, decltype(&BIO_free)> chain(BIO_new(BIO_s_mem()), BIO_free);
    PEM_write_bio_X509(chain.get(), certificate.get());
    std::unique_ptr<BIO, decltype(&BIO_free)> private_key(BIO_new(BIO_s_mem()), BIO_free);
    PEM_write_bio_PrivateKey(private_key.get(), key.native_handle(), nullptr, nullptr, 0, nullptr, nullptr);
    write_file(dir.path() / "server.crt", pem_of(chain.get()) + ca.certificate().pem());
    write_file(dir.path() / "server.key", pem_of(private_key.get()));
    return {dir.path() / "server.crt", dir.path() / "server.key", {}};
  }

  std::shared_ptr<const dannan::listen::ClientTrust> trust() {
    write_file(dir.path() / "trust.pem", ca.certificate().pem());
    return std::make_shared<dannan::listen::ClientTrust>(dir.path() / "trust.pem");
  }
};

// Accepts one connection on 127.0.0.1, reads what comes, and answers with
// `reply` (nothing: never answers), keeping the connection open.
class OneAnswer {
 public:
  explicit OneAnswer(std::string reply) : _reply(std::move(reply)) {
    _acceptor.async_accept(_socket, [this](const boost::system::error_code& failed) {
      if (failed) return;
      _socket.async_read_some(asio::buffer(_request), [this](const boost::system::error_code& read, size_t) {
        if (read || _reply.empty()) return;
        asio::async_write(_socket, asio::buffer(_reply), [](const boost::system::error_code&, size_t) {});
      });
    });
    _thread = std::thread([this] { _io.run(); });
  }
  ~OneAnswer() {
    _io.stop();
    _thread.join();
  }
  uint16_t port() const { return _acceptor.local_endpoint().port(); }

 private:
  asio::io_context _io;
  tcp::acceptor _acceptor{_io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)};
  tcp::socket _socket{_io};
  std::array<char, 4096> _request{};
  std::string _reply;
  std::thread _thread;
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

// Dashboard design: the OpenAPI document is behind a token like every route.
TEST_F(HttpTest, TheOpenApiDocumentNeedsAToken) {
  start();
  EXPECT_EQ(status_of(exchange(get("/v1/openapi.json", ""))), 401u);
  const auto reply = exchange(get("/v1/openapi.json", "admin"));
  EXPECT_EQ(status_of(reply), 200u) << reply;
  EXPECT_EQ(body_of(reply).at("openapi"), "3.1.0");
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

TEST_F(HttpTest, EveryStatusLineCarriesItsReasonPhrase) {
  start();
  EXPECT_EQ(status_line(exchange(get("/v1/projects", ""))), "HTTP/1.1 401 Unauthorized");
  EXPECT_EQ(status_line(exchange(with_body("PUT", "/v1/projects/web", "reader", "{}"))), "HTTP/1.1 403 Forbidden");
  EXPECT_EQ(status_line(exchange(get("/v1/projects/none", "admin"))), "HTTP/1.1 404 Not Found");
  EXPECT_EQ(status_line(exchange(with_body("DELETE", "/v1/projects/web", "admin", ""))), "HTTP/1.1 405 Method Not Allowed");
  EXPECT_EQ(status_line(exchange(with_body("PUT", "/v1/projects/web", "admin", "{}"))), "HTTP/1.1 200 OK");
  EXPECT_EQ(status_line(exchange(with_body("PUT", "/v1/projects/web/quota", "admin", "{not json"))), "HTTP/1.1 400 Bad Request");
  const std::string definition = R"({"family":"f","cpu_millis":1,"memory_mib":4,"containers":[{"name":"a","image":"alpine"}]})";
  images.fail_next("resolve", runtime::Error{runtime::Error::Kind::timed_out, "no answer"});
  EXPECT_EQ(status_line(exchange(with_body("POST", "/v1/projects/web/task-definitions", "admin", definition))), "HTTP/1.1 503 Service Unavailable");
  images.digests = {{"alpine", "sha256:" + std::string(64, 'b')}};
  EXPECT_EQ(status_line(exchange(with_body("POST", "/v1/projects/web/task-definitions", "admin", definition))), "HTTP/1.1 200 OK");
  EXPECT_EQ(status_line(exchange(with_body("POST", "/v1/projects/web/tasks", "admin", R"({"family":"f"})"))), "HTTP/1.1 409 Conflict") << "no quota";
  EXPECT_EQ(status_line(exchange(with_body("PUT", "/v1/projects/web", "admin", std::string(2048, ' ')))), "HTTP/1.1 413 Payload Too Large");
  EXPECT_EQ(status_line(exchange("GET /v1/projects HTTP/1.1\r\nHost: louhi\r\nX-Padding: " + std::string(17 * 1024, 'x') + "\r\n\r\n")),
            "HTTP/1.1 431 Request Header Fields Too Large");
  put_raw(model::key(test::project()), store::Bytes{0xff});
  EXPECT_EQ(status_line(exchange(get("/v1/projects/web", "admin"))), "HTTP/1.1 500 Internal Server Error");
}

TEST_F(HttpTest, TheClientSpeaksToTheServerOverHttps) {
  Pki pki;
  api::HttpConfig config;
  api::HttpsListen https;
  https.host = "127.0.0.1";
  https.certificates = pki.server();
  config.tls = https;
  server = std::make_unique<api::HttpServer>(io, api, authenticator, config, dannan::status::ScopedLogger(dannan::status::null_sink(), "test"));
  server->start();
  EXPECT_EQ(server->port(), 0u) << "no plain listener";
  ASSERT_NE(server->tls_port(), 0u);
  api::Client client(io, api::Endpoint{true, "127.0.0.1", server->tls_port()}, pki.trust());
  const auto r = ask(client, "admin");
  ASSERT_TRUE(r.ok()) << r.error().message;
  EXPECT_EQ(r.value().status, 200u);
  EXPECT_TRUE(r.value().body.at("projects").as_array().empty());
}

TEST_F(HttpTest, CertificatesThatCannotBeLoadedAreRefusedAtStart) {
  dannan::test::TempDir dir;
  api::HttpConfig config;
  api::HttpsListen https;
  https.host = "127.0.0.1";
  https.certificates = {dir.path() / "none.crt", dir.path() / "none.key", {}};
  config.tls = https;
  api::HttpServer refused(io, api, authenticator, config, dannan::status::ScopedLogger(dannan::status::null_sink(), "test"));
  EXPECT_THROW(refused.start(), std::runtime_error);
}

TEST_F(HttpTest, AnAnswerThatIsNotHttpIsMalformed) {
  OneAnswer server_of_nonsense("SSH-2.0-OpenSSH\r\n\r\n");
  api::Client client(io, api::Endpoint{false, "127.0.0.1", server_of_nonsense.port()});
  const auto r = ask(client, "admin");
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().kind, runtime::Error::Kind::malformed);
}

TEST_F(HttpTest, AnAnswerWhoseBodyIsNotJsonIsMalformed) {
  OneAnswer not_json("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello");
  api::Client client(io, api::Endpoint{false, "127.0.0.1", not_json.port()});
  const auto r = ask(client, "admin");
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().kind, runtime::Error::Kind::malformed);
}

TEST_F(HttpTest, NoWholeAnswerWithinTheDeadlineTimesOut) {
  OneAnswer silent("");
  api::Client client(io, api::Endpoint{false, "127.0.0.1", silent.port()});
  const auto r = ask(client, "admin", runtime::Deadline(100));
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().kind, runtime::Error::Kind::timed_out);
}
