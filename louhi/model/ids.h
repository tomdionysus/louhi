//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "dannan/identity/node_id.h"

// Identifiers. A task is named by sixteen random bytes made where it is
// created, never by a counter, because no node can hand out a cluster-wide
// sequence. A node is Dannan's NodeId.
namespace louhi::model {

using NodeId = dannan::identity::NodeId;

struct TaskId {
  std::array<uint8_t, 16> bytes{};
  auto operator<=>(const TaskId&) const = default;

  static TaskId random();
  // Thirty-two hex digits, either case; empty on anything else.
  static std::optional<TaskId> parse(std::string_view text);
};

// Thirty-two lowercase hex digits.
std::string to_string(const TaskId& id);

}  // namespace louhi::model
