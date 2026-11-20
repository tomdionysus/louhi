//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/ctl/ctl.h"

#include <yaml-cpp/yaml.h>

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
using Kind = dannan::config::Setting::Kind;

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

// The reader takes every argument that starts with '-' for a flag, so "-",
// the operand for stdin, is handed to it as text no argument can hold, and
// put back after.
const std::string stdin_stand_in("\0-", 2);

// Every setting: its key in the file, its flag and its variable.
dannan::config::Reader reader() {
  dannan::config::Reader out;
  out.config_variable = "LOUHI_CONFIG";
  out.settings = {
      {"issuer", Kind::value, "--issuer", "", "the issuer URL of the Lugh to sign in to"},
      {"client_id", Kind::value, "--client-id", "", "louhi-ctl's client id at the issuer"},
      {"endpoints", Kind::list, "--endpoint", "LOUHI_ENDPOINT", "a node's API, an http or https URL"},
      {"scopes", Kind::list, "--scope", "", "a scope to sign in with"},
      {"ca", Kind::value, "--ca", "", "the roots an https endpoint and the issuer chain to, PEM; the system's when absent"},
      {"credentials", Kind::value, "--credentials", "LOUHI_CREDENTIALS", "the credentials file"},
      {"command.version", Kind::toggle, "--version", "", "print the version and stop"},
      {"command.description", Kind::value, "--description", "", "a project's description"},
      {"command.revision", Kind::value, "--revision", "", "a task definition's revision"},
      {"command.count", Kind::value, "--count", "", "a number of tasks"},
      {"command.client_token", Kind::value, "--client-token", "", "a token that makes run-task safe to repeat"},
      {"command.limit", Kind::value, "--limit", "", "a number of tasks"},
      {"command.next", Kind::value, "--next", "", "where a list goes on from"},
      {"command.reason", Kind::value, "--reason", "", "why a task is stopped"},
  };
  return out;
}

// The environment for the reader: a variable set but empty is unset.
class Variables final : public dannan::config::Environment {
 public:
  explicit Variables(const Getenv& getenv) : _getenv(getenv) {}
  std::optional<std::string> get(const std::string& name) const override {
    auto value = _getenv(name);
    if (value && value->empty()) return std::nullopt;
    return value;
  }

 private:
  const Getenv& _getenv;
};

// $HOME/.config/louhi, where louhi-ctl keeps its files by default.
std::optional<std::filesystem::path> home_directory(const Getenv& getenv) {
  const auto home = getenv("HOME");
  if (!home || home->empty()) return std::nullopt;
  return std::filesystem::path(*home) / ".config" / "louhi";
}

// The configuration file in force: the one read, else the default.
std::filesystem::path config_path(const Args& args, const Getenv& getenv) {
  if (!args.settings.path.empty()) return args.settings.path;
  const auto directory = home_directory(getenv);
  if (!directory) throw std::runtime_error("no HOME, so give --config");
  return *directory / "config.yaml";
}

bool default_credentials(const Args& args) { return args.settings.find("credentials") == nullptr; }

// Where an older louhi-ctl kept the credentials and the settings together.
std::filesystem::path old_credentials_path(const std::filesystem::path& credentials) { return credentials.parent_path() / "credentials.json"; }

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

// An endpoint, refused in the name of the layer that gave it.
api::Endpoint checked_endpoint(const std::string& source, const std::string& text) {
  try {
    return api::parse_endpoint(text);
  } catch (const std::invalid_argument& why) {
    throw Usage(source + ": " + why.what());
  }
}

std::string source_of(const Args& args, const std::string& key) {
  const auto* value = args.settings.find(key);
  return value ? value->source : key;
}

std::string emitted(const YAML::Emitter& out) {
  if (!out.good()) throw std::runtime_error("writing YAML: " + out.GetLastError());
  return std::string(out.c_str()) + "\n";
}

// The configuration login leaves: what it signed in with.
struct Saved {
  std::string issuer;
  std::string client_id;
  std::vector<std::string> endpoints;
  std::vector<std::string> scopes;
  std::optional<std::string> ca;
  std::optional<std::string> credentials;
};

std::string config_text(const Saved& saved) {
  YAML::Emitter out;
  out << YAML::BeginMap;
  out << YAML::Key << "issuer" << YAML::Value << saved.issuer;
  out << YAML::Key << "client_id" << YAML::Value << saved.client_id;
  out << YAML::Key << "endpoints" << YAML::Value << saved.endpoints;
  out << YAML::Key << "scopes" << YAML::Value << saved.scopes;
  if (saved.ca) out << YAML::Key << "ca" << YAML::Value << *saved.ca;
  if (saved.credentials) out << YAML::Key << "credentials" << YAML::Value << *saved.credentials;
  out << YAML::EndMap;
  return emitted(out);
}

