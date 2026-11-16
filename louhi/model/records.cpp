//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/model/records.h"

#include <set>
#include <stdexcept>
#include <utility>

#include "louhi/model/image.h"
#include "louhi/model/names.h"

namespace louhi::model {

namespace {

void require(bool condition, const std::string& why) {
  if (!condition) throw std::invalid_argument(why);
}

void require_text(const std::string& what, const std::string& text, size_t maximum) {
  require(text.size() <= maximum, what + " is longer than " + std::to_string(maximum) + " bytes");
  require(text.find('\0') == std::string::npos, what + " holds a NUL byte");
}

void require_arguments(const std::string& what, const std::vector<std::string>& arguments) {
  require(arguments.size() <= max_arguments, what + " has more than " + std::to_string(max_arguments) + " arguments");
  for (const auto& argument : arguments) require_text(what + " argument", argument, max_argument);
}

void validate_health_check(const std::string& container, const HealthCheck& check) {
  const std::string what = "container " + container + " health check";
  require(!check.command.empty(), what + " has no command");
  require_arguments(what + " command", check.command);
  require(check.interval_s >= 5 && check.interval_s <= 300, what + " interval is not 5 to 300 seconds");
  require(check.timeout_s >= 2 && check.timeout_s <= 120, what + " timeout is not 2 to 120 seconds");
  require(check.timeout_s < check.interval_s, what + " timeout is not shorter than its interval");
  require(check.retries >= 1 && check.retries <= 10, what + " retries is not 1 to 10");
  require(check.start_period_s <= 300, what + " start period is over 300 seconds");
}

void validate_container(const ContainerDefinition& c, const std::set<std::string>& volumes) {
  require_name("container name", c.name);
  const std::string what = "container " + c.name;
  parse_image_reference(c.image);
  require(c.image_digest.empty() || is_digest(c.image_digest), what + " image digest is not sha256 and 64 lowercase hex digits");
  require_arguments(what + " entry point", c.entry_point);
  require_arguments(what + " command", c.command);

  require(c.environment.size() <= max_environment, what + " has more than " + std::to_string(max_environment) + " environment variables");
  std::set<std::string> names;
  for (const auto& variable : c.environment) {
    require(is_env_name(variable.name), what + " has an environment variable whose name is not letters, digits and '_', not starting with a digit");
    require_text(what + " environment value", variable.value, max_env_value);
    require(names.insert(variable.name).second, what + " names an environment variable twice");
  }

  require(c.working_directory.empty() || is_container_path(c.working_directory), what + " working directory is not an absolute path");

  require(c.ports.size() <= max_ports, what + " has more than " + std::to_string(max_ports) + " ports");
  std::set<std::pair<uint16_t, Protocol>> ports;
  for (const auto& port : c.ports) {
    require(port.container_port != 0, what + " has port 0");
    require(port.protocol == Protocol::tcp || port.protocol == Protocol::udp, what + " has a port whose protocol is not tcp or udp");
    require(ports.insert({port.container_port, port.protocol}).second, what + " maps one port twice");
  }

  require(c.mounts.size() <= max_mounts, what + " has more than " + std::to_string(max_mounts) + " mounts");
  std::set<std::string> paths;
  for (const auto& mount : c.mounts) {
    require(volumes.count(mount.volume) == 1, what + " mounts a volume the task definition does not declare");
    require(is_container_path(mount.container_path) && mount.container_path != "/", what + " mount path is not an absolute path below '/'");
    require(paths.insert(mount.container_path).second, what + " mounts two volumes at one path");
  }

  if (c.health_check) validate_health_check(c.name, *c.health_check);
}

}  // namespace

void Project::validate() const {
  require_name("project name", name);
  require_text("project description", description, max_description);
}

void Quota::validate() const { require_name("quota project", project); }

void TaskDefinition::validate() const {
  require_name("task definition project", project);
  require_name("task definition family", family);
  require(revision >= 1, "task definition revision is not 1 or more");
  require(cpu_millis >= 1 && cpu_millis <= max_task_cpu_millis, "task definition cpu is not 1 to " + std::to_string(max_task_cpu_millis) + " millis");
  require(memory_mib >= min_task_memory_mib && memory_mib <= max_task_memory_mib,
          "task definition memory is not " + std::to_string(min_task_memory_mib) + " to " + std::to_string(max_task_memory_mib) + " MiB");

  require(
      ephemeral_storage_mib >= min_ephemeral_storage_mib && ephemeral_storage_mib <= max_ephemeral_storage_mib,
      "task definition ephemeral storage is not " + std::to_string(min_ephemeral_storage_mib) + " to " + std::to_string(max_ephemeral_storage_mib) + " MiB");

  require(volumes.size() <= max_volumes, "task definition has more than " + std::to_string(max_volumes) + " volumes");
  std::set<std::string> volume_names;
  uint64_t in_memory = 0;
  for (const auto& volume : volumes) {
    require_name("volume name", volume.name);
    require(volume.kind == VolumeKind::memory || volume.kind == VolumeKind::disk, "volume " + volume.name + " kind is not memory or disk");
    require(volume.size_mib >= 1 && volume.size_mib <= max_scratch_mib,
            "volume " + volume.name + " size is not 1 to " + std::to_string(max_scratch_mib) + " MiB");
    require(volume_names.insert(volume.name).second, "task definition declares a volume twice");
    if (volume.kind == VolumeKind::memory) in_memory += volume.size_mib;
  }
  // A memory volume's pages are charged to the task's memory.
  require(in_memory <= memory_mib, "task definition's memory volumes are larger than its memory");

  require(!containers.empty(), "task definition has no containers");
  require(containers.size() <= max_containers, "task definition has more than " + std::to_string(max_containers) + " containers");
  std::set<std::string> container_names;
  bool essential = false;
  uint64_t cpu = 0;
  uint64_t memory = 0;
  for (const auto& container : containers) {
    validate_container(container, volume_names);
    require(container_names.insert(container.name).second, "task definition names a container twice");
    essential = essential || container.essential;
    cpu += container.cpu_millis;
    memory += container.memory_mib;
  }
  require(essential, "task definition has no essential container");
  require(cpu <= cpu_millis, "task definition's containers ask for more cpu than the task has");
  require(memory <= memory_mib, "task definition's containers ask for more memory than the task has");
}

void Service::validate() const {
  require_name("service project", project);
  require_name("service name", name);
  require_name("service family", family);
  require(revision >= 1, "service revision is not 1 or more");
  require(desired_count <= max_desired_count, "service desired count is over " + std::to_string(max_desired_count));
  require(deployment.minimum_healthy_percent <= 100, "service minimum healthy percent is over 100");
  require(deployment.maximum_percent >= 100 && deployment.maximum_percent <= 200, "service maximum percent is not 100 to 200");
}

void Task::validate() const {
  require_name("task project", project);
  require_name("task family", family);
  require(revision >= 1, "task revision is not 1 or more");
  require(service.empty() || is_name(service), "task service is not empty or a name");
  require(desired == DesiredState::running || desired == DesiredState::stopped, "task desired state is not running or stopped");
  require_text("task stop reason", stop_reason, max_reason);
  require(desired == DesiredState::stopped || stop_reason.empty(), "task has a stop reason but is wanted running");
  require(node.has_value() || placement_epoch == 0, "task has a placement epoch but no node");
  require_text("task started by", started_by, max_principal);
}

void TaskStatus::validate() const {
  require(state >= TaskState::pending && state <= TaskState::parked, "task status state is not known");
  require_text("task status reason", reason, max_reason);
  require(containers.size() <= max_containers, "task status has more than " + std::to_string(max_containers) + " containers");
  std::set<std::string> names;
  for (const auto& container : containers) {
    require_name("task status container name", container.name);
    require(container.state >= ContainerState::created && container.state <= ContainerState::exited, "task status container state is not known");
    require(container.health >= Health::unknown && container.health <= Health::unhealthy, "task status container health is not known");
    require(container.state == ContainerState::exited || !container.exit_code, "task status container has an exit code but has not exited");
    require(names.insert(container.name).second, "task status names a container twice");
  }
}

void NodeStatus::validate() const {
  require_text("node status louhi version", louhi_version, max_version_text);
  require_text("node status runtime", runtime, max_version_text);
  require_text("node status runtime version", runtime_version, max_version_text);
  require(conditions.size() <= max_conditions, "node status has more than " + std::to_string(max_conditions) + " conditions");
  for (const auto& condition : conditions) require_text("node status condition", condition, max_condition);
}

}  // namespace louhi::model
