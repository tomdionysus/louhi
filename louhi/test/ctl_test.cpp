//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// louhi-ctl (ctl/ctl.h; design, "API and sign-on"):
// - each command makes its one call, its operands one path segment each,
//   and a command line it cannot act on exits 2 before anything is asked;
// - the credentials file is owner-only, replaced whole, read with a bound,
//   and found by --credentials, then $LOUHI_CREDENTIALS, then $HOME;
// - a call carries the access token held while more than a minute of it is
//   left; otherwise the refresh token is spent first and the new tokens
//   kept; a refresh refused says to log in and changes nothing;
// - the endpoints are --endpoint, then $LOUHI_ENDPOINT, then those saved,
//   each a node's API; a call goes to the next when one cannot be reached,
//   and a read also when one gave no answer, but a write that may have
//   arrived is never sent again; any HTTP answer is the answer;
// - the answer's JSON goes to stdout, and the exit is 0 for a 2xx answer, 1
//   for any other answer or failure;
// - login signs in by the device flow, telling the person where to go, and
//   keeps nothing unless it signed in; logout removes the credentials.
// Driven by a fake issuer, a fake API, credentials in memory and stepped
// clocks: no sleep anywhere.

#include "louhi/ctl/ctl.h"

#include <gtest/gtest.h>
#include <sys/stat.h>

#include <atomic>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "dannan/test/fixtures/stepped_time.h"
#include "dannan/test/fixtures/temp_dir.h"
#include "louhi/test/fixtures/fake_https.h"
#include "louhi/version.h"

using namespace louhi;
namespace asio = boost::asio;
namespace json = boost::json;
using ctl::Args;
using ctl::Call;
using ctl::Credentials;

namespace {

constexpr const char* issuer = "https://lugh.example";
constexpr uint64_t now_unix = 1'800'000'000;
const std::string home_credentials = "/home/user/.config/louhi/credentials.json";

// The API, answering each request at once with what the test queued, else
// 200 and an empty object; each answer posted to the caller's executor.
class FakeApi final : public api::Requester {
 public:
  struct Request {
    std::string method;
    std::string target;
    std::string body;
    std::string token;
  };
  void request(std::string method, std::string target, std::string body, std::string token, runtime::Deadline, runtime::Executor executor,
               runtime::Completion<api::Response> done) override {
    runtime::Result<api::Response> answer = api::Response{200, json::object{}};
    {
      std::lock_guard<std::mutex> lock(_mutex);
      _requests.push_back({std::move(method), std::move(target), std::move(body), std::move(token)});
      if (!_answers.empty()) {
        answer = std::move(_answers.front());
        _answers.pop_front();
      }
    }
    asio::post(executor, [done = std::move(done), answer = std::move(answer)]() mutable { done(std::move(answer)); });
  }
  void queue(runtime::Result<api::Response> answer) {
    std::lock_guard<std::mutex> lock(_mutex);
    _answers.push_back(std::move(answer));
  }
  std::vector<Request> requests() {
    std::lock_guard<std::mutex> lock(_mutex);
    return _requests;
  }

 private:
  std::mutex _mutex;
  std::vector<Request> _requests;
  std::deque<runtime::Result<api::Response>> _answers;
};

// Credentials files in memory, by path.
class Files {
 public:
  class File final : public ctl::CredentialFile {
   public:
    File(Files& files, std::string path) : _files(files), _path(std::move(path)) {}
    std::optional<std::string> read() override {
      std::lock_guard<std::mutex> lock(_files._mutex);
      const auto it = _files._text.find(_path);
      if (it == _files._text.end()) return std::nullopt;
      return it->second;
    }
    void write(std::string_view text) override {
      std::lock_guard<std::mutex> lock(_files._mutex);
      _files._text[_path] = std::string(text);
    }
    void remove() override {
      std::lock_guard<std::mutex> lock(_files._mutex);
      _files._text.erase(_path);
    }

   private:
    Files& _files;
    const std::string _path;
  };

  std::unique_ptr<ctl::CredentialFile> open(const std::filesystem::path& path) { return std::make_unique<File>(*this, path.string()); }
  std::optional<Credentials> at(const std::string& path) {
    const auto text = File(*this, path).read();
    if (!text) return std::nullopt;
    return ctl::decode(*text);
  }
  void put(const std::string& path, const Credentials& c) { File(*this, path).write(ctl::encode(c)); }

