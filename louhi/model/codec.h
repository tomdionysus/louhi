//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <span>

#include "dannan/net/codec.h"
#include "louhi/model/records.h"

// Each record's bytes, in Dannan's encoding (big-endian integers, a string as
// a u32 length then its bytes). Every record opens with two bytes: its kind
// and its format version, both checked, so a record is never read as another
// kind or as a version this build does not know (a mismatch is refused, as
// every stack format is).
//
// A decoder is a parser of untrusted bytes: every count and length is checked
// against the record's bounds before anything is allocated, every enum and
// flag byte must be one the format defines, nothing may be left over, and the
// result must validate. Anything else throws dannan::net::DecodeError.
// Decoding what encode() made gives the same record back, and encoding a
// decoded record gives the same bytes back.
namespace louhi::model {

using Bytes = dannan::net::Bytes;
using DecodeError = dannan::net::DecodeError;

enum class Kind : uint8_t {
  project = 1,
  quota = 2,
  task_definition = 3,
  service = 4,
  task = 5,
  task_status = 6,
  node_status = 7,
};

// The format version every kind is written in by this build. 2: a task
// definition's ephemeral storage, and disk volumes (step 13). Nothing written
// in 1 was ever kept, so 1 is simply refused.
inline constexpr uint8_t format_version = 2;

// The kind a record's bytes say they hold. Throws DecodeError for bytes too
// short to say, or a kind this build does not know.
Kind peek_kind(std::span<const uint8_t> bytes);

Bytes encode(const Project& record);
Bytes encode(const Quota& record);
Bytes encode(const TaskDefinition& record);
Bytes encode(const Service& record);
Bytes encode(const Task& record);
Bytes encode(const TaskStatus& record);
Bytes encode(const NodeStatus& record);

Project decode_project(std::span<const uint8_t> bytes);
Quota decode_quota(std::span<const uint8_t> bytes);
TaskDefinition decode_task_definition(std::span<const uint8_t> bytes);
Service decode_service(std::span<const uint8_t> bytes);
Task decode_task(std::span<const uint8_t> bytes);
TaskStatus decode_task_status(std::span<const uint8_t> bytes);
NodeStatus decode_node_status(std::span<const uint8_t> bytes);

}  // namespace louhi::model
