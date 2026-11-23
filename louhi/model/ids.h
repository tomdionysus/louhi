//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include "dannan/identity/node_id.h"
#include "dannan/identity/random_id.h"

// Identifiers. A task is named by sixteen random bytes made where it is
// created, never by a counter, because no node can hand out a cluster-wide
// sequence: Dannan's RandomId, a type of its own (random(), parse() of
// thirty-two hex digits, to_string() as thirty-two lowercase ones). A node is
// Dannan's NodeId.
namespace louhi::model {

using NodeId = dannan::identity::NodeId;

struct TaskTag;
using TaskId = dannan::identity::RandomId<TaskTag>;
using dannan::identity::to_string;

}  // namespace louhi::model
