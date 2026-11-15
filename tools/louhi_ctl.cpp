//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// louhi-ctl: the API from a terminal (design, "API and sign-on"). It signs in
// to a Lugh by the device flow (RFC 8628), keeps the tokens in a credentials
// file only its owner can read, refreshes the access token when it has run
// out, and prints each answer's JSON on stdout. It exits 0 for a 2xx answer,
// 1 for any other answer or failure, 2 for a usage error.
//
//   louhi-ctl login --issuer URL --client-id ID --endpoint URL [--scope S]... [--ca FILE]
//   louhi-ctl logout
//   louhi-ctl list-projects
//   louhi-ctl put-project PROJECT [--description TEXT]
//   louhi-ctl describe-project PROJECT
//   louhi-ctl put-quota PROJECT FILE            a JSON quota; - reads stdin
//   louhi-ctl describe-quota PROJECT
//   louhi-ctl register-task-definition PROJECT FILE
//   louhi-ctl list-task-definition-families PROJECT
//   louhi-ctl list-task-definitions PROJECT FAMILY
//   louhi-ctl describe-task-definition PROJECT FAMILY REVISION
//   louhi-ctl run-task PROJECT FAMILY [--revision N] [--count N] [--client-token T]
//   louhi-ctl list-tasks PROJECT [--limit N] [--next ID]
//   louhi-ctl describe-task PROJECT TASK
//   louhi-ctl stop-task PROJECT TASK [--reason TEXT]
//   louhi-ctl list-nodes
//   louhi-ctl --version
//
// Before any command: --endpoint URL (else the one saved at login, or
// $LOUHI_ENDPOINT), --ca FILE (the roots an https endpoint and the issuer
// chain to; the system's when absent), --credentials FILE (else
// $LOUHI_CREDENTIALS, else ~/.config/louhi/credentials.json).

#include <sys/stat.h>

#include <boost/asio/io_context.hpp>
#include <boost/json.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "dannan/contract/time_source.h"
#include "dannan/encoding/percent.h"
#include "dannan/identity/secure_memory.h"
#include "louhi/api/client.h"
#include "louhi/version.h"
#include "lugh/client/https.h"
#include "lugh/client/oauth_client.h"
#include "lugh/client/verifier.h"

namespace {

namespace asio = boost::asio;
namespace json = boost::json;
using namespace std::chrono_literals;

struct Usage : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// The command line: flags (each --name VALUE, some repeatable) and the rest.
struct Args {
  std::map<std::string, std::vector<std::string>> flags;
  std::vector<std::string> positional;

  std::optional<std::string> flag(const std::string& name) const {
    const auto it = flags.find(name);
    if (it == flags.end() || it->second.empty()) return std::nullopt;
    if (it->second.size() > 1) throw Usage("--" + name + " is given more than once");
    return it->second.front();
  }
  std::vector<std::string> all(const std::string& name) const {
    const auto it = flags.find(name);
    return it == flags.end() ? std::vector<std::string>{} : it->second;
  }
};

Args parse(int argc, char** argv) {
  Args out;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--version") {
      out.flags["version"].push_back("");
    } else if (arg.rfind("--", 0) == 0) {
      if (i + 1 >= argc) throw Usage(arg + " needs a value");
      out.flags[arg.substr(2)].push_back(argv[++i]);
    } else {
      out.positional.push_back(arg);
    }
  }
  return out;
}