 private:
  std::mutex _mutex;
  std::map<std::string, std::string> _text;
};

// Steady time that moves a second at every reading, and never asks a timer
// to wait: the device flow's polls come as fast as the io thread runs them.
class RushingTime final : public dannan::contract::TimeSource {
 public:
  dannan::contract::TimePoint now() const noexcept override { return dannan::contract::TimePoint(std::chrono::seconds(_seconds.fetch_add(1) + 1'000'000)); }
  std::chrono::milliseconds wake_within(std::chrono::milliseconds) const noexcept override { return std::chrono::milliseconds(0); }

 private:
  mutable std::atomic<int64_t> _seconds{0};
};

Credentials fresh() {
  Credentials c;
  c.issuer = issuer;
  c.client_id = "louhi-ctl";
  c.endpoints = {"http://louhi.example:8080"};
  c.scopes = {"louhi:web:read"};
  c.access_token = "a1";
  c.refresh_token = "r1";
  c.expires_unix = now_unix + 3600;
  return c;
}

std::vector<std::string> words(std::initializer_list<const char*> list) { return std::vector<std::string>(list.begin(), list.end()); }

Args args(std::initializer_list<const char*> list) { return ctl::parse(words(list)); }

// What the command reads from a file operand: the file's name, as a marker.
std::string read_marker(const std::string& path) { return "<" + path + ">"; }

struct CtlTest : ::testing::Test {
  asio::io_context io;
  asio::executor_work_guard<asio::io_context::executor_type> work = asio::make_work_guard(io);
  std::thread thread{[this] { io.run(); }};

  std::shared_ptr<test::FakeHttps> https = std::make_shared<test::FakeHttps>();
  std::shared_ptr<FakeApi> server = std::make_shared<FakeApi>();
  Files files;
  std::map<std::string, std::string> variables{{"HOME", "/home/user"}};
  dannan::test::SteppedWall wall{now_unix * 1000};
  RushingTime steady;
  std::ostringstream out;
  std::ostringstream err;
  std::vector<std::string> issuer_roots;
  std::vector<std::pair<api::Endpoint, std::string>> api_endpoints;

  ~CtlTest() override {
    work.reset();
    io.stop();
    thread.join();
  }

  int run(std::initializer_list<const char*> list) {
    out.str("");
    err.str("");
    ctl::Environment env;
    env.issuer = [this](const std::string& ca) -> std::shared_ptr<lugh::client::Https> {
      issuer_roots.push_back(ca);
      return https;
    };
    env.api = [this](const api::Endpoint& endpoint, const std::string& ca) -> std::shared_ptr<api::Requester> {
      api_endpoints.emplace_back(endpoint, ca);
      return server;
    };
    env.credentials = [this](const std::filesystem::path& path) { return files.open(path); };
    env.read = read_marker;
    env.getenv = [this](const std::string& name) -> std::optional<std::string> {
      const auto it = variables.find(name);
      if (it == variables.end()) return std::nullopt;
      return it->second;
    };
    env.wall = &wall;
    env.steady = &steady;
    env.executor = io.get_executor();
    env.out = &out;
    env.err = &err;
    return ctl::run(words(list), env);
  }

  // The issuer: its discovery, and its token endpoint answered by `token`.
  void issuer_answers(std::function<lugh::Outcome<lugh::client::HttpsResponse>(const std::map<std::string, std::string>& form)> token,
                      std::function<lugh::Outcome<lugh::client::HttpsResponse>(const std::map<std::string, std::string>& form)> device = nullptr) {
    https->answer_by([token = std::move(token), device = std::move(device)](const lugh::client::HttpsRequest& request) {
      const std::string base = issuer;
      if (request.url == base + "/.well-known/openid-configuration") return test::json_answer(200, test::discovery(issuer));
      if (request.url == base + "/token") return token(test::form_fields(request.body));
      if (request.url == base + "/device" && device) return device(test::form_fields(request.body));
      return test::json_answer(404, {{"error", "not_found"}});
    });
  }
};

}  // namespace

TEST(CtlArgs, FlagsTakeTheirValuesAndTheRestStaysInOrder) {
  const auto a = args({"run-task", "--count", "3", "web", "--scope", "x", "frontend", "--scope", "y"});
  EXPECT_EQ(a.positional, words({"run-task", "web", "frontend"}));
  EXPECT_EQ(a.flag("count"), "3");
  EXPECT_EQ(a.all("scope"), words({"x", "y"}));
  EXPECT_FALSE(a.flag("revision"));
  EXPECT_TRUE(args({"--version"}).flags.count("version"));
}

TEST(CtlArgs, AFlagWithNoValueOrGivenTwiceWhereOneIsWantedIsAUsageError) {
  EXPECT_THROW(args({"list-tasks", "web", "--limit"}), ctl::Usage);
  const auto twice = args({"run-task", "web", "frontend", "--count", "1", "--count", "2"});
  EXPECT_THROW(twice.flag("count"), ctl::Usage);
  EXPECT_THROW(ctl::command(twice, read_marker), ctl::Usage);
}

