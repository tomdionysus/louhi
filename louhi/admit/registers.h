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
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "dannan/agree/caspaxos.h"
#include "louhi/model/ids.h"
#include "louhi/model/records.h"

// What Louhi keeps in Dannan's agreement registers (design, "Projects and
// quotas"; the maintainer, 2026-11-20): a family's next revision, and a project's
// admitted tasks. Each is a value, its versioned codec, and the changes made
// of it, as pure functions a register runs on its current value.
//
// A change may run more than once, on retries and on a fresh base, so each
// is deterministic over its input, and each recognises its own mark in the
// value (Changed::repeatable): a revision taken by a request answers that
// request again with the same number, and a task admitted or released is
// found by its id. So a caller that saw no answer (Outcome::unknown, or a
// deadline) asks again with the same request, and nothing is counted twice.
//
// A change cannot refuse: an admission over quota commits the value as it
// was and says so in its result.
//
// The decoders read untrusted bytes (another node's register): bounded,
// versioned, nothing left over; anything else throws dannan::net::DecodeError.
namespace louhi::admit {

using Bytes = dannan::agree::Bytes;

// Names one call, made where the call is made: sixteen random bytes.
struct RequestTag;
using RequestId = dannan::identity::RandomId<RequestTag>;

// --- Revision numbers: a register per task definition family.

// How many requests the counter remembers. A request retried after this many
// others is given a new number, so the first one it was given is a gap,
// never a duplicate.
inline constexpr size_t remembered_requests = 256;

struct RevisionCounter {
  struct Given {
    RequestId request;
    uint32_t revision = 0;
    bool operator==(const Given&) const = default;
  };
  // The revision the next request is given; revisions start at 1.
  uint32_t next = 1;
  // Oldest first, at most remembered_requests.
  std::deque<Given> given;
  bool operator==(const RevisionCounter&) const = default;
};

Bytes encode(const RevisionCounter& counter);
RevisionCounter decode_revision_counter(std::span<const uint8_t> bytes);

// What a take came to: the revision given, or none when the family has
// given every revision there is.
struct RevisionTaken {
  std::optional<uint32_t> revision;
  bool operator==(const RevisionTaken&) const = default;
};

Bytes encode(const RevisionTaken& taken);
RevisionTaken decode_revision_taken(std::span<const uint8_t> bytes);

// Gives `request` the next revision, or the one it was already given.
dannan::agree::Change take_revision(RequestId request);

// --- Quota admission: a register per project.

// What one task holds of its project's quota while it is admitted.
struct TaskShare {
  uint32_t cpu_millis = 0;
  uint32_t memory_mib = 0;
  auto operator<=>(const TaskShare&) const = default;
};

// The sums of what is admitted.
struct Usage {
  uint64_t tasks = 0;
  uint64_t cpu_millis = 0;
  uint64_t memory_mib = 0;
  bool operator==(const Usage&) const = default;
};

// The most tasks a project's register holds: a quota above it admits no more.
inline constexpr size_t max_admitted_tasks = 10'000;

// The tasks admitted, each with its share: usage is derived from it, never
// kept as a counter (discipline 1).
struct Admitted {
  std::map<model::TaskId, TaskShare> tasks;
  Usage usage() const;
  bool operator==(const Admitted&) const = default;
};

Bytes encode(const Admitted& admitted);
Admitted decode_admitted(std::span<const uint8_t> bytes);

enum class Verdict : uint8_t {
  // The tasks are admitted (now, or by an earlier run of the same request).
  admitted = 1,
  // Admitting them would pass `limit`; nothing changed.
  refused = 2,
  // The task's share is given back.
  released = 3,
  // The task was not admitted, so nothing was given back.
  absent = 4,
};

// Which limit a refusal would have passed.
enum class Limit : uint8_t { none = 0, tasks = 1, cpu = 2, memory = 3 };

struct Decision {
  Verdict verdict = Verdict::refused;
  Limit limit = Limit::none;
  // What is admitted after the change.
  Usage usage;
  bool operator==(const Decision&) const = default;
};

Bytes encode(const Decision& decision);
Decision decode_decision(std::span<const uint8_t> bytes);

// Admits `tasks`, each holding `share`, all or none, while the project's
// usage stays within `quota`'s tasks, CPU and memory. Tasks already admitted
// answer admitted again and change nothing. Throws std::invalid_argument for
// no tasks or a task named twice.
dannan::agree::Change admit(std::vector<model::TaskId> tasks, TaskShare share, const model::Quota& quota);

// Gives back `task`'s share; a task not admitted is absent.
dannan::agree::Change release(model::TaskId task);

}  // namespace louhi::admit
