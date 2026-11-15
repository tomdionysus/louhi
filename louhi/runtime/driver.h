//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// The container runtime as the agent sees it (design, "The runtime"): a
// contract in Louhi's terms (containers, images, volumes, limits, isolation),
// never one runtime's own, so a microVM driver can follow the Docker one.
//
// Every operation is asynchronous in the stack's shape: it takes a deadline
// and the caller's executor, and posts its completion there exactly once,
// never inline, at the latest when the deadline passes. Waits on nothing.
// Thread-safe.
//
// Containers and volumes Louhi makes carry labels (louhi.task, louhi.container
// and the rest, labels.h), and list() returns only those: the agent re-derives
// what this node runs from the runtime itself after a restart (discipline 1),
// never from a memory of its own.
namespace louhi::runtime {

using Executor = boost::asio::any_io_executor;
using Labels = std::map<std::string, std::string>;

// Why an operation did not happen.
struct Error {
  enum class Kind {
    // The runtime could not be reached, or failed inside.
    unavailable,
    // The runtime refused it: a bad request, a conflict, a pull denied.
    refused,
    // What was named does not exist.
    not_found,
    // The deadline passed first.
    timed_out,
    // The runtime's reply was not understood.
    malformed,
  };
  Kind kind = Kind::unavailable;
  // For a person: what was asked and what came back. Bounded; never holds an
  // environment value or other secret Louhi sent.
  std::string message;
  bool operator==(const Error&) const = default;
};

const char* kind_name(Error::Kind kind) noexcept;

// What an operation completes with: its value, or why there is none.
template <class T>
class Result {
 public:
  Result(T value) : _value(std::move(value)) {}
  Result(Error error) : _value(std::move(error)) {}
  bool ok() const noexcept { return _value.index() == 0; }
  const T& value() const { return std::get<0>(_value); }
  T& value() { return std::get<0>(_value); }
  const Error& error() const { return std::get<1>(_value); }

 private:
  std::variant<T, Error> _value;
};

// The value of an operation that returns nothing but that it happened.
struct Done {
  bool operator==(const Done&) const = default;
};

template <class T>
using Completion = std::function<void(Result<T>)>;

using Deadline = std::chrono::milliseconds;

struct RuntimeInfo {
  // "docker".
  std::string name;
  std::string version;
  std::string api_version;
  // Whether containers run in a user namespace, so root inside is not root on
  // the node. Louhi refuses to run tasks where this is false unless the
  // operator says otherwise (the node checks and reports it).
  bool user_namespaces = false;
  // The cgroup version, and which limits the runtime can enforce on this node.
  // A limit it cannot enforce is discarded silently by the runtime, so the
  // node refuses tasks that need one it lacks (the Raspberry Pi kernel boots
  // with the memory controller disabled unless told otherwise).
  int cgroup_version = 0;
  bool limits_memory = false;
  bool limits_swap = false;
  bool limits_cpu = false;
  bool limits_pids = false;
  uint32_t cpus = 0;
  uint64_t memory_bytes = 0;
};

struct Mount {
  std::string volume;
  std::string target;
  bool read_only = false;
  bool operator==(const Mount&) const = default;
};

struct HealthCheck {
  std::vector<std::string> command;
  std::chrono::seconds interval{30};
  std::chrono::seconds timeout{5};
  uint32_t retries = 3;
  std::chrono::seconds start_period{0};
  bool operator==(const HealthCheck&) const = default;
};

// One container as Louhi asks for it. Every field that bounds the container
// is always set; there is no "unlimited".
struct ContainerSpec {
  // The runtime's name for it, unique on the node (labels.h, container_name).
  std::string name;
  // A reference with its digest when one is known.
  std::string image;
  std::vector<std::string> entry_point;
  std::vector<std::string> command;
  std::vector<std::pair<std::string, std::string>> environment;
  std::string working_directory;
  std::vector<Mount> mounts;
  std::optional<HealthCheck> health_check;
  // CPU in thousandths of a core; memory in bytes, with no swap beyond it.
  uint32_t cpu_millis = 0;
  uint64_t memory_bytes = 0;
  uint32_t pids_limit = 0;
  bool read_only_root = false;
  Labels labels;
  bool operator==(const ContainerSpec&) const = default;
};

struct VolumeSpec {
  std::string name;
  // Scratch: memory-backed, bounded, gone with the volume.
  uint64_t size_bytes = 0;
  Labels labels;
  bool operator==(const VolumeSpec&) const = default;
};

struct ImageInfo {
  // As asked for.
  std::string reference;
  // "sha256:..." of the manifest the reference resolved to, when the runtime
  // knows it (a locally built image has none).
  std::string digest;
};

enum class ContainerState { created, running, paused, restarting, exited, removing, dead };

enum class Health { none, starting, healthy, unhealthy };

struct ContainerInfo {
  std::string id;
  std::string name;
  std::string image;
  Labels labels;
  ContainerState state = ContainerState::created;
  std::optional<int32_t> exit_code;
  bool oom_killed = false;
  Health health = Health::none;
  // Zero when it never started, or has not finished.
  uint64_t started_unix_ms = 0;
  uint64_t finished_unix_ms = 0;
  bool operator==(const ContainerInfo&) const = default;
};

struct VolumeInfo {
  std::string name;
  Labels labels;
  bool operator==(const VolumeInfo&) const = default;
};

// Something happened to one of Louhi's containers. A prompt to look, never the
// truth: the agent re-lists rather than trusting a sequence of events.
struct ContainerEvent {
  std::string id;
  // "start", "die", "oom", "health_status", "destroy", and so on.
  std::string action;
  Labels labels;
  std::optional<int32_t> exit_code;
  bool operator==(const ContainerEvent&) const = default;
};

class Driver {
 public:
  virtual ~Driver() = default;

