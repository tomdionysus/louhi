//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// louhi-ctl (ctl/ctl.h; design, "API and sign-on"):
// - its settings are read by the stack's one reader: the configuration file
//   (--config, then $LOUHI_CONFIG, then $HOME's), then the environment, then
//   the flags; a flag unknown or with no value, a single-valued flag given
//   twice, and a configuration that cannot be read are usage errors;
// - each command makes its one call, its operands one path segment each,
//   and a command line it cannot act on exits 2 before anything is asked;
// - the credentials file is YAML, owner-only, replaced whole, read with a
//   bound, and found by --credentials, then $LOUHI_CREDENTIALS, then the
//   configuration, then $HOME; an older credentials.json beside the default
//   is moved to the YAML files once, so no one signs in again;
// - a call carries the access token held while more than a minute of it is
//   left; otherwise the refresh token is spent first and the new tokens
//   kept; a refresh refused says to log in and changes nothing;
// - the endpoints are --endpoint, then $LOUHI_ENDPOINT, then the
//   configuration's, each a node's API; a call goes to the next when one
//   cannot be reached, and a read also when one gave no answer, but a write
//   that may have arrived is never sent again; any HTTP answer is the answer;
// - the answer's JSON goes to stdout, and the exit is 0 for a 2xx answer, 1
//   for any other answer or failure;
// - login signs in by the device flow, telling the person where to go, and
//   keeps nothing unless it signed in: then it writes the settings to the
//   configuration and the tokens to the credentials; logout removes the
//   credentials.
// Driven by a fake issuer, a fake API, files in memory (or on disk, in a
// directory of the test's own) and stepped clocks: no sleep anywhere.

#include "louhi/ctl/ctl.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <yaml-cpp/yaml.h>

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

// louhi-ctl's files in memory, by path.
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
  std::optional<std::string> text(const std::string& path) { return File(*this, path).read(); }
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
  c.access_token = "a1";
  c.refresh_token = "r1";
  c.expires_unix = now_unix + 3600;
  return c;
}

std::vector<std::string> words(std::initializer_list<const char*> list) { return std::vector<std::string>(list.begin(), list.end()); }

ctl::Getenv lookup(const std::map<std::string, std::string>& variables) {
  return [&variables](const std::string& name) -> std::optional<std::string> {
    const auto it = variables.find(name);
    if (it == variables.end()) return std::nullopt;
    return it->second;
  };
}

// The command line alone: no variables, so no configuration file either.
Args args(std::initializer_list<const char*> list) {
  return ctl::read_args(words(list), [](const std::string&) -> std::optional<std::string> { return std::nullopt; });
}