TEST(CtlCommand, EachCommandMakesItsCall) {
  const auto call = [](std::initializer_list<const char*> list) { return ctl::command(args(list), read_marker); };
  EXPECT_EQ(call({"list-projects"}), (Call{"GET", "/v1/projects", ""}));
  EXPECT_EQ(call({"list-nodes"}), (Call{"GET", "/v1/nodes", ""}));
  EXPECT_EQ(call({"put-project", "web", "--description", "the site"}), (Call{"PUT", "/v1/projects/web", R"({"description":"the site"})"}));
  EXPECT_EQ(call({"put-project", "web"}), (Call{"PUT", "/v1/projects/web", R"({"description":""})"}));
  EXPECT_EQ(call({"describe-project", "web"}), (Call{"GET", "/v1/projects/web", ""}));
  EXPECT_EQ(call({"put-quota", "web", "quota.json"}), (Call{"PUT", "/v1/projects/web/quota", "<quota.json>"}));
  EXPECT_EQ(call({"put-quota", "web", "-"}), (Call{"PUT", "/v1/projects/web/quota", "<->"}));
  EXPECT_EQ(call({"describe-quota", "web"}), (Call{"GET", "/v1/projects/web/quota", ""}));
  EXPECT_EQ(call({"register-task-definition", "web", "def.json"}), (Call{"POST", "/v1/projects/web/task-definitions", "<def.json>"}));
  EXPECT_EQ(call({"list-task-definition-families", "web"}), (Call{"GET", "/v1/projects/web/task-definitions", ""}));
  EXPECT_EQ(call({"list-task-definitions", "web", "frontend"}), (Call{"GET", "/v1/projects/web/task-definitions/frontend", ""}));
  EXPECT_EQ(call({"describe-task-definition", "web", "frontend", "3"}), (Call{"GET", "/v1/projects/web/task-definitions/frontend/3", ""}));
  EXPECT_EQ(call({"run-task", "web", "frontend"}), (Call{"POST", "/v1/projects/web/tasks", R"({"family":"frontend"})"}));
  EXPECT_EQ(call({"run-task", "web", "frontend", "--revision", "3", "--count", "2", "--client-token", "c1"}),
            (Call{"POST", "/v1/projects/web/tasks", R"({"family":"frontend","revision":3,"count":2,"client_token":"c1"})"}));
  EXPECT_EQ(call({"list-tasks", "web"}), (Call{"GET", "/v1/projects/web/tasks", ""}));
  EXPECT_EQ(call({"list-tasks", "web", "--limit", "10", "--next", "abc"}), (Call{"GET", "/v1/projects/web/tasks?limit=10&next=abc", ""}));
  EXPECT_EQ(call({"list-tasks", "web", "--next", "abc"}), (Call{"GET", "/v1/projects/web/tasks?next=abc", ""}));
  EXPECT_EQ(call({"describe-task", "web", "t1"}), (Call{"GET", "/v1/projects/web/tasks/t1", ""}));
  EXPECT_EQ(call({"stop-task", "web", "t1", "--reason", "done"}), (Call{"POST", "/v1/projects/web/tasks/t1/stop", R"({"reason":"done"})"}));
  EXPECT_EQ(call({"stop-task", "web", "t1"}), (Call{"POST", "/v1/projects/web/tasks/t1/stop", "{}"}));
}

TEST(CtlCommand, EachOperandIsOnePathSegment) {
  const auto c = ctl::command(args({"describe-task", "a b/c", "x?y#z"}), read_marker);
  EXPECT_EQ(c.target, "/v1/projects/a%20b%2Fc/tasks/x%3Fy%23z");
  const auto list = ctl::command(args({"list-tasks", "web", "--next", "a&b=c"}), read_marker);
  EXPECT_EQ(list.target, "/v1/projects/web/tasks?next=a%26b%3Dc");
}

TEST(CtlCommand, AWrongCountOfOperandsOrAnUnknownCommandIsAUsageError) {
  for (const auto& list : {words({"list-projects", "extra"}), words({"describe-project"}), words({"put-quota", "web"}), words({"describe-task", "web"}),
                           words({"describe-task-definition", "web", "frontend"}), words({"stop-task", "web", "t1", "t2"}), words({"launch", "web"})}) {
    EXPECT_THROW(ctl::command(ctl::parse(list), read_marker), ctl::Usage) << list.front();
  }
}

