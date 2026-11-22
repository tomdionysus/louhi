//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "louhi/model/ids.h"

// The records Louhi keeps in the store (design, "Records"). Declared state
// (Project, Quota, TaskDefinition, Service, Task) is written by callers and
// the system; observed state (TaskStatus, NodeStatus) only by the node that
// observed it, so a merge never reconciles an intent with a report.
//
// Every record has a validate() that throws std::invalid_argument naming the
// field at fault, never its value. A record is validated when it is made and
// again when it is decoded (codec.h), so a record held in memory is valid.
// Bounds below are part of the format: a record over one is refused.
namespace louhi::model {

// Bounds.
inline constexpr size_t max_description = 1024;
inline constexpr size_t max_reason = 1024;
inline constexpr size_t max_principal = 256;
inline constexpr size_t max_containers = 10;
inline constexpr size_t max_arguments = 256;
inline constexpr size_t max_argument = 4096;
inline constexpr size_t max_environment = 256;
inline constexpr size_t max_env_value = 32768;
inline constexpr size_t max_ports = 64;
inline constexpr size_t max_mounts = 32;
inline constexpr size_t max_volumes = 32;
inline constexpr size_t max_conditions = 32;
inline constexpr size_t max_condition = 256;
inline constexpr size_t max_version_text = 64;
// A task's size: CPU in thousandths of a core, memory in MiB.
inline constexpr uint32_t max_task_cpu_millis = 256'000;
inline constexpr uint32_t min_task_memory_mib = 4;
inline constexpr uint32_t max_task_memory_mib = 1'048'576;
inline constexpr uint32_t max_scratch_mib = 1'048'576;
// A task's writable space outside its volumes, in MiB, shared equally by its
// containers.
inline constexpr uint32_t min_ephemeral_storage_mib = 64;
inline constexpr uint32_t max_ephemeral_storage_mib = 1'048'576;
inline constexpr uint32_t max_desired_count = 10'000;

// A project: who owns a set of definitions, services, networks and volumes.
struct Project {
  std::string name;
  std::string description;
  uint64_t created_unix_ms = 0;
  bool operator==(const Project&) const = default;
  void validate() const;
};

// What a project may use at once. Written by the operator, so a project's own
// writers never move their own limits. A limit of zero allows none.
struct Quota {
  std::string project;
  uint32_t max_tasks = 0;
  uint32_t max_cpu_millis = 0;
  uint32_t max_memory_mib = 0;
  uint32_t max_addresses = 0;
  uint32_t max_volumes = 0;
  uint32_t max_load_balancers = 0;
  uint64_t updated_unix_ms = 0;
  bool operator==(const Quota&) const = default;
  void validate() const;
};

enum class Protocol : uint8_t { tcp = 1, udp = 2 };

struct PortMapping {
  uint16_t container_port = 0;
  Protocol protocol = Protocol::tcp;
  bool operator==(const PortMapping&) const = default;
};

struct EnvVar {
  std::string name;
  std::string value;
  bool operator==(const EnvVar&) const = default;
};

struct Mount {
  // A volume the task definition declares.
  std::string volume;
  std::string container_path;
  bool read_only = false;
  bool operator==(const Mount&) const = default;
};

// A command run inside the container; exit 0 is healthy.
struct HealthCheck {
  std::vector<std::string> command;
  uint16_t interval_s = 30;
  uint16_t timeout_s = 5;
  uint16_t retries = 3;
  uint16_t start_period_s = 0;
  bool operator==(const HealthCheck&) const = default;
};

struct ContainerDefinition {
  std::string name;
  // As the caller wrote it (image.h).
  std::string image;
  // The digest the reference resolved to when the revision was registered,
  // so every task of a revision runs the same bytes; empty until resolved.
  std::string image_digest;
  std::vector<std::string> entry_point;
  std::vector<std::string> command;
  std::vector<EnvVar> environment;
  std::string working_directory;
  std::vector<PortMapping> ports;
  std::vector<Mount> mounts;
  std::optional<HealthCheck> health_check;
  // Zero: none of its own, a share of the task's.
  uint32_t cpu_millis = 0;
  uint32_t memory_mib = 0;
  // A task stops when an essential container stops. At least one is.
  bool essential = true;
  bool read_only_root = false;
  bool operator==(const ContainerDefinition&) const = default;
};

// Scratch space for the task's life, gone when it stops (design, "Ephemeral
// storage in detail").
enum class VolumeKind : uint8_t {
  // In memory: its pages are charged to the task's memory, so a task's memory
  // volumes together fit in its memory.
  memory = 1,
  // On the node's disk, bounded by the runtime.
  disk = 2,
  // A read-only view of a path inside a host directory the operator names on
  // the node (the maintainer, 2026-11-22): a caller names the directory and
  // a relative path in it, never a host path.
  host = 3,
  // The task's own directory on the node's output area, writable, bounded by
  // a quota, mounted nosuid and nodev; kept with the task's containers, for
  // its results.
  output = 4,
};

struct VolumeDefinition {
  VolumeDefinition() = default;
  VolumeDefinition(std::string name_, VolumeKind kind_, uint32_t size_mib_ = 0, std::string host_directory_ = {}, std::string host_path_ = {})
      : name(std::move(name_)), kind(kind_), size_mib(size_mib_), host_directory(std::move(host_directory_)), host_path(std::move(host_path_)) {}

