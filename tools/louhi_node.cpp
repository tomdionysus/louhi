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
//   {"event":"status","ready":true,"tasks":N,"passes":N,"conditions":N}   every --status-ms
//   {"event":"stopped"}
//
// It runs for --seconds (0: until SIGTERM or SIGINT). Stopping it leaves the
// tasks running; a node started again adopts them.
//
// Every setting is also a key in a YAML file (--config PATH, $LOUHI_CONFIG,
// or /usr/local/etc/louhi/config.yaml when it exists), in secrets.yaml beside
// it, or a LOUHI_* variable, the flags winning (reader() below lists them).
//
//   louhi-node --key FILE --state DIR [--port N] [--listen ADDR]
//              [--advertise HOST] [--bootstrap HOST:PORT]... [--name NAME]
//              [--domain NAME] [--heartbeat-ms N] [--status-ms N]
//              [--seconds N] [--log-level LEVEL] [--log-format text|json]
//              [--docker-socket PATH] [--allow-unenforced-memory]
//              [--allow-without-user-namespaces]
//   louhi-node --version
//
// The store is the in-memory one until the Dagda store exists (ACTIVE.md):
// it holds only what was declared since this node started, so the node
// treats it as never whole and removes nothing it does not recognise. Until
// the API (step 5), nothing declares tasks to it; the node checks and reports
// its runtime, and adopts what it already runs.

#include <signal.h>

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "dannan/config/config.h"
#include "dannan/node.h"
#include "louhi/agent/agent.h"
#include "louhi/agent/store_bindings.h"
#include "louhi/model/codec.h"
#include "louhi/model/keys.h"
#include "louhi/node/status.h"
#include "louhi/runtime/docker/docker_driver.h"
#include "louhi/store/memory_store.h"
#include "louhi/version.h"

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
  std::string docker_socket = "/var/run/docker.sock";
  bool allow_unenforced_memory = false;
  bool allow_without_user_namespaces = false;
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
  out.default_path = "/usr/local/etc/louhi/config.yaml";
  const auto add = [&](std::string key, Kind kind, std::string flag, std::string what) {
    std::string variable = "LOUHI_";
    for (const char c : key) variable += c == '.' ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    out.settings.push_back({std::move(key), kind, std::move(flag), std::move(variable), std::move(what)});
  };
  add("command.version", Kind::toggle, "--version", "print the version and stop");
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
  if (out.key.empty() || out.state.empty()) throw std::runtime_error("--key and --state are required");
  return out;
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
    dannan::Node node(io, config);
    auto& context = node.context();
    const louhi::model::NodeId self = context.identity().id();

    louhi::store::MemoryStore store(louhi::store::MemoryStore::Whole::never);
    louhi::agent::StoreDesired desired(store, self, context.logger("louhi.desired"));
    louhi::agent::StoreReports reports(store, io.get_executor(), context.logger("louhi.reports"));
    louhi::runtime::docker::DockerDriver driver(io.get_executor(), options.docker_socket);

    louhi::agent::Policy policy;
    policy.allow_unenforced_memory = options.allow_unenforced_memory;
    policy.allow_without_user_namespaces = options.allow_without_user_namespaces;
    louhi::agent::Clock clock{&dannan::contract::steady_time_source(), &dannan::contract::system_wall_clock(),
                              [&io](std::chrono::milliseconds delay, std::function<void()> fn) {
                                auto timer = std::make_shared<asio::steady_timer>(io, delay);
                                timer->async_wait([timer, fn = std::move(fn)](const boost::system::error_code& failed) {
                                  if (!failed) fn();
                                });
                              }};
    auto agent = std::make_shared<louhi::agent::Agent>(driver, desired, reports, self, clock, io.get_executor(), context.logger("louhi.agent"), policy);

    node.start();
    context.nodes().advertise("louhi", {});
    // Desired state moving is a prompt to the agent, never the change itself.
    const auto tasks_watch = store.watch(louhi::model::tasks_prefix(""), io.get_executor(), [agent] { agent->poke(); });
    const auto definitions_watch = store.watch("taskdef/", io.get_executor(), [agent] { agent->poke(); });
    agent->start();

    say("{\"event\":\"listening\",\"id\":\"" + dannan::identity::to_string(self) + "\",\"port\":" + std::to_string(node.port()) + "}");

    auto logger = context.logger("louhi.node");
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
          last = now;
        }
        say(std::string("{\"event\":\"status\",\"ready\":") + (now.ready ? "true" : "false") + ",\"tasks\":" + std::to_string(now.tasks) +
            ",\"passes\":" + std::to_string(agent->passes()) + ",\"conditions\":" + std::to_string(now.conditions.size()) + "}");
        next_status = std::chrono::steady_clock::now() + options.status;
      }
      std::this_thread::sleep_for(50ms);
    }

    store.unwatch(tasks_watch);
    store.unwatch(definitions_watch);
    std::promise<void> agent_stopped;
    agent->stop([&agent_stopped] { agent_stopped.set_value(); });
    if (agent_stopped.get_future().wait_for(60s) != std::future_status::ready) logger.warn("the agent's pass did not finish within 60 s; stopping anyway");
    context.nodes().withdraw("louhi");
    node.stop();
  } catch (const std::exception& error) {
    std::cerr << "louhi-node: " << error.what() << "\n";
    status = 1;
  }
  work.reset();
  io.stop();
  for (auto& thread : threads) thread.join();
  say("{\"event\":\"stopped\"}");
  return status;
}
