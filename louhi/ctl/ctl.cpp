//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/ctl/ctl.h"

#include <boost/json.hpp>
#include <fstream>
#include <future>
#include <iterator>
#include <span>
#include <sstream>

#include "dannan/encoding/percent.h"
#include "dannan/identity/durable_file.h"
#include "dannan/identity/secure_memory.h"
#include "louhi/version.h"
#include "lugh/client/oauth_client.h"
#include "lugh/client/verifier.h"

namespace louhi::ctl {

namespace {

namespace json = boost::json;
using namespace std::chrono_literals;

// Each call to the issuer or the API may take this long, and is waited for a
// minute more: the deadline answers it first.
constexpr std::chrono::milliseconds step = 30s;
constexpr std::chrono::seconds patience = 90s;
// A token with less left than this is refreshed first.
constexpr uint64_t least_life_seconds = 60;
// The credentials file is a few hundred bytes; it is read with a bound all
// the same.
constexpr std::uintmax_t most_credentials_bytes = 64 * 1024;

constexpr const char* usage_text =
    "usage: louhi-ctl COMMAND ...: login, logout, list-projects, put-project, describe-project, put-quota, describe-quota, register-task-definition, "
    "list-task-definition-families, list-task-definitions, describe-task-definition, run-task, list-tasks, describe-task, stop-task, list-nodes";

// The answer to what `start` begins, waited for at most `most`. The promise
// is shared, so an answer after the wait has given up lands nowhere.
template <typename T, typename Start>
T ask(Start start, std::chrono::seconds most) {
  auto got = std::make_shared<std::promise<T>>();
  auto future = got->get_future();
  start(std::function<void(T)>([got](T answer) { got->set_value(std::move(answer)); }));
  if (future.wait_for(most) != std::future_status::ready) throw std::runtime_error("no answer in time");
  return future.get();
}

std::string text_of(const std::shared_ptr<const dannan::identity::Secret>& secret) {
  if (!secret) return {};
  const auto bytes = secret->reveal();
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::string segment(const std::string& text) { return dannan::encoding::percent_encode(text); }

// A count or a revision: decimal digits only, no sign.
uint64_t number(const std::string& flag, const std::string& text) {
  if (text.empty() || text.size() > 19 || text.find_first_not_of("0123456789") != std::string::npos)
    throw Usage("--" + flag + " needs a whole number, not \"" + text + "\"");
  return std::stoull(text);
}

api::Endpoint endpoint_from_flag(const std::string& text) {
  try {
    return api::parse_endpoint(text);
  } catch (const std::invalid_argument& why) {
    throw Usage(std::string("--endpoint: ") + why.what());
  }
}

// The issuer's endpoints from its discovery document.
lugh::client::Endpoints discover(const Environment& env, lugh::client::Https& https, const std::string& issuer) {
  lugh::client::HttpsRequest request;
  request.url = issuer + "/.well-known/openid-configuration";
  const auto r = ask<lugh::Outcome<lugh::client::HttpsResponse>>([&](auto done) { https.send(request, step, env.executor, std::move(done)); }, patience);
  if (!r.ok()) throw std::runtime_error("reading the issuer's discovery: " + r.message);
  if (r.value.status != 200) throw std::runtime_error("the issuer's discovery answered " + std::to_string(r.value.status));
  boost::system::error_code failed;
  const auto document = json::parse(r.value.body, failed);
  if (failed || !document.is_object()) throw std::runtime_error("the issuer's discovery is not a JSON object");
  auto endpoints = lugh::client::Endpoints::from_discovery(document.as_object(), issuer);
  if (auto* why = std::get_if<std::string>(&endpoints)) throw std::runtime_error("the issuer's discovery: " + *why);
  return std::get<lugh::client::Endpoints>(endpoints);
}

// $LOUHI_ENDPOINT's list: separated by commas or spaces, empty items dropped.
std::vector<std::string> endpoint_list(const std::string& text) {
  std::vector<std::string> out;
  std::string item;
  for (const char ch : text + ",") {
    if (ch == ',' || ch == ' ' || ch == '\t') {
      if (!item.empty()) out.push_back(item);
      item.clear();
    } else {
      item.push_back(ch);
    }
  }
  return out;
}

// louhi-ctl is a public client: it proves nothing but its client_id.
lugh::client::ClientIdentity public_client(const std::string& client_id) {
  lugh::client::ClientIdentity identity;
  identity.client_id = client_id;
  return identity;
}

void keep(Credentials& c, const lugh::client::TokenSet& tokens, const dannan::contract::WallClock& wall) {
  c.access_token = text_of(tokens.access_token);
  // Lugh rotates refresh tokens: the new one replaces the one just spent.
  if (tokens.refresh_token) c.refresh_token = text_of(tokens.refresh_token);
  c.expires_unix = wall.unix_ms() / 1000 + tokens.expires_in;
}

int login(const Args& args, const Environment& env) {
  if (args.positional.size() != 1) throw Usage("usage: louhi-ctl login --issuer URL --client-id ID --endpoint URL... [--scope S]... [--ca FILE]");
  const auto issuer = args.flag("issuer");
  const auto client_id = args.flag("client-id");
  const auto apis = args.all("endpoint");
  if (!issuer || !client_id || apis.empty()) throw Usage("login needs --issuer, --client-id and --endpoint");
  for (const auto& api : apis) endpoint_from_flag(api);
  Credentials c;
  c.issuer = *issuer;
  c.client_id = *client_id;
  c.endpoints = apis;
  c.scopes = args.all("scope");
  // The issuer answers openid with an ID token, which louhi-ctl has no use
  // for and holds no keys to verify: refused before anything is sent.
  for (const auto& scope : c.scopes) {
    std::istringstream words(scope);
    for (std::string word; words >> word;) {
      if (word == "openid") throw Usage("louhi-ctl needs no ID token: leave out --scope openid");
    }
  }
  const auto path = credentials_path(args, env.getenv);

  const auto https = env.issuer(args.flag("ca").value_or(""));
  const auto endpoints = discover(env, *https, c.issuer);
  const lugh::client::Verifier verifier({c.issuer}, *env.wall);
  lugh::client::OAuthClient oauth(endpoints, public_client(c.client_id), *https, verifier, *env.wall);

  const auto started = ask<lugh::Outcome<lugh::client::DeviceAuthorization>>(
      [&](auto done) { oauth.device_authorization(c.scopes, step, env.executor, std::move(done)); }, patience);
  if (!started.ok()) throw std::runtime_error("beginning the sign-in: " + started.message);
  *env.err << "To sign in, visit " << started.value.verification_uri << " and enter the code " << started.value.user_code << "\n";
  if (!started.value.verification_uri_complete.empty()) *env.err << "(or visit " << started.value.verification_uri_complete << ")\n";

  const auto got =
      ask<lugh::Outcome<lugh::client::TokenSet>>([&](auto done) { oauth.device_tokens(started.value, *env.steady, step, env.executor, std::move(done)); },
                                                 std::chrono::seconds(started.value.expires_in) + patience);
  if (!got.ok()) throw std::runtime_error("signing in: " + got.message);
  keep(c, got.value, *env.wall);
  env.credentials(path)->write(encode(c));
  *env.err << "Signed in; the credentials are in " << path.string() << "\n";
  return 0;
}

// An access token good for a minute more, refreshed and saved when it is not.
std::string access_token(const Args& args, const Environment& env, CredentialFile& file, Credentials& c) {
  if (!c.access_token.empty() && c.expires_unix > env.wall->unix_ms() / 1000 + least_life_seconds) return c.access_token;
  if (c.refresh_token.empty()) throw std::runtime_error("the sign-in has run out: run louhi-ctl login");
  const auto https = env.issuer(args.flag("ca").value_or(""));
  const auto endpoints = discover(env, *https, c.issuer);
  const lugh::client::Verifier verifier({c.issuer}, *env.wall);
  lugh::client::OAuthClient oauth(endpoints, public_client(c.client_id), *https, verifier, *env.wall);
  const dannan::identity::Secret refresh(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(c.refresh_token.data()), c.refresh_token.size()));
  const auto r = ask<lugh::Outcome<lugh::client::TokenSet>>([&](auto done) { oauth.refresh(refresh, {}, step, env.executor, std::move(done)); }, patience);
  if (!r.ok()) throw std::runtime_error("refreshing the sign-in: " + r.message + ": run louhi-ctl login");
  keep(c, r.value, *env.wall);
  file.write(encode(c));
  return c.access_token;
}

int call_api(const Args& args, const Environment& env) {
  const Call call = command(args, env.read);
  const auto path = credentials_path(args, env.getenv);
  const auto file = env.credentials(path);
  const auto text = file->read();
  if (!text) throw std::runtime_error("no credentials at " + path.string() + ": run louhi-ctl login");
  Credentials c;
  try {
    c = decode(*text);
  } catch (const std::exception& why) {
    throw std::runtime_error("the credentials at " + path.string() + ": " + why.what() + ": run louhi-ctl login");
  }
  // The endpoints, every one checked before any is called.
  std::vector<std::pair<std::string, api::Endpoint>> endpoints;
  if (const auto given = args.all("endpoint"); !given.empty()) {
    for (const auto& url : given) endpoints.emplace_back(url, endpoint_from_flag(url));
  } else {
    auto named = endpoint_list(env.getenv("LOUHI_ENDPOINT").value_or(""));
    if (named.empty()) named = c.endpoints;
    if (named.empty()) throw std::runtime_error("no endpoint: give --endpoint");
    for (const auto& url : named) endpoints.emplace_back(url, api::parse_endpoint(url));
  }
  const std::string token = access_token(args, env, *file, c);

  // A request that never left this machine can go anywhere; a read can be
  // asked again; a write that may have arrived must not be.
  const bool read = call.method == "GET";
  std::string failures;
  for (const auto& [url, endpoint] : endpoints) {
    const auto requester = env.api(endpoint, args.flag("ca").value_or(""));
    const auto r = ask<runtime::Result<api::Response>>(
        [&](auto done) { requester->request(call.method, call.target, call.body, token, step, env.executor, std::move(done)); }, patience);
    if (r.ok()) {
      *env.out << json::serialize(r.value().body) << "\n";
      return r.value().status >= 200 && r.value().status < 300 ? 0 : 1;
    }
    failures += (failures.empty() ? "" : "; ") + url + ": " + r.error().message;
    if (r.error().kind != runtime::Error::Kind::unavailable && !read)
      throw std::runtime_error(failures + " (the request may have reached it, so it is not sent elsewhere)");
  }
  throw std::runtime_error("no endpoint answered: " + failures);
}

}  // namespace

std::optional<std::string> Args::flag(const std::string& name) const {
  const auto it = flags.find(name);
  if (it == flags.end() || it->second.empty()) return std::nullopt;
  if (it->second.size() > 1) throw Usage("--" + name + " is given more than once");
  return it->second.front();
}

std::vector<std::string> Args::all(const std::string& name) const {
  const auto it = flags.find(name);
  return it == flags.end() ? std::vector<std::string>{} : it->second;
}

Args parse(const std::vector<std::string>& arguments) {
  Args out;
  for (size_t i = 0; i < arguments.size(); ++i) {
    const std::string& arg = arguments[i];
    if (arg == "--version") {
      out.flags["version"].push_back("");
    } else if (arg.rfind("--", 0) == 0) {
      if (i + 1 >= arguments.size()) throw Usage(arg + " needs a value");
      out.flags[arg.substr(2)].push_back(arguments[++i]);
    } else {
      out.positional.push_back(arg);
    }
  }
  return out;
}

std::string encode(const Credentials& c) {
  json::array scopes;
  for (const auto& s : c.scopes) scopes.emplace_back(s);
  json::array endpoints;
  for (const auto& e : c.endpoints) endpoints.emplace_back(e);
  const json::object o{{"issuer", c.issuer},
                       {"client_id", c.client_id},
                       {"endpoints", endpoints},
                       {"scopes", scopes},
                       {"access_token", c.access_token},
                       {"refresh_token", c.refresh_token},
                       {"expires_unix", c.expires_unix}};
  return json::serialize(o) + "\n";
}

Credentials decode(std::string_view text) {
  boost::system::error_code failed;
  const auto value = json::parse(text, failed);
  if (failed || !value.is_object()) throw std::runtime_error("not a JSON object");
  const auto& o = value.as_object();
  const auto string_of = [&](const char* key) {
    const auto* v = o.if_contains(key);
    return v && v->is_string() ? std::string(v->as_string()) : std::string();
  };
  Credentials c;
  c.issuer = string_of("issuer");
  c.client_id = string_of("client_id");
  if (const auto* v = o.if_contains("endpoints"); v && v->is_array()) {
    for (const auto& e : v->as_array())
      if (e.is_string()) c.endpoints.emplace_back(e.as_string());
  } else if (const auto old = string_of("endpoint"); !old.empty()) {
    c.endpoints.push_back(old);
  }
  c.access_token = string_of("access_token");
  c.refresh_token = string_of("refresh_token");
  // Anything but a whole number in range is no expiry: the token is
  // refreshed.
  if (const auto* v = o.if_contains("expires_unix")) {
    if (v->is_uint64()) c.expires_unix = v->as_uint64();
    if (v->is_int64() && v->as_int64() > 0) c.expires_unix = static_cast<uint64_t>(v->as_int64());
  }
  if (const auto* v = o.if_contains("scopes"); v && v->is_array())
    for (const auto& s : v->as_array())
      if (s.is_string()) c.scopes.emplace_back(s.as_string());
  return c;
}

std::optional<std::string> DiskCredentialFile::read() {
  std::error_code ec;
  if (!std::filesystem::exists(_path, ec)) return std::nullopt;
  const auto size = std::filesystem::file_size(_path, ec);
  if (ec) throw std::runtime_error("cannot read " + _path.string());
  if (size > most_credentials_bytes) throw std::runtime_error(_path.string() + " is too large to be credentials");
  std::ifstream in(_path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read " + _path.string());
  return std::string(std::istreambuf_iterator<char>(in), {});
}

void DiskCredentialFile::write(std::string_view text) { dannan::identity::durable_replace_file(_path, text); }

void DiskCredentialFile::remove() { std::filesystem::remove(_path); }

std::filesystem::path credentials_path(const Args& args, const Getenv& getenv) {
  if (const auto given = args.flag("credentials")) return *given;
  if (const auto variable = getenv("LOUHI_CREDENTIALS")) return *variable;
  const auto home = getenv("HOME");
  if (!home) throw std::runtime_error("no HOME, so give --credentials");
  return std::filesystem::path(*home) / ".config" / "louhi" / "credentials.json";
}

Call command(const Args& args, const std::function<std::string(const std::string&)>& read) {
  const auto& p = args.positional;
  if (p.empty()) throw Usage(usage_text);
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
  if (verb == "put-quota") return need(2, "put-quota PROJECT FILE"), Call{"PUT", project() + "/quota", read(p.at(2))};
  if (verb == "describe-quota") return need(1, "describe-quota PROJECT"), Call{"GET", project() + "/quota", ""};
  if (verb == "register-task-definition") return need(2, "register-task-definition PROJECT FILE"), Call{"POST", project() + "/task-definitions", read(p.at(2))};
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
    if (const auto revision = args.flag("revision")) body["revision"] = number("revision", *revision);
    if (const auto count = args.flag("count")) body["count"] = number("count", *count);
    if (const auto token = args.flag("client-token")) body["client_token"] = *token;
    return {"POST", project() + "/tasks", json::serialize(body)};
  }
  if (verb == "list-tasks") {
    need(1, "list-tasks PROJECT [--limit N] [--next ID]");
    std::string query;
    if (const auto limit = args.flag("limit")) query += "limit=" + std::to_string(number("limit", *limit));
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

int run(const std::vector<std::string>& arguments, const Environment& env) {
  try {
    const Args args = parse(arguments);
    if (args.flags.count("version")) {
      *env.out << kVersion << "\n";
      return 0;
    }
    if (args.positional.empty()) throw Usage(usage_text);
    if (args.positional[0] == "login") return login(args, env);
    if (args.positional[0] == "logout") {
      if (args.positional.size() != 1) throw Usage("usage: louhi-ctl logout");
      env.credentials(credentials_path(args, env.getenv))->remove();
      return 0;
    }
    return call_api(args, env);
  } catch (const Usage& error) {
    *env.err << "louhi-ctl: " << error.what() << "\n";
    return 2;
  } catch (const std::exception& error) {
    *env.err << "louhi-ctl: " << error.what() << "\n";
    return 1;
  }
}

}  // namespace louhi::ctl
