//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/node/status.h"

#include <algorithm>
#include <limits>

#include "louhi/version.h"

namespace louhi::node {

namespace {

// Text from outside, cut to `maximum` bytes and stopped at any NUL.
std::string bounded(const std::string& text, size_t maximum) {
  std::string out = text.substr(0, std::min(text.find('\0'), maximum));
  return out;
}

uint32_t saturated(uint64_t value) { return static_cast<uint32_t>(std::min<uint64_t>(value, std::numeric_limits<uint32_t>::max())); }

void add(std::vector<std::string>& conditions, const std::string& condition) {
  if (conditions.size() < model::max_conditions) conditions.push_back(bounded(condition, model::max_condition));
}

}  // namespace

model::NodeStatus node_status(const Facts& facts, const std::vector<agent::Assignment>& placed, uint64_t sequence, uint64_t observed_unix_ms) {
  model::NodeStatus out;
  out.node = facts.self;
  out.sequence = sequence;
  out.observed_unix_ms = observed_unix_ms;
  out.louhi_version = bounded(kVersion, model::max_version_text);

  if (!facts.runtime) {
    out.ready = false;
    add(out.conditions, "runtime unreachable: " + facts.runtime_error);
  } else {
    const auto& r = *facts.runtime;
    out.runtime = bounded(r.name, model::max_version_text);
    out.runtime_version = bounded(r.version, model::max_version_text);
    out.total.cpu_millis = saturated(static_cast<uint64_t>(r.cpus) * 1000);
    out.total.memory_mib = saturated(r.memory_bytes >> 20);
    out.ready = (r.limits_memory || facts.allow_unenforced_memory) && (r.user_namespaces || facts.allow_without_user_namespaces);
    if (!r.limits_memory) {
      add(out.conditions, facts.allow_unenforced_memory ? "the runtime cannot enforce memory limits; tasks run anyway, as the operator allows"
                                                        : "the runtime cannot enforce memory limits; no task is launched");
    }
    if (!r.limits_cpu) add(out.conditions, "the runtime cannot enforce CPU limits");
    if (!r.limits_pids) add(out.conditions, "the runtime cannot enforce process limits");
    if (!r.user_namespaces) {
      add(out.conditions, facts.allow_without_user_namespaces
                              ? "the runtime has no user namespaces: root in a container is root on the node; tasks run anyway, as the operator allows"
                              : "the runtime has no user namespaces: root in a container is root on the node; no task is launched");
    }
  }
  if (!facts.store_whole) add(out.conditions, "the store holds only what was declared since this node started");

  uint64_t cpu = 0;
  uint64_t memory = 0;
  for (const auto& assignment : placed) {
    if (assignment.task.node != facts.self || assignment.task.desired != model::DesiredState::running) continue;
    cpu += assignment.definition.cpu_millis;
    memory += assignment.definition.memory_mib;
    if (out.tasks < std::numeric_limits<uint32_t>::max()) ++out.tasks;
  }
  out.allocated.cpu_millis = saturated(cpu);
  out.allocated.memory_mib = saturated(memory);
  return out;
}

bool same_report(const model::NodeStatus& a, const model::NodeStatus& b) {
  auto x = a;
  auto y = b;
  x.sequence = y.sequence = 0;
  x.observed_unix_ms = y.observed_unix_ms = 0;
  return x == y;
}

}  // namespace louhi::node
