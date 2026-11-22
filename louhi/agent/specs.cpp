//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/agent/specs.h"

#include <algorithm>

#include "louhi/model/image.h"
#include "louhi/runtime/labels.h"

namespace louhi::agent {

namespace {

constexpr uint64_t mib = 1024 * 1024;
constexpr uint32_t least_cpu_millis = 1;
constexpr uint32_t least_memory_mib = 4;

runtime::Labels labels_of(const Assignment& a) {
  return {
      {std::string(runtime::label_task), model::to_string(a.task.id)},
      {std::string(runtime::label_project), a.task.project},
      {std::string(runtime::label_family), a.task.family},
      {std::string(runtime::label_revision), std::to_string(a.task.revision)},
  };
}

// The share each undeclaring container gets of what the declaring ones left.
uint32_t share(uint64_t total, uint64_t declared, size_t undeclared, uint32_t least) {
  if (undeclared == 0) return 0;
  const uint64_t left = total > declared ? total - declared : 0;
  return std::max<uint32_t>(least, static_cast<uint32_t>(left / undeclared));
}

}  // namespace

std::string image_of(const model::ContainerDefinition& container) {
  if (container.image_digest.empty()) return container.image;
  model::ImageReference reference = model::parse_image_reference(container.image);
  reference.digest = container.image_digest;
  return model::to_string(reference);
}

std::vector<runtime::VolumeSpec> volume_specs(const Assignment& a, const SpecPolicy& policy) {
  std::vector<runtime::VolumeSpec> out;
  for (const auto& volume : a.definition.volumes) {
    runtime::VolumeSpec spec;
    spec.name = runtime::volume_name(a.task.id, volume.name);
    switch (volume.kind) {
      case model::VolumeKind::disk:
        spec.kind = runtime::VolumeKind::disk;
        break;
      case model::VolumeKind::host:
        spec.kind = runtime::VolumeKind::host;
        spec.host_directory = volume.host_directory;
        spec.host_path = volume.host_path;
        break;
      case model::VolumeKind::output:
        spec.kind = runtime::VolumeKind::output;
        spec.owner_uid = policy.root_uid;
        spec.owner_gid = policy.root_gid;
        break;
      default:
        spec.kind = runtime::VolumeKind::memory;
        break;
    }
    if (spec.kind == runtime::VolumeKind::memory || (spec.kind != runtime::VolumeKind::host && policy.bound_storage))
      spec.size_bytes = static_cast<uint64_t>(volume.size_mib) * mib;
    spec.labels = labels_of(a);
    out.push_back(std::move(spec));
  }
  return out;
}

std::vector<runtime::ContainerSpec> container_specs(const Assignment& a, const SpecPolicy& policy) {
  uint64_t declared_cpu = 0;
  uint64_t declared_memory = 0;
  size_t undeclared_cpu = 0;
  size_t undeclared_memory = 0;
  for (const auto& c : a.definition.containers) {
    declared_cpu += c.cpu_millis;
    declared_memory += c.memory_mib;
    undeclared_cpu += c.cpu_millis == 0 ? 1 : 0;
    undeclared_memory += c.memory_mib == 0 ? 1 : 0;
  }
  const uint32_t cpu_share = share(a.definition.cpu_millis, declared_cpu, undeclared_cpu, least_cpu_millis);
  const uint32_t memory_share = share(a.definition.memory_mib, declared_memory, undeclared_memory, least_memory_mib);

  std::vector<runtime::ContainerSpec> out;
  for (const auto& c : a.definition.containers) {
    runtime::ContainerSpec s;
    s.name = runtime::container_name(a.task.id, c.name);
    s.image = image_of(c);
    s.entry_point = c.entry_point;
    s.command = c.command;
    for (const auto& variable : c.environment) s.environment.emplace_back(variable.name, variable.value);
    s.working_directory = c.working_directory;
    for (const auto& mount : c.mounts) {
      // A host directory is never writable, whatever reached here.
      const auto declared = std::find_if(a.definition.volumes.begin(), a.definition.volumes.end(), [&](const auto& v) { return v.name == mount.volume; });
      const bool host = declared != a.definition.volumes.end() && declared->kind == model::VolumeKind::host;
      s.mounts.push_back({runtime::volume_name(a.task.id, mount.volume), mount.container_path, mount.read_only || host});
    }
    if (c.health_check) {
      const auto& h = *c.health_check;
      s.health_check = runtime::HealthCheck{h.command, std::chrono::seconds(h.interval_s), std::chrono::seconds(h.timeout_s), h.retries,
                                            std::chrono::seconds(h.start_period_s)};
    }
    s.cpu_millis = c.cpu_millis != 0 ? c.cpu_millis : cpu_share;
    s.memory_bytes = static_cast<uint64_t>(c.memory_mib != 0 ? c.memory_mib : memory_share) * mib;
    s.pids_limit = policy.pids_per_container;
    if (policy.bound_storage) s.storage_bytes = static_cast<uint64_t>(a.definition.ephemeral_storage_mib) * mib / a.definition.containers.size();
    s.read_only_root = c.read_only_root;
    s.labels = labels_of(a);
    s.labels.emplace(std::string(runtime::label_container), c.name);
    out.push_back(std::move(s));
  }
  return out;
}

}  // namespace louhi::agent
