//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "louhi/agent/specs.h"
#include "louhi/model/records.h"
#include "louhi/runtime/driver.h"

// What a node reports of itself (design, "The node"): a NodeStatus built from
// what its runtime says and what is placed on it. Pure; the node writes it
// under its own key and advertises it.
namespace louhi::node {

struct Facts {
  model::NodeId self;
  // What the runtime said of itself; empty when it could not be reached.
  std::optional<runtime::RuntimeInfo> runtime;
  // Why it could not be reached.
  std::string runtime_error;
  // The operator allows tasks on a node that cannot enforce memory limits.
  bool allow_unenforced_memory = false;
  // Whether the store holds the whole desired state (store/store.h, ready).
  bool store_whole = true;
};

// The node is ready when its runtime answers and can enforce memory limits,
// or the operator allows it not to. Every degradation is a condition, even
// one the operator allows. Allocation counts the tasks placed here and wanted
// running, saturating. Text from the runtime is cut to the record's bounds,
// so the result always validates.
model::NodeStatus node_status(const Facts& facts, const std::vector<agent::Assignment>& placed, uint64_t sequence, uint64_t observed_unix_ms);

// Whether two reports say the same thing, sequence and time aside: a node
// writes and advertises again only when they differ.
bool same_report(const model::NodeStatus& a, const model::NodeStatus& b);

}  // namespace louhi::node