// An older louhi-ctl's credentials.json: the tokens with the settings.
struct OldCredentials {
  Credentials credentials;
  std::vector<std::string> endpoints;
  std::vector<std::string> scopes;
};

OldCredentials decode_json(std::string_view text) {
  boost::system::error_code failed;
  const auto value = json::parse(text, failed);
  if (failed || !value.is_object()) throw std::runtime_error("not a JSON object");
  const auto& o = value.as_object();
  const auto string_of = [&](const char* key) {
    const auto* v = o.if_contains(key);
    return v && v->is_string() ? std::string(v->as_string()) : std::string();
  };
  OldCredentials out;
  Credentials& c = out.credentials;
  c.issuer = string_of("issuer");
  c.client_id = string_of("client_id");
  if (const auto* v = o.if_contains("endpoints"); v && v->is_array()) {
    for (const auto& e : v->as_array())
      if (e.is_string()) out.endpoints.emplace_back(e.as_string());
  } else if (const auto old = string_of("endpoint"); !old.empty()) {
    out.endpoints.push_back(old);
  }
  c.access_token = string_of("access_token");
  c.refresh_token = string_of("refresh_token");
  if (const auto* v = o.if_contains("expires_unix")) {
    if (v->is_uint64()) c.expires_unix = v->as_uint64();
    if (v->is_int64() && v->as_int64() > 0) c.expires_unix = static_cast<uint64_t>(v->as_int64());
  }
  if (const auto* v = o.if_contains("scopes"); v && v->is_array())
    for (const auto& s : v->as_array())
      if (s.is_string()) out.scopes.emplace_back(s.as_string());
  return out;
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
  Saved saved;
  const auto issuer = args.flag("issuer");
  const auto client_id = args.flag("client_id");
  saved.endpoints = args.all("endpoints");
  if (!issuer || !client_id || saved.endpoints.empty()) throw Usage("login needs --issuer, --client-id and --endpoint");
  saved.issuer = *issuer;
  saved.client_id = *client_id;
  for (const auto& api : saved.endpoints) checked_endpoint(source_of(args, "endpoints"), api);
  saved.scopes = args.all("scopes");
  // The issuer answers openid with an ID token, which louhi-ctl has no use
  // for and holds no keys to verify: refused before anything is sent.
  for (const auto& scope : saved.scopes) {
    std::istringstream words(scope);
    for (std::string word; words >> word;) {
      if (word == "openid") throw Usage("louhi-ctl needs no ID token: leave out --scope openid");
    }
  }
  saved.ca = args.flag("ca");
  // The file's own credentials path stays in it; one given otherwise is for
  // this run only, as it always was.
  if (const auto* credentials = args.settings.find("credentials"); credentials && credentials->source == args.settings.path)
    saved.credentials = args.flag("credentials");
  const auto path = credentials_path(args, env.getenv);
  const auto config = config_path(args, env.getenv);

  const auto https = env.issuer(saved.ca.value_or(""));
  const auto endpoints = discover(env, *https, saved.issuer);
  const lugh::client::Verifier verifier({saved.issuer}, *env.wall);
  lugh::client::OAuthClient oauth(endpoints, public_client(saved.client_id), *https, verifier, *env.wall);

  const auto started = ask<lugh::Outcome<lugh::client::DeviceAuthorization>>(
      [&](auto done) { oauth.device_authorization(saved.scopes, step, env.executor, std::move(done)); }, patience);
  if (!started.ok()) throw std::runtime_error("beginning the sign-in: " + started.message);
  *env.err << "To sign in, visit " << started.value.verification_uri << " and enter the code " << started.value.user_code << "\n";
  if (!started.value.verification_uri_complete.empty()) *env.err << "(or visit " << started.value.verification_uri_complete << ")\n";

  const auto got =
      ask<lugh::Outcome<lugh::client::TokenSet>>([&](auto done) { oauth.device_tokens(started.value, *env.steady, step, env.executor, std::move(done)); },
                                                 std::chrono::seconds(started.value.expires_in) + patience);
  if (!got.ok()) throw std::runtime_error("signing in: " + got.message);
  Credentials c;
  c.issuer = saved.issuer;
  c.client_id = saved.client_id;
  keep(c, got.value, *env.wall);
  env.file(config)->write(config_text(saved));
  env.file(path)->write(encode(c));
  if (default_credentials(args)) env.file(old_credentials_path(path))->remove();
  *env.err << "Signed in; the settings are in " << config.string() << " and the credentials in " << path.string() << "\n";
  return 0;
}

