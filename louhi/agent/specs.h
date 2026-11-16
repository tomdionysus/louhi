//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "louhi/model/records.h"
#include "louhi/runtime/driver.h"

// What a node asks its runtime for to run one task: a pure translation from
// the records to the driver's specs, so every rule is tested without a
// runtime.
//
// Each container gets its own CPU and memory when it declares them; the
// containers that declare none share what is left of the task's equally, and
// never get nothing (at least one thousandth of a core and four MiB each).
// Until tasks have a cgroup of their own (a later step), the task's size is
// therefore enforced as the sum of its containers' limits. The task's
// ephemeral storage is shared equally between its containers.
namespace louhi::agent {

struct Assignment {
  model::Task task;
  model::TaskDefinition definition;
  bool operator==(const Assignment&) const = default;
};

// Settings that are the node's, not the task's.
struct SpecPolicy {
  uint32_t pids_per_container = 1024;
  // Whether the runtime here bounds storage. When false (the operator allowed
  // such a node), containers and disk volumes carry no storage bound, since
  // the runtime would refuse one.
  bool bound_storage = true;
};

// The image a container runs: pinned to its digest when the revision has one.
std::string image_of(const model::ContainerDefinition& container);

std::vector<runtime::VolumeSpec> volume_specs(const Assignment& assignment, const SpecPolicy& policy);
// In the definition's order, which is the order they are started in.
std::vector<runtime::ContainerSpec> container_specs(const Assignment& assignment, const SpecPolicy& policy);

}  // namespace louhi::agent
