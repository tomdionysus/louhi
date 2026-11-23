//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Any bytes the store hands back: every decoder sees them, as a record of any
// kind may be read from a corrupt or hostile replica. Decoding either refuses
// the bytes or yields a record whose encoding is exactly those bytes: the
// format has one spelling of every record.

#include <algorithm>
#include <span>
#include <stdexcept>
#include <vector>

#include "dannan/fuzz/fuzz.h"
#include "louhi/model/codec.h"
#include "louhi/test/fixtures/records.h"

namespace {

using namespace louhi::model;

template <class Decode>
void check(std::span<const uint8_t> bytes, Decode decode) {
  try {
    const auto record = decode(bytes);
    const auto again = encode(record);
    if (again.size() != bytes.size() || !std::equal(again.begin(), again.end(), bytes.begin()))
      throw std::logic_error("a record decoded does not encode to its bytes");
  } catch (const DecodeError&) {
  }
}

const dannan::fuzz::Registration records(
    "record",
    [](std::span<const uint8_t> bytes) {
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
    },
    [] {
      namespace fixture = louhi::test;
      auto with_mounts = fixture::task_definition();
      with_mounts.volumes.push_back({"source", VolumeKind::host, 0, "work", "web/src"});
      with_mounts.volumes.push_back({"out", VolumeKind::output, 64});
      return std::vector<dannan::net::Bytes>{
          encode(fixture::project()), encode(fixture::quota()), encode(fixture::task_definition()), encode(with_mounts),
          encode(fixture::service()), encode(fixture::task()),  encode(fixture::task_status()),     encode(fixture::node_status())};
    });

}  // namespace