void write_file(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

std::string endpoints_yaml(const std::vector<std::string>& endpoints) {
  std::string out = "endpoints:\n";
  for (const auto& e : endpoints) out += "  - " + e + "\n";
  return out;
}

// An older louhi-ctl's credentials.json, the tokens fresh() holds.
std::string old_json(const std::string& endpoints) {
  return R"({"issuer":"https://lugh.example","client_id":"louhi-ctl",)" + endpoints +
         R"(,"scopes":["louhi:web:read"],"access_token":"a1","refresh_token":"r1","expires_unix":)" + std::to_string(now_unix + 3600) + "}";
}

unsigned mode_of(const std::filesystem::path& path) {
  struct stat info{};
  if (::stat(path.c_str(), &info) != 0) return ~0u;
  return info.st_mode & 0777;
}

// What the command reads from a file operand: the file's name, as a marker.
std::string read_marker(const std::string& path) { return "<" + path + ">"; }

struct CtlTest : ::testing::Test {
  // $HOME: the configuration is read from disk by Dannan's reader.
  dannan::test::TempDir home;
  const std::string home_config = (home.path() / ".config" / "louhi" / "config.yaml").string();
  const std::string home_credentials = (home.path() / ".config" / "louhi" / "credentials.yaml").string();
  const std::string old_credentials = (home.path() / ".config" / "louhi" / "credentials.json").string();

  asio::io_context io;
  asio::executor_work_guard<asio::io_context::executor_type> work = asio::make_work_guard(io);
  std::thread thread{[this] { io.run(); }};

  std::shared_ptr<test::FakeHttps> https = std::make_shared<test::FakeHttps>();
  std::shared_ptr<FakeApi> server = std::make_shared<FakeApi>();
  Files files;
  // The files louhi-ctl writes go to `files`, or to disk when this is set.
  bool on_disk = false;
  std::map<std::string, std::string> variables{{"HOME", home.path().string()}};
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
    env.file = [this](const std::filesystem::path& path) -> std::unique_ptr<ctl::CredentialFile> {
      if (on_disk) return std::make_unique<ctl::DiskCredentialFile>(path);
      return files.open(path);
    };
    env.read = read_marker;
    env.getenv = lookup(variables);
    env.wall = &wall;
    env.steady = &steady;
    env.executor = io.get_executor();
    env.out = &out;
    env.err = &err;
    return ctl::run(words(list), env);
  }

  // Signed in: the tokens in the credentials, the endpoints in $HOME's
  // configuration.
  void signed_in(const Credentials& c = fresh(), const std::vector<std::string>& endpoints = {"http://louhi.example:8080"}) {
    files.put(home_credentials, c);
    write_file(home_config, endpoints_yaml(endpoints));
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

  // The issuer signs the person in at the first poll.
  void issuer_signs_in() {
    issuer_answers(
        [](const std::map<std::string, std::string>&) {
          return test::json_answer(200, {{"access_token", "a1"}, {"token_type", "Bearer"}, {"expires_in", 3600}, {"refresh_token", "r1"}});
        },
        [](const std::map<std::string, std::string>&) {
          return test::json_answer(
              200, {{"device_code", "d1"}, {"user_code", "ABCD-EFGH"}, {"verification_uri", "https://lugh.example/activate"}, {"expires_in", 600}});
        });
  }

  // The configuration login left in memory.
  YAML::Node saved_config() {
    const auto text = files.text(home_config);
    return text ? YAML::Load(*text) : YAML::Node();
  }
};

}  // namespace

TEST(CtlArgs, FlagsTakeTheirValuesAndTheRestStaysInOrder) {
  const auto a = args({"run-task", "--count", "3", "web", "--scope", "x", "frontend", "--scope", "y"});
  EXPECT_EQ(a.positional, words({"run-task", "web", "frontend"}));
  EXPECT_EQ(a.flag("command.count"), "3");
  EXPECT_EQ(a.all("scopes"), words({"x", "y"}));
  EXPECT_FALSE(a.flag("command.revision"));
  EXPECT_EQ(args({"list-tasks", "web", "--limit=10"}).flag("command.limit"), "10");
  EXPECT_EQ(args({"--version"}).settings.toggle("command.version"), true);
  EXPECT_EQ(args({"put-quota", "web", "-"}).positional, words({"put-quota", "web", "-"})) << "\"-\" is the operand for stdin, not a flag";
  EXPECT_EQ(args({"put-project", "web", "--description", "-"}).flag("command.description"), "-");
}

TEST(CtlArgs, AFlagWithNoValueOrGivenTwiceWhereOneIsWantedIsAUsageError) {
  EXPECT_THROW(args({"list-tasks", "web", "--limit"}), ctl::Usage);
  EXPECT_THROW(args({"run-task", "web", "frontend", "--count", "1", "--count", "2"}), ctl::Usage);
  EXPECT_THROW(args({"run-task", "web", "frontend", "--count=1", "--count", "2"}), ctl::Usage);
  EXPECT_NO_THROW(args({"list-nodes", "--endpoint", "http://a.example", "--endpoint", "http://b.example"})) << "a list's flag repeats";
}

TEST(CtlArgs, AnUnknownFlagIsAUsageError) {
  EXPECT_THROW(args({"list-projects", "--endpiont", "http://a.example"}), ctl::Usage);
  EXPECT_THROW(args({"list-projects", "-x"}), ctl::Usage);
}