TEST(CtlCommand, ANumberThatIsNotOneIsAUsageError) {
  for (const char* bad : {"abc", "-1", "+1", "1.5", "1e3", " 1", "99999999999999999999"}) {
    EXPECT_THROW(ctl::command(args({"run-task", "web", "frontend", "--count", bad}), read_marker), ctl::Usage) << bad;
    EXPECT_THROW(ctl::command(args({"run-task", "web", "frontend", "--revision", bad}), read_marker), ctl::Usage) << bad;
    EXPECT_THROW(ctl::command(args({"list-tasks", "web", "--limit", bad}), read_marker), ctl::Usage) << bad;
  }
}

TEST(CtlCredentials, RoundTripThroughTheirText) {
  const auto c = fresh();
  EXPECT_EQ(ctl::decode(ctl::encode(c)), c);
}

TEST(CtlCredentials, AFieldOfTheWrongTypeIsEmptyAndAnExpiryThatIsNotOneIsNone) {
  const auto c = ctl::decode(R"({"issuer":7,"client_id":"louhi-ctl","scopes":["a",3,"b"],"expires_unix":-5,"access_token":"a1"})");
  EXPECT_EQ(c.issuer, "");
  EXPECT_EQ(c.client_id, "louhi-ctl");
  EXPECT_EQ(c.scopes, words({"a", "b"}));
  EXPECT_EQ(c.expires_unix, 0u);
  EXPECT_EQ(ctl::decode(R"({"expires_unix":1.5})").expires_unix, 0u);
  EXPECT_EQ(ctl::decode(R"({"expires_unix":"soon"})").expires_unix, 0u);
}