// Credentials an older louhi-ctl kept in credentials.json, beside the default
// credentials.yaml, are moved to the YAML files once: the tokens to
// credentials.yaml, the settings to the configuration unless there is one
// already, and the old file removed. Returns the credentials' text, and the
// endpoints moved for this call to use.
std::optional<std::string> move_old_credentials(const Args& args, const Environment& env, const std::filesystem::path& path, CredentialFile& file,
                                                std::vector<std::string>& moved_endpoints) {
  const auto old_path = old_credentials_path(path);
  const auto old_file = env.file(old_path);
  const auto old_text = old_file->read();
  if (!old_text) return std::nullopt;
  OldCredentials old;
  try {
    old = decode_json(*old_text);
  } catch (const std::exception& why) {
    throw std::runtime_error("the credentials at " + old_path.string() + ": " + why.what() + ": run louhi-ctl login");
  }
  auto text = encode(old.credentials);
  file.write(text);
  if (args.settings.path.empty()) {
    const auto config_file = env.file(config_path(args, env.getenv));
    if (!config_file->read()) {
      config_file->write(config_text({old.credentials.issuer, old.credentials.client_id, old.endpoints, old.scopes, std::nullopt, std::nullopt}));
      moved_endpoints = old.endpoints;
    }
  }
  old_file->remove();
  *env.err << "louhi-ctl: moved " << old_path.string() << " to " << path.string() << "\n";
  return text;
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
  // The endpoints, every one checked before any is called.
  std::vector<std::pair<std::string, api::Endpoint>> endpoints;
  for (const auto& url : args.all("endpoints")) endpoints.emplace_back(url, checked_endpoint(source_of(args, "endpoints"), url));

  const auto path = credentials_path(args, env.getenv);
  const auto file = env.file(path);
  auto text = file->read();
  std::vector<std::string> moved_endpoints;
  if (!text && default_credentials(args)) text = move_old_credentials(args, env, path, *file, moved_endpoints);
  if (!text) throw std::runtime_error("no credentials at " + path.string() + ": run louhi-ctl login");
  Credentials c;
  try {
    c = decode(*text);
  } catch (const std::exception& why) {
    throw std::runtime_error("the credentials at " + path.string() + ": " + why.what() + ": run louhi-ctl login");
  }
  if (endpoints.empty())
    for (const auto& url : moved_endpoints) endpoints.emplace_back(url, checked_endpoint("the moved credentials", url));
  if (endpoints.empty()) throw std::runtime_error("no endpoint: give --endpoint");
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

std::optional<std::string> Args::flag(const std::string& key) const { return settings.text(key); }

std::vector<std::string> Args::all(const std::string& key) const { return settings.list(key); }

Args read_args(const std::vector<std::string>& arguments, const Getenv& getenv) {
  const Variables variables(getenv);
  auto r = reader();
  r.environment = &variables;
  if (const auto directory = home_directory(getenv)) r.default_path = (*directory / "config.yaml").string();
  std::vector<std::string> given = arguments;
  for (auto& argument : given)
    if (argument == "-") argument = stdin_stand_in;
  // The reader keeps the last of a single-valued flag given twice; louhi-ctl
  // refuses it rather than guess which was meant.
  for (const auto& setting : r.settings) {
    if (setting.kind != Kind::value) continue;
    size_t seen = 0;
    for (const auto& argument : given)
      if (argument == setting.flag || argument.rfind(setting.flag + "=", 0) == 0) ++seen;
    if (seen > 1) throw Usage(setting.flag + " is given more than once");
  }
  Args out;
  try {
    out.settings = r.read(given);
  } catch (const dannan::config::ConfigError& why) {
    throw Usage(why.what());
  }
  const auto restore = [](std::string& text) {
    if (text == stdin_stand_in) text = "-";
  };
  for (auto& argument : out.settings.positional) restore(argument);
  for (auto& [key, value] : out.settings.values)
    for (auto& item : value.items) restore(item);
  out.positional = out.settings.positional;
  return out;
}

std::string encode(const Credentials& c) {
  YAML::Emitter out;
  out << YAML::BeginMap;
  out << YAML::Key << "issuer" << YAML::Value << c.issuer;
  out << YAML::Key << "client_id" << YAML::Value << c.client_id;
  out << YAML::Key << "access_token" << YAML::Value << c.access_token;
  out << YAML::Key << "refresh_token" << YAML::Value << c.refresh_token;
  out << YAML::Key << "expires_unix" << YAML::Value << c.expires_unix;
  out << YAML::EndMap;
  return emitted(out);
}

Credentials decode(std::string_view text) {
  YAML::Node root;
  try {
    root = YAML::Load(std::string(text));
  } catch (const YAML::Exception& why) {
    throw std::runtime_error(std::string("not YAML: ") + why.what());
  }
  if (!root.IsMap()) throw std::runtime_error("not a YAML map");
  const YAML::Node& map = root;
  const auto string_of = [&](const char* key) {
    const YAML::Node node = map[key];
    return node && node.IsScalar() ? node.Scalar() : std::string();
  };
  Credentials c;
  c.issuer = string_of("issuer");
  c.client_id = string_of("client_id");
  c.access_token = string_of("access_token");
  c.refresh_token = string_of("refresh_token");
  // Anything but a whole number in range is no expiry: the token is
  // refreshed.
  if (const auto expiry = string_of("expires_unix"); !expiry.empty() && expiry.size() <= 19 && expiry.find_first_not_of("0123456789") == std::string::npos)
    c.expires_unix = std::stoull(expiry);
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

void DiskCredentialFile::write(std::string_view text) {
  dannan::identity::durable_replace_file(_path, text);
  // Bearer credentials: owner-only whatever mode the replacement was made
  // with.
  std::filesystem::permissions(_path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace);
}

void DiskCredentialFile::remove() { std::filesystem::remove(_path); }

std::filesystem::path credentials_path(const Args& args, const Getenv& getenv) {
  if (const auto given = args.settings.path_of("credentials")) return *given;
  const auto directory = home_directory(getenv);
  if (!directory) throw std::runtime_error("no HOME, so give --credentials");
  return *directory / "credentials.yaml";
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
    return {"PUT", project(), json::serialize(json::object{{"description", args.flag("command.description").value_or("")}})};
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
    if (const auto revision = args.flag("command.revision")) body["revision"] = number("revision", *revision);
    if (const auto count = args.flag("command.count")) body["count"] = number("count", *count);
    if (const auto token = args.flag("command.client_token")) body["client_token"] = *token;
    return {"POST", project() + "/tasks", json::serialize(body)};
  }
  if (verb == "list-tasks") {
    need(1, "list-tasks PROJECT [--limit N] [--next ID]");
    std::string query;
    if (const auto limit = args.flag("command.limit")) query += "limit=" + std::to_string(number("limit", *limit));
    if (const auto next = args.flag("command.next")) query += std::string(query.empty() ? "" : "&") + "next=" + segment(*next);
    return {"GET", project() + "/tasks" + (query.empty() ? "" : "?" + query), ""};
  }
  if (verb == "describe-task") return need(2, "describe-task PROJECT TASK"), Call{"GET", project() + "/tasks/" + segment(p.at(2)), ""};
  if (verb == "stop-task") {
    need(2, "stop-task PROJECT TASK [--reason TEXT]");
    json::object body;
    if (const auto reason = args.flag("command.reason")) body["reason"] = *reason;
    return {"POST", project() + "/tasks/" + segment(p.at(2)) + "/stop", json::serialize(body)};
  }
  throw Usage("no command " + verb);
}

int run(const std::vector<std::string>& arguments, const Environment& env) {
  try {
    const Args args = read_args(arguments, env.getenv);
    for (const auto& warning : args.settings.warnings) *env.err << "louhi-ctl: " << warning << "\n";
    if (args.settings.toggle("command.version").value_or(false)) {
      *env.out << kVersion << "\n";
      return 0;
    }
    if (args.positional.empty()) throw Usage(usage_text);
    if (args.positional[0] == "login") return login(args, env);
    if (args.positional[0] == "logout") {
      if (args.positional.size() != 1) throw Usage("usage: louhi-ctl logout");
      const auto path = credentials_path(args, env.getenv);
      env.file(path)->remove();
      if (default_credentials(args)) env.file(old_credentials_path(path))->remove();
      return 0;
    }
    return call_api(args, env);
  } catch (const Usage& error) {
    *env.err << "louhi-ctl: " << error.what() << "\n";
    return 2;
  } catch (const dannan::config::ConfigError& error) {
    *env.err << "louhi-ctl: " << error.what() << "\n";
    return 2;
  } catch (const std::exception& error) {
    *env.err << "louhi-ctl: " << error.what() << "\n";
    return 1;
  }
}

}  // namespace louhi::ctl
