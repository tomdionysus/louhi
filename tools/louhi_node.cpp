//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// louhi-node: one node of Louhi (design, "The node"). A Dannan node that
// advertises "louhi" and runs the tasks placed on it through the container
// runtime, by the agent (agent/agent.h). It reports what it does as one JSON
// object per line on stdout:
//
//   {"event":"listening","id":"...","port":N}
//   {"event":"api","http":N,"https":N}                 when the API is served
//   {"event":"status","ready":true,"tasks":N,"passes":N,"conditions":N}   every --status-ms
//   {"event":"stopped"}
//
// It runs for --seconds (0: until SIGTERM or SIGINT). Stopping it leaves the
// tasks running; a node started again adopts them.
//
// Every setting is also a key in a YAML file (--config PATH, $LOUHI_CONFIG,
// or <sysconfdir>/louhi/config.yaml when it exists: /etc/louhi for the
// package, /usr/local/etc/louhi for a source install), in secrets.yaml beside
// it, or a LOUHI_* variable, the flags winning (reader() below lists them).
//
//   louhi-node --key FILE --state DIR [--port N] [--listen ADDR]
//              [--advertise HOST] [--bootstrap HOST:PORT]... [--name NAME]
//              [--domain NAME] [--heartbeat-ms N] [--status-ms N]
//              [--seconds N] [--log-level LEVEL] [--log-format text|json]
//              [--docker-socket PATH] [--allow-unenforced-memory]
//              [--allow-without-user-namespaces] [--allow-unbounded-storage]
//              [--registry DOMAIN]... [--registry-credential DOMAIN=USER:PASSWORD]...
//              [--api-http HOST:PORT] [--api-https HOST:PORT --api-tls-chain FILE --api-tls-key FILE]
//              [--lugh-issuer URL]... [--lugh-ca FILE]
//              [--lir [--lir-tls [--lir-tls-trust FILE]]
//               [--lir-client-id ID (--lir-client-key DER --lir-client-kid KID | --lir-client-secret S)]]
//   louhi-node --version
//   louhi-node --key FILE --state DIR --retire NODE_ID...
//
// --retire is an operator's word that a node has left the cluster for good
// (the maintainer, 2026-11-17, as lir-node has it): Dannan takes it out of this node's
// agreed roster (Nodes::retire), so the partition reading no longer counts
// it. Dannan never drops a member for being absent, so a node gone for good
// counts against the majority until it is retired. It runs with this node
// stopped, since it opens the node's state, and is refused while the node
// runs (the state lock). Run it on each node in turn. It prints one line a
// node, {"event":"retire","id":"...","retired":true}, false when the node was
// not a member. Retiring a node that is still running does not last: it is a
// member again when it is next seen to join.
//
// With --lir, events are published to Lir (design, "Events"), its nodes found
// in Dannan's roster. With --lir-client-id, the node signs in to Lir with a
// token from the Lugh issuers, by the client-credentials grant, trying each in
// turn (Lugh's TokenSource); without it, with no password.
//
// The API (design, "API and sign-on") is served when at least one Lugh
// issuer and at least one of --api-http and --api-https are given; sign-in
// is by the Lugh plugin only, so without an issuer there is no API. A build
// without the plugin (LOUHI_BUILD_LUGH off) serves none.
//
// Images come only from the registries listed (--registry, by domain;
// docker.io for Docker Hub): none listed allows none. A tag is resolved to a
// digest when a definition is registered (design, "Images in detail").
//
// The store is the in-memory one until the Dagda store exists (ACTIVE.md):
// it holds only what was declared since this node started, so the node
// treats it as never whole and removes nothing it does not recognise. Tasks
// run through the API are placed on the node that took them (step 6 places
// across nodes).