TEST(CtlCredentials, SavedEndpointsAreAListAndAnOldSingleOneIsRead) {
  auto c = fresh();
  c.endpoints = {"http://a.example:8080", "https://b.example"};
  EXPECT_EQ(ctl::decode(ctl::encode(c)).endpoints, c.endpoints);
  EXPECT_EQ(ctl::decode(R"({"endpoint":"http://old.example:8080"})").endpoints, words({"http://old.example:8080"}));
  EXPECT_EQ(ctl::decode(R"({"endpoints":["http://a.example",4,"http://b.example"]})").endpoints, words({"http://a.example", "http://b.example"}));
}

TEST(CtlCredentials, TextThatIsNotAnObjectIsRefused) {
  for (const char* bad : {"", "[]", "\"x\"", "{", "{} {}"}) EXPECT_THROW(ctl::decode(bad), std::runtime_error) << bad;
}

TEST(CtlCredentials, AreFoundByTheFlagThenTheVariableThenHome) {
  std::map<std::string, std::string> variables{{"HOME", "/home/user"}, {"LOUHI_CREDENTIALS", "/etc/louhi/c.json"}};
  const ctl::Getenv getenv = [&](const std::string& name) -> std::optional<std::string> {
    const auto it = variables.find(name);
    if (it == variables.end()) return std::nullopt;
    return it->second;
  };
  EXPECT_EQ(ctl::credentials_path(args({"--credentials", "/x/c.json"}), getenv), "/x/c.json");
  EXPECT_EQ(ctl::credentials_path(args({}), getenv), "/etc/louhi/c.json");
  variables.erase("LOUHI_CREDENTIALS");
  EXPECT_EQ(ctl::credentials_path(args({}), getenv), home_credentials);
  variables.erase("HOME");
  EXPECT_THROW(ctl::credentials_path(args({}), getenv), std::runtime_error);
}

TEST(CtlCredentials, OnDiskTheyAreOwnerOnlyAndReplacedWhole) {
  dannan::test::TempDir dir;
  const auto path = dir.path() / "config" / "louhi" / "credentials.json";
  ctl::DiskCredentialFile file(path);
  EXPECT_FALSE(file.read()) << "no file is nothing, not an error";

  file.write(ctl::encode(fresh()));
  struct stat info{};
  ASSERT_EQ(::stat(path.c_str(), &info), 0);
  EXPECT_EQ(info.st_mode & 0777, 0600u);
  ASSERT_EQ(::stat(path.parent_path().c_str(), &info), 0);
  EXPECT_EQ(info.st_mode & 0077, 0u) << "a directory made for them is its owner's alone";

  file.write("{}\n");
  EXPECT_EQ(file.read(), "{}\n");
  file.remove();
  EXPECT_FALSE(file.read());
  EXPECT_NO_THROW(file.remove());
}

TEST(CtlCredentials, OnDiskAFileTooLargeToBeThemIsRefused) {
  dannan::test::TempDir dir;
  const auto path = dir.path() / "credentials.json";
  {
    std::ofstream big(path, std::ios::binary);
    big << std::string(65 * 1024, ' ');
  }
  EXPECT_THROW(ctl::DiskCredentialFile(path).read(), std::runtime_error);
}

TEST_F(CtlTest, VersionPrintsTheVersion) {
  EXPECT_EQ(run({"--version"}), 0);
  EXPECT_EQ(out.str(), std::string(kVersion) + "\n");
}

TEST_F(CtlTest, ACommandLineThatCannotBeActedOnExitsTwoBeforeAnythingIsAsked) {
  EXPECT_EQ(run({}), 2);
  EXPECT_EQ(err.str().rfind("louhi-ctl: usage", 0), 0u) << err.str();
  EXPECT_EQ(run({"launch", "web"}), 2) << "before the missing credentials are noticed";
  EXPECT_EQ(run({"run-task", "web", "frontend", "--count", "many"}), 2);
  files.put(home_credentials, fresh());
  EXPECT_EQ(run({"list-projects", "--endpoint", "ftp://louhi.example"}), 2);
  EXPECT_EQ(run({"logout", "now"}), 2);
  EXPECT_TRUE(server->requests().empty());
  EXPECT_TRUE(https->sent().empty());
  EXPECT_TRUE(files.at(home_credentials)) << "a usage error removes nothing";
}

TEST_F(CtlTest, ACallCarriesTheTokenHeldAndPrintsTheAnswer) {
  files.put(home_credentials, fresh());
  server->queue(api::Response{200, json::object{{"projects", json::array{"web"}}}});
  EXPECT_EQ(run({"list-projects"}), 0);
  EXPECT_EQ(out.str(), "{\"projects\":[\"web\"]}\n");
  ASSERT_EQ(server->requests().size(), 1u);
  EXPECT_EQ(server->requests()[0].method, "GET");
  EXPECT_EQ(server->requests()[0].target, "/v1/projects");
  EXPECT_EQ(server->requests()[0].token, "a1");
  ASSERT_EQ(api_endpoints.size(), 1u);
  EXPECT_FALSE(api_endpoints[0].first.tls);
  EXPECT_EQ(api_endpoints[0].first.host, "louhi.example");
  EXPECT_EQ(api_endpoints[0].first.port, 8080);
  EXPECT_TRUE(https->sent().empty()) << "a token with time left needs no issuer";
}

TEST_F(CtlTest, ABodyFromAFileIsSent) {
  files.put(home_credentials, fresh());
  EXPECT_EQ(run({"register-task-definition", "web", "def.json"}), 0);
  ASSERT_EQ(server->requests().size(), 1u);
  EXPECT_EQ(server->requests()[0].body, "<def.json>");
}

TEST_F(CtlTest, AnAnswerThatIsNot2xxIsPrintedAndExitsOne) {
  files.put(home_credentials, fresh());
  server->queue(api::Response{404, json::object{{"error", "not_found"}, {"message", "no project web"}}});
  EXPECT_EQ(run({"describe-project", "web"}), 1);
  EXPECT_EQ(out.str(), "{\"error\":\"not_found\",\"message\":\"no project web\"}\n");
}

TEST_F(CtlTest, AnApiThatCannotBeReachedExitsOneAndSaysWhy) {
  files.put(home_credentials, fresh());
  server->queue(runtime::Error{runtime::Error::Kind::unavailable, "connection refused"});
  EXPECT_EQ(run({"list-projects"}), 1);
  EXPECT_EQ(out.str(), "");
  EXPECT_NE(err.str().find("connection refused"), std::string::npos) << err.str();
}

// Any node serves the API, so a node down is not the person's problem.
TEST_F(CtlTest, ACallGoesToTheNextEndpointWhenOneCannotBeReached) {
  auto c = fresh();
  c.endpoints = {"http://a.example:8080", "http://b.example:8080", "http://c.example:8080"};
  files.put(home_credentials, c);
  server->queue(runtime::Error{runtime::Error::Kind::unavailable, "connecting: refused"});
  server->queue(runtime::Error{runtime::Error::Kind::unavailable, "connecting: no route"});
  server->queue(api::Response{200, json::object{{"task", "t1"}}});
  // A write too: nothing reached a node that could not be connected to.
  EXPECT_EQ(run({"run-task", "web", "frontend"}), 0) << err.str();
  EXPECT_EQ(out.str(), "{\"task\":\"t1\"}\n");
  ASSERT_EQ(api_endpoints.size(), 3u);
  EXPECT_EQ(api_endpoints[0].first.host, "a.example");
  EXPECT_EQ(api_endpoints[1].first.host, "b.example");
  EXPECT_EQ(api_endpoints[2].first.host, "c.example");
  EXPECT_EQ(server->requests().size(), 3u);
}

TEST_F(CtlTest, AReadWithNoAnswerGoesToTheNextEndpoint) {
  auto c = fresh();
  c.endpoints = {"http://a.example:8080", "http://b.example:8080"};
  files.put(home_credentials, c);
  server->queue(runtime::Error{runtime::Error::Kind::timed_out, "no whole answer within the deadline"});
  EXPECT_EQ(run({"list-projects"}), 0) << err.str();
  EXPECT_EQ(server->requests().size(), 2u);
  server->queue(runtime::Error{runtime::Error::Kind::malformed, "the answer is not HTTP"});
  EXPECT_EQ(run({"describe-project", "web"}), 0) << err.str();
  EXPECT_EQ(server->requests().size(), 4u);
}

TEST_F(CtlTest, AWriteThatMayHaveArrivedIsNeverSentAgain) {
  auto c = fresh();
  c.endpoints = {"http://a.example:8080", "http://b.example:8080"};
  files.put(home_credentials, c);
  server->queue(runtime::Error{runtime::Error::Kind::timed_out, "no whole answer within the deadline"});
  EXPECT_EQ(run({"run-task", "web", "frontend"}), 1);
  EXPECT_EQ(server->requests().size(), 1u);
  EXPECT_NE(err.str().find("a.example"), std::string::npos) << err.str();
  EXPECT_NE(err.str().find("may have"), std::string::npos) << "the person is told it may have happened: " << err.str();
}

TEST_F(CtlTest, AnHttpAnswerIsTheAnswerWhateverItSays) {
  auto c = fresh();
  c.endpoints = {"http://a.example:8080", "http://b.example:8080"};
  files.put(home_credentials, c);
  server->queue(api::Response{503, json::object{{"error", "not_ready"}}});
  EXPECT_EQ(run({"list-projects"}), 1);
  EXPECT_EQ(out.str(), "{\"error\":\"not_ready\"}\n");
  EXPECT_EQ(server->requests().size(), 1u);
}

TEST_F(CtlTest, WhenNoEndpointAnswersEachIsNamedWithWhy) {
  auto c = fresh();
  c.endpoints = {"http://a.example:8080", "http://b.example:8080"};
  files.put(home_credentials, c);
  server->queue(runtime::Error{runtime::Error::Kind::unavailable, "connecting: refused"});
  server->queue(runtime::Error{runtime::Error::Kind::unavailable, "connecting: no route"});
  EXPECT_EQ(run({"list-projects"}), 1);
  for (const char* expected : {"a.example", "refused", "b.example", "no route"}) EXPECT_NE(err.str().find(expected), std::string::npos) << err.str();
}

TEST_F(CtlTest, EndpointsComeFromRepeatedFlagsOrAListInTheVariable) {
  files.put(home_credentials, fresh());
  server->queue(runtime::Error{runtime::Error::Kind::unavailable, "connecting: refused"});
  EXPECT_EQ(run({"list-nodes", "--endpoint", "http://f1.example", "--endpoint", "http://f2.example"}), 0) << err.str();
  variables["LOUHI_ENDPOINT"] = "http://v1.example, https://v2.example";
  server->queue(runtime::Error{runtime::Error::Kind::unavailable, "connecting: refused"});
  EXPECT_EQ(run({"list-nodes"}), 0) << err.str();
  ASSERT_EQ(api_endpoints.size(), 4u);
  EXPECT_EQ(api_endpoints[0].first.host, "f1.example");
  EXPECT_EQ(api_endpoints[1].first.host, "f2.example");
  EXPECT_EQ(api_endpoints[2].first.host, "v1.example");
  EXPECT_EQ(api_endpoints[3].first.host, "v2.example");
  EXPECT_TRUE(api_endpoints[3].first.tls);
  EXPECT_EQ(run({"list-nodes", "--endpoint", "http://ok.example", "--endpoint", "not-a-url"}), 2) << "every endpoint given is checked first";
}

TEST_F(CtlTest, TheEndpointIsTheFlagThenTheVariableThenTheOneSaved) {
  files.put(home_credentials, fresh());
  variables["LOUHI_ENDPOINT"] = "https://env.example";
  EXPECT_EQ(run({"list-nodes", "--endpoint", "http://flag.example:9000", "--ca", "roots.pem"}), 0);
  EXPECT_EQ(run({"list-nodes"}), 0);
  variables["LOUHI_ENDPOINT"] = "";
  EXPECT_EQ(run({"list-nodes"}), 0);
  ASSERT_EQ(api_endpoints.size(), 3u);
  EXPECT_EQ(api_endpoints[0].first.host, "flag.example");
  EXPECT_EQ(api_endpoints[0].first.port, 9000);
  EXPECT_EQ(api_endpoints[0].second, "roots.pem");
  EXPECT_TRUE(api_endpoints[1].first.tls);
  EXPECT_EQ(api_endpoints[1].first.host, "env.example");
  EXPECT_EQ(api_endpoints[1].first.port, 443);
  EXPECT_EQ(api_endpoints[2].first.host, "louhi.example");
}

TEST_F(CtlTest, WithoutCredentialsACallSaysToLogIn) {
  EXPECT_EQ(run({"list-projects"}), 1);
  EXPECT_NE(err.str().find("run louhi-ctl login"), std::string::npos) << err.str();
  files.open(home_credentials)->write("not json");
  EXPECT_EQ(run({"list-projects"}), 1);
  EXPECT_NE(err.str().find("run louhi-ctl login"), std::string::npos) << err.str();
  EXPECT_TRUE(server->requests().empty());
}

TEST_F(CtlTest, ATokenWithAMinuteOrLessLeftIsRefreshedAndTheNewTokensKept) {
  auto c = fresh();
  c.expires_unix = now_unix + 60;
  files.put(home_credentials, c);
  std::map<std::string, std::string> asked;
  issuer_answers([&](const std::map<std::string, std::string>& form) {
    asked = form;
    return test::json_answer(200, {{"access_token", "a2"}, {"token_type", "Bearer"}, {"expires_in", 600}, {"refresh_token", "r2"}});
  });
  EXPECT_EQ(run({"list-projects", "--ca", "roots.pem"}), 0) << err.str();
  EXPECT_EQ(asked["grant_type"], "refresh_token");
  EXPECT_EQ(asked["refresh_token"], "r1");
  EXPECT_EQ(asked["client_id"], "louhi-ctl");
  EXPECT_EQ(issuer_roots, words({"roots.pem"}));
  ASSERT_EQ(server->requests().size(), 1u);
  EXPECT_EQ(server->requests()[0].token, "a2");

  auto kept = c;
  kept.access_token = "a2";
  kept.refresh_token = "r2";
  kept.expires_unix = now_unix + 600;
  EXPECT_EQ(files.at(home_credentials), kept) << "the spent refresh token is replaced";
}

TEST_F(CtlTest, ATokenWithMoreThanAMinuteLeftIsNotRefreshed) {
  auto c = fresh();
  c.expires_unix = now_unix + 61;
  files.put(home_credentials, c);
  EXPECT_EQ(run({"list-projects"}), 0);
  EXPECT_TRUE(https->sent().empty());
}

TEST_F(CtlTest, ARefreshRefusedSaysToLogInAndChangesNothing) {
  auto c = fresh();
  c.expires_unix = now_unix - 1;
  files.put(home_credentials, c);
  issuer_answers([](const std::map<std::string, std::string>&) { return test::json_answer(400, {{"error", "invalid_grant"}}); });
  EXPECT_EQ(run({"list-projects"}), 1);
  EXPECT_NE(err.str().find("invalid_grant"), std::string::npos) << err.str();
  EXPECT_NE(err.str().find("run louhi-ctl login"), std::string::npos) << err.str();
  EXPECT_EQ(files.at(home_credentials), c);
  EXPECT_TRUE(server->requests().empty());
}

TEST_F(CtlTest, AnExpiredTokenWithNoRefreshTokenSaysToLogIn) {
  auto c = fresh();
  c.expires_unix = now_unix - 1;
  c.refresh_token.clear();
  files.put(home_credentials, c);
  EXPECT_EQ(run({"list-projects"}), 1);
  EXPECT_NE(err.str().find("run louhi-ctl login"), std::string::npos) << err.str();
  EXPECT_TRUE(https->sent().empty());
  EXPECT_TRUE(server->requests().empty());
}

TEST_F(CtlTest, LoginSignsInByTheDeviceFlowAndKeepsTheTokens) {
  std::map<std::string, std::string> device_form;
  std::vector<std::map<std::string, std::string>> polls;
  issuer_answers(
      [&](const std::map<std::string, std::string>& form) {
        polls.push_back(form);
        if (polls.size() == 1) return test::json_answer(400, {{"error", "authorization_pending"}});
        return test::json_answer(200, {{"access_token", "a1"}, {"token_type", "Bearer"}, {"expires_in", 3600}, {"refresh_token", "r1"}});
      },
      [&](const std::map<std::string, std::string>& form) {
        device_form = form;
        return test::json_answer(200, {{"device_code", "d1"},
                                       {"user_code", "ABCD-EFGH"},
                                       {"verification_uri", "https://lugh.example/activate"},
                                       {"verification_uri_complete", "https://lugh.example/activate?code=ABCD-EFGH"},
                                       {"expires_in", 600},
                                       {"interval", 5}});
      });
  EXPECT_EQ(run({"login", "--issuer", issuer, "--client-id", "louhi-ctl", "--endpoint", "http://louhi.example:8080", "--scope", "louhi:web:read",
                 "--credentials", "/c/credentials.json"}),
            0)
      << err.str();

  EXPECT_EQ(device_form["client_id"], "louhi-ctl");
  EXPECT_EQ(device_form["scope"], "louhi:web:read");
  EXPECT_NE(err.str().find("https://lugh.example/activate"), std::string::npos) << err.str();
  EXPECT_NE(err.str().find("ABCD-EFGH"), std::string::npos) << err.str();
  ASSERT_EQ(polls.size(), 2u) << "polled again while the person had not answered";
  EXPECT_EQ(polls[1]["grant_type"], "urn:ietf:params:oauth:grant-type:device_code");
  EXPECT_EQ(polls[1]["device_code"], "d1");
  EXPECT_EQ(files.at("/c/credentials.json"), fresh());
  EXPECT_FALSE(files.at(home_credentials));
}

TEST_F(CtlTest, LoginKeepsEveryEndpointGiven) {
  issuer_answers(
      [](const std::map<std::string, std::string>&) {
        return test::json_answer(200, {{"access_token", "a1"}, {"token_type", "Bearer"}, {"expires_in", 3600}, {"refresh_token", "r1"}});
      },
      [](const std::map<std::string, std::string>&) {
        return test::json_answer(
            200, {{"device_code", "d1"}, {"user_code", "ABCD-EFGH"}, {"verification_uri", "https://lugh.example/activate"}, {"expires_in", 600}});
      });
  EXPECT_EQ(run({"login", "--issuer", issuer, "--client-id", "louhi-ctl", "--endpoint", "http://a.example:8080", "--endpoint", "https://b.example"}), 0)
      << err.str();
  ASSERT_TRUE(files.at(home_credentials));
  EXPECT_EQ(files.at(home_credentials)->endpoints, words({"http://a.example:8080", "https://b.example"}));
}

TEST_F(CtlTest, ALoginRefusedKeepsNothing) {
  issuer_answers([](const std::map<std::string, std::string>&) { return test::json_answer(400, {{"error", "access_denied"}}); },
                 [](const std::map<std::string, std::string>&) {
                   return test::json_answer(
                       200, {{"device_code", "d1"}, {"user_code", "ABCD-EFGH"}, {"verification_uri", "https://lugh.example/activate"}, {"expires_in", 600}});
                 });
  EXPECT_EQ(run({"login", "--issuer", issuer, "--client-id", "louhi-ctl", "--endpoint", "http://louhi.example:8080"}), 1);
  EXPECT_NE(err.str().find("access_denied"), std::string::npos) << err.str();
  EXPECT_FALSE(files.at(home_credentials));
}

TEST_F(CtlTest, LoginNeedsItsFlagsAndAnEndpointThatIsOne) {
  EXPECT_EQ(run({"login", "--issuer", issuer, "--client-id", "louhi-ctl"}), 2);
  EXPECT_EQ(run({"login", "--issuer", issuer, "--endpoint", "http://louhi.example"}), 2);
  EXPECT_EQ(run({"login", "--issuer", issuer, "--client-id", "louhi-ctl", "--endpoint", "louhi.example"}), 2);
  EXPECT_TRUE(https->sent().empty());
}

// louhi-ctl needs no ID token, and one it cannot verify would fail the sign-in
// after the person had answered: asking for one is a usage error, before
// anything is sent.
TEST_F(CtlTest, LoginRefusesTheOpenidScope) {
  EXPECT_EQ(run({"login", "--issuer", issuer, "--client-id", "louhi-ctl", "--endpoint", "http://louhi.example", "--scope", "openid"}), 2);
  EXPECT_EQ(
      run({"login", "--issuer", issuer, "--client-id", "louhi-ctl", "--endpoint", "http://louhi.example", "--scope", "louhi:web:read", "--scope", "openid"}),
      2);
  EXPECT_NE(err.str().find("openid"), std::string::npos) << err.str();
  EXPECT_TRUE(https->sent().empty());
  EXPECT_FALSE(files.at(home_credentials));
}

TEST_F(CtlTest, LogoutRemovesTheCredentials) {
  files.put(home_credentials, fresh());
  EXPECT_EQ(run({"logout"}), 0);
  EXPECT_FALSE(files.at(home_credentials));
  EXPECT_EQ(run({"logout"}), 0) << "none to remove is no error";
}