TEST(CtlArgs, ASettingIsTheFlagThenTheVariableThenTheConfiguration) {
  dannan::test::TempDir dir;
  write_file(dir.path() / ".config" / "louhi" / "config.yaml",
             "issuer: https://file.example\nclient_id: from-file\nca: file.pem\ncredentials: /file/credentials.yaml\nscopes: [a, b]\n" +
                 endpoints_yaml({"http://file1.example", "http://file2.example"}));
  std::map<std::string, std::string> variables{{"HOME", dir.path().string()}};
  const auto getenv = lookup(variables);

  auto a = ctl::read_args(words({"list-nodes"}), getenv);
  EXPECT_EQ(a.flag("issuer"), "https://file.example");
  EXPECT_EQ(a.flag("client_id"), "from-file");
  EXPECT_EQ(a.flag("ca"), "file.pem");
  EXPECT_EQ(a.all("scopes"), words({"a", "b"}));
  EXPECT_EQ(a.all("endpoints"), words({"http://file1.example", "http://file2.example"})) << "a list from YAML";
  EXPECT_EQ(ctl::credentials_path(a, getenv), "/file/credentials.yaml");

  variables["LOUHI_ENDPOINT"] = "http://v1.example, http://v2.example";
  variables["LOUHI_CREDENTIALS"] = "/variable/credentials.yaml";
  a = ctl::read_args(words({"list-nodes"}), getenv);
  EXPECT_EQ(a.all("endpoints"), words({"http://v1.example", "http://v2.example"})) << "a list from a variable";
  EXPECT_EQ(ctl::credentials_path(a, getenv), "/variable/credentials.yaml");
  EXPECT_EQ(a.flag("issuer"), "https://file.example");

  a = ctl::read_args(words({"list-nodes", "--endpoint", "http://f1.example", "--endpoint=http://f2.example", "--credentials", "/flag/credentials.yaml",
                            "--issuer", "https://flag.example", "--ca", "flag.pem"}),
                     getenv);
  EXPECT_EQ(a.all("endpoints"), words({"http://f1.example", "http://f2.example"})) << "a list from repeated flags";
  EXPECT_EQ(ctl::credentials_path(a, getenv), "/flag/credentials.yaml");
  EXPECT_EQ(a.flag("issuer"), "https://flag.example");
  EXPECT_EQ(a.flag("ca"), "flag.pem");

  variables["LOUHI_ENDPOINT"] = "";
  EXPECT_EQ(ctl::read_args(words({"list-nodes"}), getenv).all("endpoints"), words({"http://file1.example", "http://file2.example"}))
      << "a variable set but empty is unset";
}

TEST(CtlArgs, TheConfigurationIsTheFlagsThenTheVariablesThenHomes) {
  dannan::test::TempDir dir;
  write_file(dir.path() / ".config" / "louhi" / "config.yaml", "issuer: https://home.example\n");
  write_file(dir.path() / "variable.yaml", "issuer: https://variable.example\n");
  write_file(dir.path() / "flag.yaml", "issuer: https://flag.example\n");
  std::map<std::string, std::string> variables{{"HOME", dir.path().string()}};
  const auto getenv = lookup(variables);
  EXPECT_EQ(ctl::read_args(words({"list-nodes"}), getenv).flag("issuer"), "https://home.example");
  variables["LOUHI_CONFIG"] = (dir.path() / "variable.yaml").string();
  EXPECT_EQ(ctl::read_args(words({"list-nodes"}), getenv).flag("issuer"), "https://variable.example");
  const auto flag = (dir.path() / "flag.yaml").string();
  EXPECT_EQ(ctl::read_args({"list-nodes", "--config", flag}, getenv).flag("issuer"), "https://flag.example");
}