#include <signal.h>

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "dannan/config/config.h"
#include "dannan/node.h"
#include "louhi/agent/agent.h"
#include "louhi/agent/store_bindings.h"
#include "louhi/api/api.h"
#include "louhi/api/auth.h"
#include "louhi/api/http.h"
#include "louhi/events/events.h"
#include "louhi/model/codec.h"
#include "louhi/model/keys.h"
#include "louhi/model/names.h"
#include "louhi/node/status.h"
#include "louhi/runtime/docker/docker_driver.h"
#include "louhi/runtime/host_files.h"
#include "louhi/runtime/registries.h"
#include "louhi/store/memory_store.h"
#include "louhi/version.h"
#ifdef LOUHI_WITH_LUGH
#include "louhi/auth/lugh.h"
#include "lugh/client/relying_party.h"
#endif
#ifdef LOUHI_WITH_LIR
#include "louhi/events/lir.h"
#endif

namespace {

using namespace std::chrono_literals;
namespace asio = boost::asio;

std::atomic<bool> stopping{false};
std::mutex output;

void say(const std::string& line) {
  std::lock_guard lock(output);
  std::cout << line << std::endl;
}

struct Options {
  std::string key;
  std::string state;
  std::string listen = "0.0.0.0";
  std::string advertise;
  uint16_t port = 7437;
  std::vector<dannan::net::Endpoint> bootstrap;
  std::string name;
  std::string domain;
  std::chrono::milliseconds heartbeat{5000};
  std::chrono::milliseconds status{10000};
  int seconds = 0;
  dannan::status::LogLevel log_level = dannan::status::LogLevel::info;
  dannan::status::LogFormat log_format = dannan::status::LogFormat::text;
  bool version = false;
  std::vector<dannan::identity::NodeId> retire;
  std::string docker_socket = "/var/run/docker.sock";
  bool allow_unenforced_memory = false;
  bool allow_without_user_namespaces = false;
  bool allow_unbounded_storage = false;
  std::chrono::hours retain_finished{24};
  std::map<std::string, std::string> host_directories;
  std::string output_directory;
  std::vector<std::string> registries;
  std::map<std::string, louhi::runtime::RegistryCredential> registry_credentials;
  std::optional<dannan::net::Endpoint> api_http;
  std::optional<dannan::net::Endpoint> api_https;
  std::string api_tls_chain;
  std::string api_tls_key;
  std::vector<std::string> lugh_issuers;
  std::string lugh_ca;
  bool lir = false;
  bool lir_tls = false;
  std::string lir_tls_trust;
  std::string lir_client_id;
  std::string lir_client_key;
  std::string lir_client_kid;
  std::string lir_client_secret;
};

dannan::net::Endpoint endpoint(const std::string& text) {
  const auto colon = text.rfind(':');
  if (colon == std::string::npos) throw std::runtime_error("expected HOST:PORT, got " + text);
  return {text.substr(0, colon), static_cast<uint16_t>(std::stoi(text.substr(colon + 1)))};
}

// Every setting, its flag, and its variable (LOUHI_ and the key in
// capitals), read through the stack's one reader (dannan/config/config.h).
dannan::config::Reader reader() {
  using Kind = dannan::config::Setting::Kind;
  dannan::config::Reader out;
  out.config_variable = "LOUHI_CONFIG";
  out.default_path = LOUHI_DEFAULT_CONFIG;
  const auto add = [&](std::string key, Kind kind, std::string flag, std::string what) {
    std::string variable = "LOUHI_";
    for (const char c : key) variable += c == '.' ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    out.settings.push_back({std::move(key), kind, std::move(flag), std::move(variable), std::move(what)});
  };
  add("command.version", Kind::toggle, "--version", "print the version and stop");
  add("command.retire", Kind::list, "--retire", "a node id to take out of the agreed roster, with this node stopped; then stop");
  add("cluster.key", Kind::value, "--key", "the cluster key file");
  add("cluster.bootstrap", Kind::list, "--bootstrap", "HOST:PORT");
  add("node.state", Kind::value, "--state", "the state directory");
  add("node.name", Kind::value, "--name", "a name");
  add("node.domain", Kind::value, "--domain", "a failure domain");
  add("node.listen", Kind::value, "--listen", "an address");
  add("node.port", Kind::value, "--port", "a port, 0 to 65535");
  add("node.advertise", Kind::value, "--advertise", "a host");
  add("timing.heartbeat_ms", Kind::value, "--heartbeat-ms", "milliseconds");
  add("timing.status_ms", Kind::value, "--status-ms", "milliseconds");
  add("timing.seconds", Kind::value, "--seconds", "seconds to run, 0 until stopped");
  add("log.level", Kind::value, "--log-level", "a log level");
  add("log.format", Kind::value, "--log-format", "text or json");
  add("runtime.docker_socket", Kind::value, "--docker-socket", "the Docker Engine's local socket");
  add("runtime.allow_unenforced_memory", Kind::toggle, "--allow-unenforced-memory", "launch tasks even where the runtime cannot enforce memory limits");
  add("runtime.allow_without_user_namespaces", Kind::toggle, "--allow-without-user-namespaces",
      "launch tasks even where the runtime has no user namespaces (root in a container is root on the node)");
  add("runtime.allow_unbounded_storage", Kind::toggle, "--allow-unbounded-storage",
      "launch tasks even where the runtime cannot bound their storage (Docker needs overlay2 on xfs with project quotas)");
  add("runtime.retain_finished_hours", Kind::value, "--retain-finished-hours",
      "hours a finished task's containers and volumes are kept, for its logs, after it stopped; 24 when absent");
  add("runtime.host_directories", Kind::list, "--host-directory",
      "NAME=PATH: a host directory tasks may mount read-only, by NAME, at a path inside it; a list");
  add("runtime.output_directory", Kind::value, "--output-directory",
      "where each task's output volumes are made, one directory each, bounded by xfs project quotas");
  add("registries.allow", Kind::list, "--registry", "a registry images may come from, by domain (docker.io for Docker Hub); none allows none");
  add("registries.credentials", Kind::list, "--registry-credential",
      "DOMAIN=USER:PASSWORD, or DOMAIN=:IDENTITY_TOKEN, for an allowed registry; keep them in secrets.yaml");
  add("api.http", Kind::value, "--api-http", "HOST:PORT for the API over plain HTTP");
  add("api.https", Kind::value, "--api-https", "HOST:PORT for the API over HTTPS");
  add("api.tls_chain", Kind::value, "--api-tls-chain", "the HTTPS certificate chain, PEM, leaf first");
  add("api.tls_key", Kind::value, "--api-tls-key", "the HTTPS certificate's key, PEM");
  add("lugh.issuer", Kind::list, "--lugh-issuer", "a trusted Lugh, as its tokens name it");
  add("lugh.ca", Kind::value, "--lugh-ca", "the roots the issuers' HTTPS chains to; the system's when absent");
  add("lir.enabled", Kind::toggle, "--lir", "publish events to Lir, its nodes found in Dannan's roster");
  add("lir.tls", Kind::toggle, "--lir-tls", "connect to Lir over TLS");
  add("lir.tls_trust", Kind::value, "--lir-tls-trust", "the roots Lir's certificate chains to, PEM; the system's when absent");
  add("lir.client_id", Kind::value, "--lir-client-id", "this node's Lugh client, for its token to Lir");
  add("lir.client_key", Kind::value, "--lir-client-key", "the client's signing key, PKCS#8 DER (private_key_jwt)");
  add("lir.client_kid", Kind::value, "--lir-client-kid", "the signing key's kid, as registered in Lugh");
  add("lir.client_secret", Kind::value, "--lir-client-secret", "the client's secret (client_secret_basic); keep it in secrets.yaml");
  return out;
}

Options parse(int argc, char** argv) {
  const auto settings = reader().read(std::vector<std::string>(argv + 1, argv + argc));
  if (!settings.positional.empty()) throw std::runtime_error("unexpected argument " + settings.positional.front());
  for (const auto& warning : settings.warnings) std::cerr << "louhi-node: " << warning << "\n";
  Options out;
  out.version = settings.toggle("command.version").value_or(false);
  if (out.version) return out;
  const auto ms = [&](const std::string& key, std::chrono::milliseconds otherwise) {
    const auto value = settings.number(key, 1, 86400000);
    return value ? std::chrono::milliseconds(*value) : otherwise;
  };
  for (const auto& text : settings.list("command.retire")) {
    const auto id = dannan::identity::NodeId::parse(text);
    if (!id) throw settings.refuse("command.retire", "a node id, 32 hex digits");
    out.retire.push_back(*id);
  }
  out.key = settings.path_of("cluster.key").value_or("");
  for (const auto& seed : settings.list("cluster.bootstrap")) out.bootstrap.push_back(endpoint(seed));
  out.state = settings.path_of("node.state").value_or("");
  out.name = settings.text("node.name").value_or("");
  out.domain = settings.text("node.domain").value_or("");
  out.listen = settings.text("node.listen").value_or(out.listen);
  out.port = static_cast<uint16_t>(settings.number("node.port", 0, 65535).value_or(out.port));
  out.advertise = settings.text("node.advertise").value_or("");
  out.heartbeat = ms("timing.heartbeat_ms", out.heartbeat);
  out.status = ms("timing.status_ms", out.status);
  out.seconds = static_cast<int>(settings.number("timing.seconds", 0, 31536000).value_or(0));
  if (const auto level = settings.text("log.level")) out.log_level = dannan::status::parse_log_level(*level);
  if (const auto format = settings.choice("log.format", {"text", "json"})) out.log_format = dannan::status::parse_log_format(*format);
  out.docker_socket = settings.path_of("runtime.docker_socket").value_or(out.docker_socket);
  out.allow_unenforced_memory = settings.toggle("runtime.allow_unenforced_memory").value_or(false);
  out.allow_without_user_namespaces = settings.toggle("runtime.allow_without_user_namespaces").value_or(false);
  out.allow_unbounded_storage = settings.toggle("runtime.allow_unbounded_storage").value_or(false);
  out.retain_finished = std::chrono::hours(settings.number("runtime.retain_finished_hours", 0, 8760).value_or(24));
  for (const auto& entry : settings.list("runtime.host_directories")) {
    const auto equals = entry.find('=');
    if (equals == std::string::npos || !louhi::model::is_name(entry.substr(0, equals)) || equals + 1 >= entry.size() || entry[equals + 1] != '/')
      throw settings.refuse("runtime.host_directories", "NAME=PATH, NAME a name and PATH absolute");
    if (!out.host_directories.emplace(entry.substr(0, equals), entry.substr(equals + 1)).second)
      throw settings.refuse("runtime.host_directories", "each NAME once");
  }
  out.output_directory = settings.path_of("runtime.output_directory").value_or("");
  out.registries = settings.list("registries.allow");
  for (const auto& entry : settings.list("registries.credentials")) {
    // A user name never holds ':' (RFC 7617), so the first ':' ends it; an
    // empty one means the rest is an identity token. The entry is a secret,
    // so a refusal never quotes it.
    const auto equals = entry.find('=');
    const auto colon = equals == std::string::npos ? std::string::npos : entry.find(':', equals + 1);
    if (colon == std::string::npos) throw std::runtime_error("a registry credential is not DOMAIN=USER:PASSWORD or DOMAIN=:IDENTITY_TOKEN");
    louhi::runtime::RegistryCredential credential;
    const std::string user = entry.substr(equals + 1, colon - equals - 1);
    if (user.empty()) {
      credential.identity_token = entry.substr(colon + 1);
    } else {
      credential.username = user;
      credential.password = entry.substr(colon + 1);
    }
    if (!out.registry_credentials.emplace(entry.substr(0, equals), std::move(credential)).second) throw std::runtime_error("a registry has two credentials");
  }
  if (const auto http = settings.text("api.http")) out.api_http = endpoint(*http);
  if (const auto https = settings.text("api.https")) out.api_https = endpoint(*https);
  out.api_tls_chain = settings.path_of("api.tls_chain").value_or("");
  out.api_tls_key = settings.path_of("api.tls_key").value_or("");
  out.lugh_issuers = settings.list("lugh.issuer");
  out.lugh_ca = settings.path_of("lugh.ca").value_or("");
  out.lir = settings.toggle("lir.enabled").value_or(false);
  out.lir_tls = settings.toggle("lir.tls").value_or(false);
  out.lir_tls_trust = settings.path_of("lir.tls_trust").value_or("");
  out.lir_client_id = settings.text("lir.client_id").value_or("");
  out.lir_client_key = settings.path_of("lir.client_key").value_or("");
  out.lir_client_kid = settings.text("lir.client_kid").value_or("");
  out.lir_client_secret = settings.text("lir.client_secret").value_or("");
  if (out.key.empty() || out.state.empty()) throw std::runtime_error("--key and --state are required");
  if (!out.lir_client_id.empty()) {
    if (out.lugh_issuers.empty()) throw std::runtime_error("--lir-client-id needs a --lugh-issuer to get its token from");
    if (out.lir_client_key.empty() == out.lir_client_secret.empty())
      throw std::runtime_error("--lir-client-id needs exactly one of --lir-client-key and --lir-client-secret");
    if (!out.lir_client_key.empty() && out.lir_client_kid.empty()) throw std::runtime_error("--lir-client-key needs --lir-client-kid");
  }
  if (out.api_https && (out.api_tls_chain.empty() || out.api_tls_key.empty())) throw std::runtime_error("--api-https needs --api-tls-chain and --api-tls-key");
  if ((out.api_http || out.api_https) && out.lugh_issuers.empty()) throw std::runtime_error("the API needs a --lugh-issuer: there is no anonymous sign-in");
  return out;
}

[[maybe_unused]] std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read " + path);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

uint64_t unix_ms() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

// What the runtime says of itself now, waiting at most `deadline` and a
// margin: the driver completes by its deadline, so the margin is never spent.
louhi::node::Facts observe(louhi::runtime::Driver& driver, asio::io_context& io, const Options& options, louhi::model::NodeId self, bool store_whole) {
  auto got = std::make_shared<std::promise<louhi::runtime::Result<louhi::runtime::RuntimeInfo>>>();
  auto future = got->get_future();
  const louhi::runtime::Deadline deadline(10'000);
  driver.describe(deadline, io.get_executor(), [got](louhi::runtime::Result<louhi::runtime::RuntimeInfo> r) { got->set_value(std::move(r)); });
  louhi::node::Facts facts;
  facts.self = self;
  facts.allow_unenforced_memory = options.allow_unenforced_memory;
  facts.allow_without_user_namespaces = options.allow_without_user_namespaces;
  facts.allow_unbounded_storage = options.allow_unbounded_storage;
  facts.store_whole = store_whole;
  if (future.wait_for(deadline + 5s) != std::future_status::ready) {
    facts.runtime_error = "no answer within the deadline";
    return facts;
  }
  auto r = future.get();
  if (r.ok())
    facts.runtime = r.value();
  else
    facts.runtime_error = r.error().message;
  return facts;
}

// --retire: opens the node's state without starting it, retires each node,
// and stops. 0 when every retire ran; 1 if the state could not be opened
// (the node is running) or a node could not be retired (this node itself).
int retire(const Options& options) {
  try {
    asio::io_context io;
    dannan::NodeConfig config;
    config.state_path = options.state;
    config.key = dannan::identity::load_cluster_key(options.key);
    config.log = dannan::status::console_sink(options.log_format, options.log_level);
    dannan::Node node(io, config);
    for (const auto& id : options.retire) {
      const bool retired = node.context().nodes().retire(id);
      say("{\"event\":\"retire\",\"id\":\"" + dannan::identity::to_string(id) + "\",\"retired\":" + (retired ? "true" : "false") + "}");
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "louhi-node: " << error.what() << "\n";
    return 1;
  }
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  try {
    options = parse(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "louhi-node: " << error.what() << "\n";
    return 2;
  }
  if (options.version) {
    std::cout << louhi::kVersion << "\n";
    return 0;
  }
  if (!options.retire.empty()) return retire(options);
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, [](int) { stopping = true; });
  signal(SIGINT, [](int) { stopping = true; });

  asio::io_context io;
  auto work = asio::make_work_guard(io);
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) threads.emplace_back([&] { io.run(); });

  int status = 0;
  try {
    dannan::NodeConfig config;
    config.state_path = options.state;
    config.key = dannan::identity::load_cluster_key(options.key);
    config.listen_host = options.listen;
    config.advertise_host = options.advertise;
    config.port = options.port;
    config.bootstrap = options.bootstrap;
    config.node_name = options.name;
    config.failure_domain = options.domain;
    config.heartbeat = options.heartbeat;
    config.dead_after = options.heartbeat * 6;
    config.telemetry_interval = options.heartbeat;
    config.version = std::string("louhi-node ") + louhi::kVersion;
    config.log = dannan::status::console_sink(options.log_format, options.log_level);
    // Held so it can be destroyed before the io threads end: its client's
    // stop waits on its own strand, so a Node destroyed after io.stop() never
    // returns (seen on a Pi, 2026-11-16).
    std::optional<dannan::Node> node_holder;
    node_holder.emplace(io, config);
    dannan::Node& node = *node_holder;
    auto& context = node.context();
    const louhi::model::NodeId self = context.identity().id();

    louhi::store::MemoryStore store(louhi::store::MemoryStore::Whole::never);
    louhi::agent::StoreDesired desired(store, self, context.logger("louhi.desired"));
    louhi::agent::StoreReports reports(store, io.get_executor(), context.logger("louhi.reports"));
    louhi::runtime::LinuxHostFiles host_files;
    louhi::runtime::docker::DockerDriver driver(io.get_executor(), options.docker_socket,
                                                louhi::runtime::Registries(options.registries, options.registry_credentials),
                                                {options.host_directories, options.output_directory, &host_files});
    // A timer on the io context, for the agent and the service token.
    const auto after = [&io](std::chrono::milliseconds delay, std::function<void()> fn) {
      auto timer = std::make_shared<asio::steady_timer>(io, delay);
      timer->async_wait([timer, fn = std::move(fn)](const boost::system::error_code& failed) {
        if (!failed) fn();
      });
    };

    // Events on Lir, when asked for: the agent's reports and the node's status
    // are published as well as stored.
#ifdef LOUHI_WITH_LUGH
    // The token's own HTTPS to its issuer, outliving the token.
    std::unique_ptr<lugh::client::HttpsClient> token_https;
    std::shared_ptr<lugh::client::TokenSource> service_token;
#endif
#ifdef LOUHI_WITH_LIR
    std::unique_ptr<louhi::events::LirPublisher> lir;
#endif
    std::unique_ptr<louhi::events::Events> events;
    std::unique_ptr<louhi::events::EventReports> event_reports;
    if (options.lir) {
#ifdef LOUHI_WITH_LIR
      louhi::events::LirConfig lir_config;
      lir_config.nodes = &context.nodes();
      if (options.lir_tls) {
        lir::client::Tls tls;
        tls.trust = options.lir_tls_trust;
        lir_config.tls = std::move(tls);
      }
      lir_config.client_id = "louhi-" + dannan::identity::to_string(self);
      if (!options.lir_client_id.empty()) {
#ifdef LOUHI_WITH_LUGH
        // Lugh's TokenSource: every trusted issuer, in order, failing over.
        lugh::client::TokenSource::Settings token;
        token.issuers = options.lugh_issuers;
        token.identity.client_id = options.lir_client_id;
        if (!options.lir_client_key.empty()) {
          const std::string der = read_file(options.lir_client_key);
          token.identity.method = lugh::client::ClientIdentity::Method::private_key_jwt;
          token.identity.key = std::make_shared<const dannan::identity::SigningKey>(
              dannan::identity::SigningKey::from_pkcs8(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(der.data()), der.size())));
          token.identity.kid = options.lir_client_kid;
        } else {
          token.identity.method = lugh::client::ClientIdentity::Method::client_secret_basic;
          token.identity.secret = std::make_shared<const dannan::identity::Secret>(
              std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(options.lir_client_secret.data()), options.lir_client_secret.size()));
        }
        token_https = std::make_unique<lugh::client::HttpsClient>(io, options.lugh_ca);
        service_token = lugh::client::TokenSource::make(*token_https, io.get_executor(), std::move(token), dannan::contract::steady_time_source(),
                                                        dannan::contract::system_wall_clock());
        lir_config.password = [service_token] { return service_token->current(); };
#else
        throw std::runtime_error("this build has no Lugh plugin, so it cannot get a token for Lir");
#endif
      }
      lir = std::make_unique<louhi::events::LirPublisher>(io.get_executor(), std::move(lir_config), louhi::events::node_topic(self),
                                                          dannan::contract::steady_time_source(), context.logger("louhi.lir"));
      events = std::make_unique<louhi::events::Events>(*lir, self, dannan::contract::system_wall_clock());
      event_reports = std::make_unique<louhi::events::EventReports>(reports, desired, *events);
#else
      throw std::runtime_error("this build has no Lir client, so it publishes no events");
#endif
    }
    louhi::agent::Reports& agent_reports = event_reports ? static_cast<louhi::agent::Reports&>(*event_reports) : reports;

    louhi::agent::Policy policy;
    policy.allow_unenforced_memory = options.allow_unenforced_memory;
    policy.allow_without_user_namespaces = options.allow_without_user_namespaces;
    policy.allow_unbounded_storage = options.allow_unbounded_storage;
    policy.retain_finished = options.retain_finished;
    louhi::agent::Clock clock{&dannan::contract::steady_time_source(), &dannan::contract::system_wall_clock(), after};
    auto agent = std::make_shared<louhi::agent::Agent>(driver, desired, agent_reports, self, clock, io.get_executor(), context.logger("louhi.agent"), policy);

    node.start();
    context.nodes().advertise("louhi", {});
#ifdef LOUHI_WITH_LUGH
    if (service_token) service_token->start();
#endif
#ifdef LOUHI_WITH_LIR
    if (lir) lir->start();
#endif
    // Desired state moving is a prompt to the agent, never the change itself.
    const auto tasks_watch = store.watch(louhi::model::tasks_prefix(""), io.get_executor(), [agent] { agent->poke(); });
    const auto definitions_watch = store.watch("taskdef/", io.get_executor(), [agent] { agent->poke(); });
    agent->start();

    say("{\"event\":\"listening\",\"id\":\"" + dannan::identity::to_string(self) + "\",\"port\":" + std::to_string(node.port()) + "}");

    auto logger = context.logger("louhi.node");

    // The API, when an issuer and a listener are given.
    louhi::api::Api api(store, dannan::contract::system_wall_clock(), self, driver, io.get_executor(), context.logger("louhi.api"),
                        louhi::store::Deadline(10'000), events.get());
#ifdef LOUHI_WITH_LUGH
    std::unique_ptr<louhi::auth::LughPlugin> lugh;
    std::unique_ptr<louhi::api::Authenticator> authenticator;
#endif
    std::unique_ptr<louhi::api::HttpServer> server;
    if (options.api_http || options.api_https) {
#ifdef LOUHI_WITH_LUGH
      louhi::auth::LughConfig lugh_config;
      lugh_config.issuers = options.lugh_issuers;
      lugh_config.ca_path = options.lugh_ca;
      lugh =
          std::make_unique<louhi::auth::LughPlugin>(io, std::move(lugh_config), dannan::contract::steady_time_source(), dannan::contract::system_wall_clock());
      lugh->start();
      authenticator = std::make_unique<louhi::api::Authenticator>(lugh->verifier(), dannan::contract::system_wall_clock());
      louhi::api::HttpConfig http;
      if (options.api_http) http.plain = louhi::api::HttpListen{options.api_http->host, options.api_http->port};
      if (options.api_https) {
        louhi::api::HttpsListen https;
        https.host = options.api_https->host;
        https.port = options.api_https->port;
        https.certificates.chain = options.api_tls_chain;
        https.certificates.key = options.api_tls_key;
        http.tls = std::move(https);
      }
      server = std::make_unique<louhi::api::HttpServer>(io, api, *authenticator, std::move(http), context.logger("louhi.http"));
      server->start();
      say("{\"event\":\"api\",\"http\":" + std::to_string(server->port()) + ",\"https\":" + std::to_string(server->tls_port()) + "}");
#else
      throw std::runtime_error("this build has no Lugh plugin, so it serves no API");
#endif
    }
    std::optional<louhi::model::NodeStatus> last;
    uint64_t sequence = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(options.seconds);
    auto next_status = std::chrono::steady_clock::now();
    while (!stopping && (options.seconds == 0 || std::chrono::steady_clock::now() < until)) {
      if (std::chrono::steady_clock::now() >= next_status) {
        const auto facts = observe(driver, io, options, self, store.ready());
        auto now = louhi::node::node_status(facts, desired.assignments(), sequence + 1, unix_ms());
        if (!last || !louhi::node::same_report(*last, now)) {
          ++sequence;
          for (const auto& condition : now.conditions) logger.warn(condition);
          const auto key = louhi::model::key(now);
          store.put(key, louhi::model::encode(now), louhi::store::Deadline(30'000), io.get_executor(),
                    [logger, key](louhi::runtime::Result<louhi::runtime::Done> r) {
                      if (!r.ok()) logger.warn("writing " + key + ": " + r.error().message);
                    });
          if (events) events->node_status(now);
          last = now;
        }
        say(std::string("{\"event\":\"status\",\"ready\":") + (now.ready ? "true" : "false") + ",\"tasks\":" + std::to_string(now.tasks) +
            ",\"passes\":" + std::to_string(agent->passes()) + ",\"conditions\":" + std::to_string(now.conditions.size()) + "}");
        next_status = std::chrono::steady_clock::now() + options.status;
      }
      std::this_thread::sleep_for(50ms);
    }

    if (server) server->stop();
#ifdef LOUHI_WITH_LUGH
    if (lugh) lugh->stop();
#endif
    store.unwatch(tasks_watch);
    store.unwatch(definitions_watch);
    std::promise<void> agent_stopped;
    agent->stop([&agent_stopped] { agent_stopped.set_value(); });
    if (agent_stopped.get_future().wait_for(60s) != std::future_status::ready) logger.warn("the agent's pass did not finish within 60 s; stopping anyway");
#ifdef LOUHI_WITH_LIR
    // A clean stop sends DISCONNECT, so the will does not run: the node's
    // retained status is cleared here first, as the will would have.
    if (lir) {
      events->node_gone();
      std::promise<void> lir_stopped;
      lir->stop([&lir_stopped] { lir_stopped.set_value(); });
      if (lir_stopped.get_future().wait_for(10s) != std::future_status::ready) logger.warn("Lir's client did not stop within 10 s; stopping anyway");
      if (lir->dropped() > 0) logger.warn(std::to_string(lir->dropped()) + " events were not taken by Lir");
    }
#endif
#ifdef LOUHI_WITH_LUGH
    if (service_token) service_token->stop();
#endif
    context.nodes().withdraw("louhi");
    node.stop();
    // Dannan's node goes while its io threads still run, as dannan-node's
    // does; nothing of Louhi's names it any more, everything Louhi started
    // having stopped above.
    node_holder.reset();
    // The io threads end here, while everything their handlers name (the
    // Api, the store, the plugin) still stands; what is still queued is
    // destroyed unrun with the io context.
    work.reset();
    io.stop();
    for (auto& thread : threads) thread.join();
    threads.clear();
  } catch (const std::exception& error) {
    std::cerr << "louhi-node: " << error.what() << "\n";
    status = 1;
  }
  work.reset();
  io.stop();
  for (auto& thread : threads)
    if (thread.joinable()) thread.join();
  say("{\"event\":\"stopped\"}");
  return status;
}