std::string read_file(const std::string& path) {
  if (path == "-") return std::string(std::istreambuf_iterator<char>(std::cin), {});
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read " + path);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

std::filesystem::path credentials_path(const Args& args) {
  if (const auto given = args.flag("credentials")) return *given;
  if (const char* env = std::getenv("LOUHI_CREDENTIALS")) return env;
  const char* home = std::getenv("HOME");
  if (home == nullptr) throw std::runtime_error("no HOME, so give --credentials");
  return std::filesystem::path(home) / ".config" / "louhi" / "credentials.json";
}

// What login keeps: where to sign in again and the tokens. The file holds
// bearer credentials, so it is made readable by its owner only.
struct Credentials {
  std::string issuer;
  std::string client_id;
  std::string endpoint;
  std::vector<std::string> scopes;
  std::string access_token;
  std::string refresh_token;
  uint64_t expires_unix = 0;
};

Credentials load(const std::filesystem::path& path) {
  json::value value;
  try {
    value = json::parse(read_file(path.string()));
  } catch (const std::exception&) {
    throw std::runtime_error("no credentials at " + path.string() + ": run louhi-ctl login");
  }
  const auto& o = value.as_object();
  Credentials c;
  const auto text = [&](const char* key) { return o.contains(key) && o.at(key).is_string() ? std::string(o.at(key).as_string()) : std::string(); };
  c.issuer = text("issuer");
  c.client_id = text("client_id");
  c.endpoint = text("endpoint");
  c.access_token = text("access_token");
  c.refresh_token = text("refresh_token");
  if (o.contains("expires_unix") && o.at("expires_unix").is_number()) c.expires_unix = o.at("expires_unix").to_number<uint64_t>();
  if (o.contains("scopes") && o.at("scopes").is_array())
    for (const auto& s : o.at("scopes").as_array())
      if (s.is_string()) c.scopes.emplace_back(s.as_string());
  return c;
}

void save(const std::filesystem::path& path, const Credentials& c) {
  std::filesystem::create_directories(path.parent_path());
  json::array scopes;
  for (const auto& s : c.scopes) scopes.emplace_back(s);
  const json::object o{{"issuer", c.issuer},
                       {"client_id", c.client_id},
                       {"endpoint", c.endpoint},
                       {"scopes", scopes},
                       {"access_token", c.access_token},
                       {"refresh_token", c.refresh_token},
                       {"expires_unix", c.expires_unix}};
  const auto temporary = path.string() + ".new";
  {
    // Made owner-only before a byte is written.
    const mode_t before = ::umask(077);
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    ::umask(before);
    if (!out) throw std::runtime_error("cannot write " + temporary);
    out << json::serialize(o) << "\n";
  }
  ::chmod(temporary.c_str(), 0600);
  std::filesystem::rename(temporary, path);
}

uint64_t now_unix() { return dannan::contract::system_wall_clock().unix_ms() / 1000; }

// One io context run on a thread of its own while a command waits on it.
struct Io {
  asio::io_context io;
  asio::executor_work_guard<asio::io_context::executor_type> work = asio::make_work_guard(io);
  std::thread thread{[this] { io.run(); }};
  ~Io() {
    work.reset();
    io.stop();
    thread.join();
  }
};

template <typename T>
T wait(std::future<T>& future, std::chrono::seconds most) {
  if (future.wait_for(most) != std::future_status::ready) throw std::runtime_error("no answer in time");
  return future.get();
}

std::string secret_text(const std::shared_ptr<const dannan::identity::Secret>& secret) {
  if (!secret) return {};
  const auto bytes = secret->reveal();
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// The issuer's endpoints from its discovery document.
lugh::client::Endpoints discover(Io& io, lugh::client::Https& https, const std::string& issuer) {
  std::promise<lugh::Outcome<lugh::client::HttpsResponse>> got;
  auto future = got.get_future();
  lugh::client::HttpsRequest request;
  request.url = issuer + "/.well-known/openid-configuration";
  https.send(request, 30s, io.io.get_executor(), [&got](lugh::Outcome<lugh::client::HttpsResponse> r) { got.set_value(std::move(r)); });
  const auto r = wait(future, 60s);
  if (!r.ok()) throw std::runtime_error("reading the issuer's discovery: " + r.message);
  if (r.value.status != 200) throw std::runtime_error("the issuer's discovery answered " + std::to_string(r.value.status));
  boost::system::error_code failed;
  const auto document = json::parse(r.value.body, failed);
  if (failed || !document.is_object()) throw std::runtime_error("the issuer's discovery is not a JSON object");
  auto endpoints = lugh::client::Endpoints::from_discovery(document.as_object(), issuer);
  if (auto* why = std::get_if<std::string>(&endpoints)) throw std::runtime_error("the issuer's discovery: " + *why);
  return std::get<lugh::client::Endpoints>(endpoints);
}

void keep(Credentials& c, const lugh::client::TokenSet& tokens) {
  c.access_token = secret_text(tokens.access_token);
  if (tokens.refresh_token) c.refresh_token = secret_text(tokens.refresh_token);
  c.expires_unix = now_unix() + tokens.expires_in;
}

int login(const Args& args) {
  const auto issuer = args.flag("issuer");
  const auto client_id = args.flag("client-id");
  const auto endpoint = args.flag("endpoint");
  if (!issuer || !client_id || !endpoint) throw Usage("login needs --issuer, --client-id and --endpoint");
  louhi::api::parse_endpoint(*endpoint);
  Credentials c;
  c.issuer = *issuer;
  c.client_id = *client_id;
  c.endpoint = *endpoint;
  c.scopes = args.all("scope");

  Io io;
  lugh::client::HttpsClient https(io.io, args.flag("ca").value_or(""));
  const auto endpoints = discover(io, https, c.issuer);
  lugh::client::Verifier verifier({c.issuer}, dannan::contract::system_wall_clock());
  lugh::client::ClientIdentity identity;
  identity.client_id = c.client_id;
  lugh::client::OAuthClient oauth(endpoints, identity, https, verifier, dannan::contract::system_wall_clock());

  std::promise<lugh::Outcome<lugh::client::DeviceAuthorization>> begun;
  auto begun_future = begun.get_future();
  oauth.device_authorization(c.scopes, 30s, io.io.get_executor(), [&begun](auto r) { begun.set_value(std::move(r)); });
  const auto started = wait(begun_future, 60s);
  if (!started.ok()) throw std::runtime_error("beginning the sign-in: " + started.message);
  std::cerr << "To sign in, visit " << started.value.verification_uri << " and enter the code " << started.value.user_code << "\n";
  if (!started.value.verification_uri_complete.empty()) std::cerr << "(or visit " << started.value.verification_uri_complete << ")\n";

  std::promise<lugh::Outcome<lugh::client::TokenSet>> tokens;
  auto tokens_future = tokens.get_future();
  oauth.device_tokens(started.value, dannan::contract::steady_time_source(), 30s, io.io.get_executor(), [&tokens](auto r) { tokens.set_value(std::move(r)); });
  const auto got = wait(tokens_future, std::chrono::seconds(started.value.expires_in + 120));
  if (!got.ok()) throw std::runtime_error("signing in: " + got.message);
  keep(c, got.value);
  save(credentials_path(args), c);
  std::cerr << "Signed in; the credentials are in " << credentials_path(args).string() << "\n";
  return 0;
}

// An access token good for a minute more, refreshed and saved when it is not.
std::string access_token(const Args& args, Credentials& c) {
  if (!c.access_token.empty() && c.expires_unix > now_unix() + 60) return c.access_token;
  if (c.refresh_token.empty()) throw std::runtime_error("the sign-in has run out: run louhi-ctl login");
  Io io;
  lugh::client::HttpsClient https(io.io, args.flag("ca").value_or(""));
  const auto endpoints = discover(io, https, c.issuer);
  lugh::client::Verifier verifier({c.issuer}, dannan::contract::system_wall_clock());
  lugh::client::ClientIdentity identity;
  identity.client_id = c.client_id;
  lugh::client::OAuthClient oauth(endpoints, identity, https, verifier, dannan::contract::system_wall_clock());
  const dannan::identity::Secret refresh(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(c.refresh_token.data()), c.refresh_token.size()));
  std::promise<lugh::Outcome<lugh::client::TokenSet>> got;
  auto future = got.get_future();
  oauth.refresh(refresh, {}, 30s, io.io.get_executor(), [&got](auto r) { got.set_value(std::move(r)); });
  const auto r = wait(future, 60s);
  if (!r.ok()) throw std::runtime_error("refreshing the sign-in: " + r.message + ": run louhi-ctl login");
  keep(c, r.value);
  save(credentials_path(args), c);
  return c.access_token;
}

std::string segment(const std::string& text) { return dannan::encoding::percent_encode(text); }

// The request a command makes: its method, target and body.
struct Call {
  std::string method;
  std::string target;
  std::string body;
};

Call command(const Args& args) {
  const auto& p = args.positional;
  const auto need = [&](size_t n, const char* usage) {
    if (p.size() != n + 1) throw Usage(std::string("usage: louhi-ctl ") + usage);
  };
  const std::string& verb = p.at(0);
  const auto project = [&] { return "/v1/projects/" + segment(p.at(1)); };
  if (verb == "list-projects") return need(0, "list-projects"), Call{"GET", "/v1/projects", ""};
  if (verb == "list-nodes") return need(0, "list-nodes"), Call{"GET", "/v1/nodes", ""};
  if (verb == "put-project") {
    need(1, "put-project PROJECT [--description TEXT]");
    return {"PUT", project(), json::serialize(json::object{{"description", args.flag("description").value_or("")}})};
  }
  if (verb == "describe-project") return need(1, "describe-project PROJECT"), Call{"GET", project(), ""};
  if (verb == "put-quota") return need(2, "put-quota PROJECT FILE"), Call{"PUT", project() + "/quota", read_file(p.at(2))};
  if (verb == "describe-quota") return need(1, "describe-quota PROJECT"), Call{"GET", project() + "/quota", ""};
  if (verb == "register-task-definition")
    return need(2, "register-task-definition PROJECT FILE"), Call{"POST", project() + "/task-definitions", read_file(p.at(2))};
  if (verb == "list-task-definition-families") return need(1, "list-task-definition-families PROJECT"), Call{"GET", project() + "/task-definitions", ""};
  if (verb == "list-task-definitions")
    return need(2, "list-task-definitions PROJECT FAMILY"), Call{"GET", project() + "/task-definitions/" + segment(p.at(2)), ""};
  if (verb == "describe-task-definition") {
    need(3, "describe-task-definition PROJECT FAMILY REVISION");
    return {"GET", project() + "/task-definitions/" + segment(p.at(2)) + "/" + segment(p.at(3)), ""};
  }
  if (verb == "run-task") {
    need(2, "run-task PROJECT FAMILY [--revision N] [--count N] [--client-token T]");
    json::object body{{"family", p.at(2)}};
    if (const auto revision = args.flag("revision")) body["revision"] = std::stoull(*revision);
    if (const auto count = args.flag("count")) body["count"] = std::stoull(*count);
    if (const auto token = args.flag("client-token")) body["client_token"] = *token;
    return {"POST", project() + "/tasks", json::serialize(body)};
  }
  if (verb == "list-tasks") {
    need(1, "list-tasks PROJECT [--limit N] [--next ID]");
    std::string query;
    if (const auto limit = args.flag("limit")) query += "limit=" + segment(*limit);
    if (const auto next = args.flag("next")) query += std::string(query.empty() ? "" : "&") + "next=" + segment(*next);
    return {"GET", project() + "/tasks" + (query.empty() ? "" : "?" + query), ""};
  }
  if (verb == "describe-task") return need(2, "describe-task PROJECT TASK"), Call{"GET", project() + "/tasks/" + segment(p.at(2)), ""};
  if (verb == "stop-task") {
    need(2, "stop-task PROJECT TASK [--reason TEXT]");
    json::object body;
    if (const auto reason = args.flag("reason")) body["reason"] = *reason;
    return {"POST", project() + "/tasks/" + segment(p.at(2)) + "/stop", json::serialize(body)};
  }
  throw Usage("no command " + verb);
}

int call_api(const Args& args) {
  const Call call = command(args);
  auto c = load(credentials_path(args));
  std::string endpoint_text = args.flag("endpoint").value_or("");
  if (endpoint_text.empty())
    if (const char* env = std::getenv("LOUHI_ENDPOINT")) endpoint_text = env;
  if (endpoint_text.empty()) endpoint_text = c.endpoint;
  const auto endpoint = louhi::api::parse_endpoint(endpoint_text);
  const std::string token = access_token(args, c);

  Io io;
  std::shared_ptr<const dannan::listen::ClientTrust> trust;
  if (endpoint.tls) trust = std::make_shared<dannan::listen::ClientTrust>(args.flag("ca").value_or(""));
  louhi::api::Client client(io.io, endpoint, trust);
  std::promise<louhi::runtime::Result<louhi::api::Response>> got;
  auto future = got.get_future();
  client.request(call.method, call.target, call.body, token, louhi::runtime::Deadline(30000), io.io.get_executor(),
                 [&got](louhi::runtime::Result<louhi::api::Response> r) { got.set_value(std::move(r)); });
  const auto r = wait(future, 60s);
  if (!r.ok()) throw std::runtime_error(r.error().message);
  std::cout << json::serialize(r.value().body) << "\n";
  return r.value().status >= 200 && r.value().status < 300 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Args args = parse(argc, argv);
    if (args.flags.count("version")) {
      std::cout << louhi::kVersion << "\n";
      return 0;
    }
    if (args.positional.empty()) throw Usage("usage: louhi-ctl COMMAND ...; see the comment atop tools/louhi_ctl.cpp");
    if (args.positional[0] == "login") return login(args);
    if (args.positional[0] == "logout") {
      std::filesystem::remove(credentials_path(args));
      return 0;
    }
    return call_api(args);
  } catch (const Usage& error) {
    std::cerr << "louhi-ctl: " << error.what() << "\n";
    return 2;
  } catch (const std::exception& error) {
    std::cerr << "louhi-ctl: " << error.what() << "\n";
    return 1;
  }
}