TEST(CtlArgs, AConfigurationThatCannotBeReadIsAUsageError) {
  dannan::test::TempDir dir;
  const auto path = dir.path() / ".config" / "louhi" / "config.yaml";
  std::map<std::string, std::string> variables{{"HOME", dir.path().string()}};
  const auto getenv = lookup(variables);
  for (const char* bad : {"endpoints: [http://a.example\n", "endpoints: http://a.example\n", "- a\n- b\n", "issuer: [a, b]\n"}) {
    write_file(path, bad);
    EXPECT_THROW(ctl::read_args(words({"list-nodes"}), getenv), ctl::Usage) << bad;
  }
  std::filesystem::remove(path);
  EXPECT_THROW(ctl::read_args({"list-nodes", "--config", (dir.path() / "none.yaml").string()}, getenv), ctl::Usage) << "a file named must exist";
  variables["LOUHI_CONFIG"] = (dir.path() / "none.yaml").string();
  EXPECT_THROW(ctl::read_args(words({"list-nodes"}), getenv), ctl::Usage);
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
  const ctl::Getenv none = [](const std::string&) -> std::optional<std::string> { return std::nullopt; };
  for (const auto& list : {words({"list-projects", "extra"}), words({"describe-project"}), words({"put-quota", "web"}), words({"describe-task", "web"}),
                           words({"describe-task-definition", "web", "frontend"}), words({"stop-task", "web", "t1", "t2"}), words({"launch", "web"})}) {
    EXPECT_THROW(ctl::command(ctl::read_args(list, none), read_marker), ctl::Usage) << list.front();
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
  auto awkward = c;
  awkward.access_token = "123";
  awkward.refresh_token = "a: b # c";
  awkward.client_id = "";
  EXPECT_EQ(ctl::decode(ctl::encode(awkward)), awkward) << "a value YAML would read as another type is still text";
}

TEST(CtlCredentials, AFieldOfTheWrongTypeIsEmptyAndAnExpiryThatIsNotOneIsNone) {
  const auto c = ctl::decode("issuer: [7]\nclient_id: louhi-ctl\naccess_token: {a: 1}\nrefresh_token:\nexpires_unix: -5\n");
  EXPECT_EQ(c.issuer, "");
  EXPECT_EQ(c.client_id, "louhi-ctl");
  EXPECT_EQ(c.access_token, "");
  EXPECT_EQ(c.refresh_token, "");
  EXPECT_EQ(c.expires_unix, 0u);
  EXPECT_EQ(ctl::decode("expires_unix: 1.5\n").expires_unix, 0u);
  EXPECT_EQ(ctl::decode("expires_unix: soon\n").expires_unix, 0u);
  EXPECT_EQ(ctl::decode("expires_unix: 99999999999999999999\n").expires_unix, 0u);
  EXPECT_EQ(ctl::decode("expires_unix: 1800000000\n").expires_unix, 1'800'000'000u);
}

TEST(CtlCredentials, TextThatIsNotAMapIsRefused) {
  for (const char* bad : {"", "[]", "x", "{", "- a\n- b\n", "a: [b\n"}) EXPECT_THROW(ctl::decode(bad), std::runtime_error) << bad;
}

TEST(CtlCredentials, AreFoundByTheFlagThenTheVariableThenHome) {
  std::map<std::string, std::string> variables{{"HOME", "/home/user"}, {"LOUHI_CREDENTIALS", "/etc/louhi/c.yaml"}};
  const auto getenv = lookup(variables);
  const auto path = [&](std::initializer_list<const char*> list) { return ctl::credentials_path(ctl::read_args(words(list), getenv), getenv); };
  EXPECT_EQ(path({"--credentials", "/x/c.yaml"}), "/x/c.yaml");
  EXPECT_EQ(path({}), "/etc/louhi/c.yaml");
  variables.erase("LOUHI_CREDENTIALS");
  EXPECT_EQ(path({}), "/home/user/.config/louhi/credentials.yaml");
  variables.erase("HOME");
  EXPECT_THROW(path({}), std::runtime_error);
}

TEST(CtlCredentials, OnDiskTheyAreOwnerOnlyAndReplacedWhole) {
  dannan::test::TempDir dir;
  const auto path = dir.path() / "config" / "louhi" / "credentials.yaml";
  ctl::DiskCredentialFile file(path);
  EXPECT_FALSE(file.read()) << "no file is nothing, not an error";

  file.write(ctl::encode(fresh()));
  EXPECT_EQ(mode_of(path), 0600u);
  EXPECT_EQ(mode_of(path.parent_path()) & 0077, 0u) << "a directory made for them is its owner's alone";
  EXPECT_EQ(ctl::decode(*file.read()), fresh());

  std::filesystem::permissions(path, std::filesystem::perms::others_read | std::filesystem::perms::group_read, std::filesystem::perm_options::add);
  file.write("{}\n");
  EXPECT_EQ(mode_of(path), 0600u) << "owner-only after every write, whatever the file was";
  EXPECT_EQ(file.read(), "{}\n");
  file.remove();
  EXPECT_FALSE(file.read());
  EXPECT_NO_THROW(file.remove());
}

TEST(CtlCredentials, OnDiskAFileTooLargeToBeThemIsRefused) {
  dannan::test::TempDir dir;
  const auto path = dir.path() / "credentials.yaml";
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
  signed_in();
  EXPECT_EQ(run({"list-projects", "--endpoint", "ftp://louhi.example"}), 2);
  EXPECT_EQ(run({"list-projects", "--frobnicate", "yes"}), 2);
  EXPECT_EQ(run({"list-projects", "--endpoint"}), 2);
  EXPECT_EQ(run({"logout", "now"}), 2);
  EXPECT_TRUE(server->requests().empty());
  EXPECT_TRUE(https->sent().empty());
  EXPECT_TRUE(files.at(home_credentials)) << "a usage error removes nothing";
}

TEST_F(CtlTest, AConfigurationThatCannotBeReadOrUsedExitsTwo) {
  files.put(home_credentials, fresh());
  write_file(home_config, "endpoints: [http://louhi.example\n");
  EXPECT_EQ(run({"list-projects"}), 2);
  EXPECT_NE(err.str().find(home_config), std::string::npos) << "the file is named: " << err.str();
  write_file(home_config, endpoints_yaml({"louhi.example"}));
  EXPECT_EQ(run({"list-projects"}), 2) << "an endpoint that is not one";
  EXPECT_NE(err.str().find(home_config), std::string::npos) << err.str();
  EXPECT_TRUE(server->requests().empty());
}

TEST_F(CtlTest, ACallCarriesTheTokenHeldAndPrintsTheAnswer) {
  signed_in();
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
  signed_in();
  EXPECT_EQ(run({"register-task-definition", "web", "def.json"}), 0);
  ASSERT_EQ(server->requests().size(), 1u);
  EXPECT_EQ(server->requests()[0].body, "<def.json>");
}

TEST_F(CtlTest, AnAnswerThatIsNot2xxIsPrintedAndExitsOne) {
  signed_in();
  server->queue(api::Response{404, json::object{{"error", "not_found"}, {"message", "no project web"}}});
  EXPECT_EQ(run({"describe-project", "web"}), 1);
  EXPECT_EQ(out.str(), "{\"error\":\"not_found\",\"message\":\"no project web\"}\n");
}

TEST_F(CtlTest, AnApiThatCannotBeReachedExitsOneAndSaysWhy) {
  signed_in();
  server->queue(runtime::Error{runtime::Error::Kind::unavailable, "connection refused"});
  EXPECT_EQ(run({"list-projects"}), 1);
  EXPECT_EQ(out.str(), "");
  EXPECT_NE(err.str().find("connection refused"), std::string::npos) << err.str();
}

// Any node serves the API, so a node down is not the person's problem.
TEST_F(CtlTest, ACallGoesToTheNextEndpointWhenOneCannotBeReached) {
  signed_in(fresh(), {"http://a.example:8080", "http://b.example:8080", "http://c.example:8080"});
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
  signed_in(fresh(), {"http://a.example:8080", "http://b.example:8080"});
  server->queue(runtime::Error{runtime::Error::Kind::timed_out, "no whole answer within the deadline"});
  EXPECT_EQ(run({"list-projects"}), 0) << err.str();
  EXPECT_EQ(server->requests().size(), 2u);
  server->queue(runtime::Error{runtime::Error::Kind::malformed, "the answer is not HTTP"});
  EXPECT_EQ(run({"describe-project", "web"}), 0) << err.str();
  EXPECT_EQ(server->requests().size(), 4u);
}

TEST_F(CtlTest, AWriteThatMayHaveArrivedIsNeverSentAgain) {
  signed_in(fresh(), {"http://a.example:8080", "http://b.example:8080"});
  server->queue(runtime::Error{runtime::Error::Kind::timed_out, "no whole answer within the deadline"});
  EXPECT_EQ(run({"run-task", "web", "frontend"}), 1);
  EXPECT_EQ(server->requests().size(), 1u);
  EXPECT_NE(err.str().find("a.example"), std::string::npos) << err.str();
  EXPECT_NE(err.str().find("may have"), std::string::npos) << "the person is told it may have happened: " << err.str();
}

TEST_F(CtlTest, AnHttpAnswerIsTheAnswerWhateverItSays) {
  signed_in(fresh(), {"http://a.example:8080", "http://b.example:8080"});
  server->queue(api::Response{503, json::object{{"error", "not_ready"}}});
  EXPECT_EQ(run({"list-projects"}), 1);
  EXPECT_EQ(out.str(), "{\"error\":\"not_ready\"}\n");
  EXPECT_EQ(server->requests().size(), 1u);
}

TEST_F(CtlTest, WhenNoEndpointAnswersEachIsNamedWithWhy) {
  signed_in(fresh(), {"http://a.example:8080", "http://b.example:8080"});
  server->queue(runtime::Error{runtime::Error::Kind::unavailable, "connecting: refused"});
  server->queue(runtime::Error{runtime::Error::Kind::unavailable, "connecting: no route"});
  EXPECT_EQ(run({"list-projects"}), 1);
  for (const char* expected : {"a.example", "refused", "b.example", "no route"}) EXPECT_NE(err.str().find(expected), std::string::npos) << err.str();
}

TEST_F(CtlTest, EndpointsComeFromRepeatedFlagsOrAListInTheVariable) {
  signed_in();
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

TEST_F(CtlTest, TheEndpointIsTheFlagThenTheVariableThenTheConfiguration) {
  signed_in();
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
  write_file(home_config, endpoints_yaml({"http://louhi.example:8080"}));
  for (const char* bad : {"not yaml at all", "{unclosed", "- a list\n"}) {
    files.open(home_credentials)->write(bad);
    EXPECT_EQ(run({"list-projects"}), 1) << bad;
    EXPECT_NE(err.str().find("run louhi-ctl login"), std::string::npos) << err.str();
  }
  EXPECT_TRUE(server->requests().empty());
}

TEST_F(CtlTest, ATokenWithAMinuteOrLessLeftIsRefreshedAndTheNewTokensKept) {
  auto c = fresh();
  c.expires_unix = now_unix + 60;
  signed_in(c);
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
  signed_in(c);
  EXPECT_EQ(run({"list-projects"}), 0);
  EXPECT_TRUE(https->sent().empty());
}

TEST_F(CtlTest, ARefreshRefusedSaysToLogInAndChangesNothing) {
  auto c = fresh();
  c.expires_unix = now_unix - 1;
  signed_in(c);
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
  signed_in(c);
  EXPECT_EQ(run({"list-projects"}), 1);
  EXPECT_NE(err.str().find("run louhi-ctl login"), std::string::npos) << err.str();
  EXPECT_TRUE(https->sent().empty());
  EXPECT_TRUE(server->requests().empty());
}

TEST_F(CtlTest, LoginSignsInByTheDeviceFlowAndKeepsTheSettingsAndTheTokens) {
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
                 "--credentials", "/c/credentials.yaml"}),
            0)
      << err.str();

  EXPECT_EQ(device_form["client_id"], "louhi-ctl");
  EXPECT_EQ(device_form["scope"], "louhi:web:read");
  EXPECT_NE(err.str().find("https://lugh.example/activate"), std::string::npos) << err.str();
  EXPECT_NE(err.str().find("ABCD-EFGH"), std::string::npos) << err.str();
  ASSERT_EQ(polls.size(), 2u) << "polled again while the person had not answered";
  EXPECT_EQ(polls[1]["grant_type"], "urn:ietf:params:oauth:grant-type:device_code");
  EXPECT_EQ(polls[1]["device_code"], "d1");
  EXPECT_EQ(files.at("/c/credentials.yaml"), fresh());
  EXPECT_FALSE(files.at(home_credentials));

  const YAML::Node saved = saved_config();
  ASSERT_TRUE(saved.IsMap()) << "the configuration is $HOME's, there being no other";
  EXPECT_EQ(saved["issuer"].as<std::string>(), issuer);
  EXPECT_EQ(saved["client_id"].as<std::string>(), "louhi-ctl");
  EXPECT_EQ(saved["endpoints"].as<std::vector<std::string>>(), words({"http://louhi.example:8080"}));
  EXPECT_EQ(saved["scopes"].as<std::vector<std::string>>(), words({"louhi:web:read"}));
  EXPECT_FALSE(saved["ca"].IsDefined()) << "none was given";
  EXPECT_FALSE(saved["credentials"].IsDefined()) << "a --credentials given is for that run";
}

