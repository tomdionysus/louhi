//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/model/ids.h"

#include <algorithm>

#include "dannan/identity/crypto.h"

namespace louhi::model {

TaskId TaskId::random() {
  const auto fresh = dannan::identity::random_bytes(16);
  TaskId id;
  std::copy_n(fresh.begin(), id.bytes.size(), id.bytes.begin());
  return id;
}

std::optional<TaskId> TaskId::parse(std::string_view text) {
  if (text.size() != 32) return std::nullopt;
  const auto bytes = dannan::identity::unhex(std::string(text));
  if (!bytes) return std::nullopt;
  TaskId id;
  std::copy_n(bytes->begin(), id.bytes.size(), id.bytes.begin());
  return id;
}

std::string to_string(const TaskId& id) { return dannan::identity::hex(id.bytes); }

}  // namespace louhi::model