  virtual void describe(Deadline deadline, Executor executor, Completion<RuntimeInfo> done) = 0;

  // Fetches an image if it is not here. Completes with what it resolved to.
  virtual void pull(const std::string& image, Deadline deadline, Executor executor, Completion<ImageInfo> done) = 0;

  virtual void create_volume(const VolumeSpec& spec, Deadline deadline, Executor executor, Completion<Done> done) = 0;
  // Removing a volume that is not there succeeds.
  virtual void remove_volume(const std::string& name, Deadline deadline, Executor executor, Completion<Done> done) = 0;
  // Louhi's volumes only.
  virtual void list_volumes(Deadline deadline, Executor executor, Completion<std::vector<VolumeInfo>> done) = 0;

  // Completes with the runtime's id for the new container. A name already in
  // use is refused.
  virtual void create(const ContainerSpec& spec, Deadline deadline, Executor executor, Completion<std::string> done) = 0;
  // Starting a running container succeeds.
  virtual void start(const std::string& id, Deadline deadline, Executor executor, Completion<Done> done) = 0;
  // Asks it to stop, then kills it after `grace`. Stopping a stopped container
  // succeeds.
  virtual void stop(const std::string& id, std::chrono::seconds grace, Deadline deadline, Executor executor, Completion<Done> done) = 0;
  // Removes it, running or not, and its anonymous storage. Removing one that is
  // not there succeeds.
  virtual void remove(const std::string& id, Deadline deadline, Executor executor, Completion<Done> done) = 0;
  // Every container of Louhi's on this node, as it is now.
  virtual void list(Deadline deadline, Executor executor, Completion<std::vector<ContainerInfo>> done) = 0;

  // Events about Louhi's containers, posted to `executor` in order, until
  // unwatch(). If the stream is lost (the runtime restarted), `lost` is posted
  // once and nothing more comes: the caller re-lists and watches again.
  virtual uint64_t watch(Executor executor, std::function<void(ContainerEvent)> event, std::function<void(Error)> lost) = 0;
  virtual void unwatch(uint64_t id) = 0;
};

}  // namespace louhi::runtime