TEST_F(CtlTest, LoginKeepsEveryEndpointGivenAndTheCa) {
  issuer_signs_in();
  EXPECT_EQ(run({"login", "--issuer", issuer, "--client-id", "louhi-ctl", "--endpoint", "http://a.example:8080", "--endpoint", "https://b.example", "--ca",
                 "roots.pem"}),
            0)
      << err.str();
  ASSERT_TRUE(files.at(home_credentials));
  const YAML::Node saved = saved_config();
  ASSERT_TRUE(saved.IsMap());
  EXPECT_EQ(saved["endpoints"].as<std::vector<std::string>>(), words({"http://a.example:8080", "https://b.example"}));
  EXPECT_EQ(saved["scopes"].as<std::vector<std::string>>(), std::vector<std::string>{});
  EXPECT_EQ(saved["ca"].as<std::string>(), "roots.pem");
  EXPECT_EQ(issuer_roots, words({"roots.pem"}));
}

// The configuration in force is replaced whole with what signed in: its own
// settings serve as login's, and a credentials path it holds stays in it.
TEST_F(CtlTest, LoginTakesItsSettingsFromTheConfigurationAndWritesThatFile) {
  write_file(home_config, std::string("issuer: ") + issuer + "\nclient_id: louhi-ctl\ncredentials: /kept/credentials.yaml\n" +
                              endpoints_yaml({"http://louhi.example:8080"}));
  issuer_signs_in();
  EXPECT_EQ(run({"login", "--scope", "louhi:web:read"}), 0) << err.str();
  EXPECT_EQ(files.at("/kept/credentials.yaml"), fresh());
  const YAML::Node saved = saved_config();
  ASSERT_TRUE(saved.IsMap());
  EXPECT_EQ(saved["issuer"].as<std::string>(), issuer);
  EXPECT_EQ(saved["endpoints"].as<std::vector<std::string>>(), words({"http://louhi.example:8080"}));
  EXPECT_EQ(saved["scopes"].as<std::vector<std::string>>(), words({"louhi:web:read"}));
  EXPECT_EQ(saved["credentials"].as<std::string>(), "/kept/credentials.yaml");
}

