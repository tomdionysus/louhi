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

namespace {

int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

TaskId TaskId::random() {
  const auto fresh = dannan::identity::random_bytes(16);
  TaskId id;
  std::copy_n(fresh.begin(), id.bytes.size(), id.bytes.begin());
  return id;
}

std::optional<TaskId> TaskId::parse(std::string_view text) {
  if (text.size() != 32) return std::nullopt;
  TaskId id;
  for (size_t i = 0; i < id.bytes.size(); ++i) {
    const int high = hex_value(text[2 * i]);
    const int low = hex_value(text[2 * i + 1]);
    if (high < 0 || low < 0) return std::nullopt;
    id.bytes[i] = static_cast<uint8_t>(high * 16 + low);
  }
  return id;
}

std::string to_string(const TaskId& id) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(32);
  for (const uint8_t b : id.bytes) {
    out.push_back(digits[b >> 4]);
    out.push_back(digits[b & 0x0f]);
  }
  return out;
}

}  // namespace louhi::model
