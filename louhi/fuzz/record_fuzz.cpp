//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "louhi/model/codec.h"

namespace {

// Decoding either refuses the bytes or yields a record whose encoding is
// exactly those bytes: the format has one spelling of every record.
template <class Decode>
void check(std::span<const uint8_t> bytes, Decode decode) {
  try {
    const auto record = decode(bytes);
    const auto again = louhi::model::encode(record);
    if (again.size() != bytes.size() || !std::equal(again.begin(), again.end(), bytes.begin())) std::abort();
  } catch (const louhi::model::DecodeError&) {
  }
}

}  // namespace

// Any bytes the store hands back: every decoder sees them, as a record of
// any kind may be read from a corrupt or hostile replica.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace louhi::model;
  const std::span<const uint8_t> bytes(data, size);
  check(bytes, decode_project);
  check(bytes, decode_quota);
  check(bytes, decode_task_definition);
  check(bytes, decode_service);
  check(bytes, decode_task);
  check(bytes, decode_task_status);
  check(bytes, decode_node_status);
  check(bytes, decode_vpc);
  check(bytes, decode_subnet);
  check(bytes, decode_route_table);
  check(bytes, decode_security_group);
  try {
    peek_kind(bytes);
  } catch (const DecodeError&) {
  }
  return 0;
}