TEST_F(CtlTest, LoginOnDiskWritesBothFilesOwnerOnlyAndACallNeedsNothingMore) {
  on_disk = true;
  issuer_signs_in();
  EXPECT_EQ(run({"login", "--issuer", issuer, "--client-id", "louhi-ctl", "--endpoint", "http://louhi.example:8080", "--scope", "louhi:web:read"}), 0)
      << err.str();
  EXPECT_EQ(mode_of(home_config), 0600u);
  EXPECT_EQ(mode_of(home_credentials), 0600u);
  EXPECT_EQ(ctl::decode(*ctl::DiskCredentialFile(home_credentials).read()), fresh());

  EXPECT_EQ(run({"list-projects"}), 0) << err.str();
  ASSERT_EQ(api_endpoints.size(), 1u);
  EXPECT_EQ(api_endpoints[0].first.host, "louhi.example");
  ASSERT_EQ(server->requests().size(), 1u);
  EXPECT_EQ(server->requests()[0].token, "a1");
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
  EXPECT_FALSE(files.text(home_config));
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
  signed_in();
  files.open(old_credentials)->write(old_json(R"("endpoints":["http://louhi.example:8080"])"));
  EXPECT_EQ(run({"logout"}), 0);
  EXPECT_FALSE(files.at(home_credentials));
  EXPECT_FALSE(files.text(old_credentials)) << "an older file is credentials too";
  EXPECT_EQ(run({"logout"}), 0) << "none to remove is no error";
}

// An older louhi-ctl kept the settings and the tokens together in
// credentials.json: they are moved, and the call goes on without a login.
TEST_F(CtlTest, AnOlderJsonFileIsMovedToTheYamlFilesOnce) {
  on_disk = true;
  write_file(old_credentials, old_json(R"("endpoints":["http://a.example:8080","http://b.example:8080"])"));
  EXPECT_EQ(run({"list-projects"}), 0) << err.str();
  EXPECT_NE(err.str().find("moved"), std::string::npos) << err.str();
  ASSERT_EQ(server->requests().size(), 1u);
  EXPECT_EQ(server->requests()[0].token, "a1");
  ASSERT_EQ(api_endpoints.size(), 1u);
  EXPECT_EQ(api_endpoints[0].first.host, "a.example");

  EXPECT_FALSE(std::filesystem::exists(old_credentials));
  EXPECT_EQ(mode_of(home_credentials), 0600u);
  EXPECT_EQ(ctl::decode(*ctl::DiskCredentialFile(home_credentials).read()), fresh());
  const YAML::Node saved = YAML::LoadFile(home_config);
  EXPECT_EQ(saved["issuer"].as<std::string>(), issuer);
  EXPECT_EQ(saved["client_id"].as<std::string>(), "louhi-ctl");
  EXPECT_EQ(saved["endpoints"].as<std::vector<std::string>>(), words({"http://a.example:8080", "http://b.example:8080"}));
  EXPECT_EQ(saved["scopes"].as<std::vector<std::string>>(), words({"louhi:web:read"}));

  EXPECT_EQ(run({"list-projects"}), 0) << err.str();
  EXPECT_EQ(err.str().find("moved"), std::string::npos) << "moved once: " << err.str();
  ASSERT_EQ(api_endpoints.size(), 2u);
  EXPECT_EQ(api_endpoints[1].first.host, "a.example");
}

TEST_F(CtlTest, AnOlderFilesSingleEndpointIsMovedAsAList) {
  files.open(old_credentials)->write(old_json(R"("endpoint":"http://old.example:8080")"));
  EXPECT_EQ(run({"list-projects"}), 0) << err.str();
  ASSERT_EQ(api_endpoints.size(), 1u);
  EXPECT_EQ(api_endpoints[0].first.host, "old.example");
  EXPECT_EQ(files.at(home_credentials), fresh());
  EXPECT_EQ(saved_config()["endpoints"].as<std::vector<std::string>>(), words({"http://old.example:8080"}));
  EXPECT_FALSE(files.text(old_credentials));
}

TEST_F(CtlTest, MovingAnOlderFileKeepsAConfigurationThereAlready) {
  write_file(home_config, endpoints_yaml({"http://configured.example:8080"}));
  files.open(old_credentials)->write(old_json(R"("endpoints":["http://old.example:8080"])"));
  EXPECT_EQ(run({"list-projects"}), 0) << err.str();
  ASSERT_EQ(api_endpoints.size(), 1u);
  EXPECT_EQ(api_endpoints[0].first.host, "configured.example");
  EXPECT_EQ(files.at(home_credentials), fresh());
  EXPECT_FALSE(files.text(home_config)) << "the configuration is not written";
  EXPECT_FALSE(files.text(old_credentials));
}

TEST_F(CtlTest, OnlyTheDefaultCredentialsAreMoved) {
  write_file(home_config, endpoints_yaml({"http://louhi.example:8080"}));
  files.open("/x/credentials.json")->write(old_json(R"("endpoints":["http://old.example:8080"])"));
  EXPECT_EQ(run({"list-projects", "--credentials", "/x/credentials.yaml"}), 1);
  EXPECT_NE(err.str().find("run louhi-ctl login"), std::string::npos) << err.str();
  EXPECT_TRUE(files.text("/x/credentials.json"));
  EXPECT_TRUE(server->requests().empty());
}

TEST_F(CtlTest, AnOlderFileThatCannotBeReadSaysToLogIn) {
  files.open(old_credentials)->write("not json");
  EXPECT_EQ(run({"list-projects"}), 1);
  EXPECT_NE(err.str().find("run louhi-ctl login"), std::string::npos) << err.str();
  EXPECT_TRUE(server->requests().empty());
}