  std::string name;
  VolumeKind kind = VolumeKind::memory;
  // Every kind but host.
  uint32_t size_mib = 0;
  // host only: the operator's name for the directory, and a relative path in
  // it ("" for the directory itself).
  std::string host_directory;
  std::string host_path;
  bool operator==(const VolumeDefinition&) const = default;
};

// One revision of a family: immutable once registered. A change is a new
// revision.
struct TaskDefinition {
  std::string project;
  std::string family;
  uint32_t revision = 0;
  uint32_t cpu_millis = 0;
  uint32_t memory_mib = 0;
  // Bounds what its containers write outside their volumes.
  uint32_t ephemeral_storage_mib = 0;
  std::vector<ContainerDefinition> containers;
  std::vector<VolumeDefinition> volumes;
  // Must not run twice at once. No arrangement on this stack can promise
  // that; such a task is fenced by its placement epoch (Task) and its own
  // side effects must check it (design, "Placement").
  bool run_once = false;
  uint64_t registered_unix_ms = 0;
  bool operator==(const TaskDefinition&) const = default;
  void validate() const;
};

struct Deployment {
  // During a rollout, how many of the desired count must stay running, and
  // how many may run at most, as percentages of it.
  uint16_t minimum_healthy_percent = 100;
  uint16_t maximum_percent = 200;
  bool operator==(const Deployment&) const = default;
};

// Keeps desired_count tasks of one revision running.
struct Service {
  std::string project;
  std::string name;
  std::string family;
  uint32_t revision = 0;
  uint32_t desired_count = 0;
  Deployment deployment;
  bool spread_failure_domains = true;
  uint64_t updated_unix_ms = 0;
  bool operator==(const Service&) const = default;
  void validate() const;
};

enum class DesiredState : uint8_t { running = 1, stopped = 2 };

// One copy of a revision, wanted on one node.
struct Task {
  TaskId id;
  std::string project;
  std::string family;
  uint32_t revision = 0;
  // The service that made it; empty for a task run on its own.
  std::string service;
  DesiredState desired = DesiredState::running;
  // Why it was asked to stop; empty while it is wanted running.
  std::string stop_reason;
  // Where the system decided it runs; empty until placed.
  std::optional<NodeId> node;
  // Raised each time it is placed anew: the fence for a run_once task.
  uint64_t placement_epoch = 0;
  // Who asked for it, as the caller's principal names it.
  std::string started_by;
  uint64_t created_unix_ms = 0;
  bool operator==(const Task&) const = default;
  void validate() const;
};

enum class TaskState : uint8_t {
  pending = 1,
  pulling = 2,
  starting = 3,
  running = 4,
  stopping = 5,
  stopped = 6,
  // Retries spent: stopped and visible until an operator acts (discipline 2).
  parked = 7,
};

enum class ContainerState : uint8_t { created = 1, running = 2, exited = 3 };

enum class Health : uint8_t { unknown = 1, healthy = 2, unhealthy = 3 };

struct ContainerStatus {
  std::string name;
  ContainerState state = ContainerState::created;
  std::optional<int32_t> exit_code;
  uint32_t restarts = 0;
  Health health = Health::unknown;
  bool operator==(const ContainerStatus&) const = default;
};

// What one node observes of one task it runs. Its sequence rises with each
// report the node makes, so a later report replaces an earlier one.
struct TaskStatus {
  TaskId task;
  NodeId node;
  uint64_t sequence = 0;
  TaskState state = TaskState::pending;
  std::string reason;
  std::vector<ContainerStatus> containers;
  uint64_t started_unix_ms = 0;
  uint64_t stopped_unix_ms = 0;
  uint64_t observed_unix_ms = 0;
  bool operator==(const TaskStatus&) const = default;
  void validate() const;
};

// A task run on its own is done once the node it is placed on reports it
// stopped: it is never launched again, and holds nothing. `status` is a
// node's last report of it. A service's task is relaunched, never finished.
bool finished(const Task& task, const std::optional<TaskStatus>& status);
// Whether a task counts against its project's quota and its node's capacity:
// wanted running and not finished.
bool holds_resources(const Task& task, const std::optional<TaskStatus>& status);

struct Capacity {
  uint32_t cpu_millis = 0;
  uint32_t memory_mib = 0;
  bool operator==(const Capacity&) const = default;
};

// What one node observes of itself.
struct NodeStatus {
  NodeId node;
  uint64_t sequence = 0;
  std::string louhi_version;
  std::string runtime;
  std::string runtime_version;
  Capacity total;
  Capacity allocated;
  uint32_t tasks = 0;
  bool ready = false;
  // Why it is not ready, or what is degraded; empty when all is well.
  std::vector<std::string> conditions;
  uint64_t observed_unix_ms = 0;
  bool operator==(const NodeStatus&) const = default;
  void validate() const;
};

}  // namespace louhi::model
