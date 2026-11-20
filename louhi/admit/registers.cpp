//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/admit/registers.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "dannan/identity/crypto.h"
#include "dannan/net/codec.h"

namespace louhi::admit {

namespace {

using dannan::net::DecodeError;
using dannan::net::Reader;
using dannan::net::Writer;

// Each value opens with its kind and its format version, as Louhi's records
// do, so one register's bytes are never read as another's.
enum class Form : uint8_t { revision_counter = 1, revision_taken = 2, admitted = 3, decision = 4 };
constexpr uint8_t form_version = 1;

Writer begin(Form form) {
  Writer writer;
  writer.u8(static_cast<uint8_t>(form));
  writer.u8(form_version);
  return writer;
}

void expect(Reader& reader, Form form) {
  if (reader.u8() != static_cast<uint8_t>(form)) throw DecodeError("register value is not of the form expected");
  if (reader.u8() != form_version) throw DecodeError("register value format version is not " + std::to_string(form_version));
}

uint32_t get_count(Reader& reader, size_t maximum) {
  const uint32_t count = reader.u32();
  if (count > maximum) throw DecodeError("count is over its bound");
  return count;
}

void put_usage(Writer& writer, const Usage& usage) {
  writer.u64(usage.tasks);
  writer.u64(usage.cpu_millis);
  writer.u64(usage.memory_mib);
}

Usage get_usage(Reader& reader) {
  Usage usage;
  usage.tasks = reader.u64();
  usage.cpu_millis = reader.u64();
  usage.memory_mib = reader.u64();
  return usage;
}

// A register absent is its starting value; one that does not decode is not
// read as anything, and the change fails on every node alike.
RevisionCounter counter_of(const std::optional<Bytes>& value) { return value ? decode_revision_counter(*value) : RevisionCounter{}; }
Admitted admitted_of(const std::optional<Bytes>& value) { return value ? decode_admitted(*value) : Admitted{}; }

// A change never throws on Dannan's strand: a value that does not decode is
// left as it is, and the empty result tells the caller, whose decoder refuses
// it.
dannan::agree::Change guarded(std::function<dannan::agree::Changed(std::span<const std::optional<Bytes>>)> change) {
  return [change = std::move(change)](std::span<const std::optional<Bytes>> current) {
    try {
      return change(current);
    } catch (const DecodeError&) {
      dannan::agree::Changed unchanged;
      unchanged.values = {current[0]};
      unchanged.repeatable = true;
      return unchanged;
    }
  };
}

}  // namespace

RequestId RequestId::random() {
  const auto fresh = dannan::identity::random_bytes(16);
  RequestId id;
  std::copy_n(fresh.begin(), id.bytes.size(), id.bytes.begin());
  return id;
}

Bytes encode(const RevisionCounter& counter) {
  if (counter.given.size() > remembered_requests) throw std::invalid_argument("a revision counter remembers too many requests");
  auto writer = begin(Form::revision_counter);
  writer.u32(counter.next);
  writer.u32(static_cast<uint32_t>(counter.given.size()));
  for (const auto& given : counter.given) {
    writer.fixed(given.request.bytes);
    writer.u32(given.revision);
  }
  return writer.take();
}

RevisionCounter decode_revision_counter(std::span<const uint8_t> bytes) {
  Reader reader(bytes);
  expect(reader, Form::revision_counter);
  RevisionCounter counter;
  counter.next = reader.u32();
  if (counter.next == 0) throw DecodeError("a revision counter's next revision is 0");
  const uint32_t count = get_count(reader, remembered_requests);
  for (uint32_t i = 0; i < count; ++i) {
    RevisionCounter::Given given;
    given.request.bytes = reader.fixed<16>();
    given.revision = reader.u32();
    if (given.revision == 0 || given.revision >= counter.next) throw DecodeError("a revision given is not one the counter has passed");
    counter.given.push_back(given);
  }
  reader.finish();
  return counter;
}

Bytes encode(const RevisionTaken& taken) {
  auto writer = begin(Form::revision_taken);
  writer.u32(taken.revision.value_or(0));
  return writer.take();
}

RevisionTaken decode_revision_taken(std::span<const uint8_t> bytes) {
  Reader reader(bytes);
  expect(reader, Form::revision_taken);
  const uint32_t revision = reader.u32();
  reader.finish();
  return RevisionTaken{revision == 0 ? std::nullopt : std::optional<uint32_t>(revision)};
}

dannan::agree::Change take_revision(RequestId request) {
  return guarded([request](std::span<const std::optional<Bytes>> current) {
    auto counter = counter_of(current[0]);
    dannan::agree::Changed changed;
    changed.repeatable = true;
    for (const auto& given : counter.given) {
      if (given.request == request) {
        changed.values = {current[0]};
        changed.result = encode(RevisionTaken{given.revision});
        return changed;
      }
    }
    // Every revision there is has been given: the value stands, and the
    // request is told so.
    if (counter.next == std::numeric_limits<uint32_t>::max()) {
      changed.values = {current[0]};
      changed.result = encode(RevisionTaken{});
      return changed;
    }
    const uint32_t revision = counter.next++;
    counter.given.push_back({request, revision});
    if (counter.given.size() > remembered_requests) counter.given.pop_front();
    changed.values = {encode(counter)};
    changed.result = encode(RevisionTaken{revision});
    return changed;
  });
}

Usage Admitted::usage() const {
  Usage usage;
  for (const auto& [id, share] : tasks) {
    ++usage.tasks;
    usage.cpu_millis += share.cpu_millis;
    usage.memory_mib += share.memory_mib;
  }
  return usage;
}

Bytes encode(const Admitted& admitted) {
  if (admitted.tasks.size() > max_admitted_tasks) throw std::invalid_argument("a project's register holds too many tasks");
  auto writer = begin(Form::admitted);
  writer.u32(static_cast<uint32_t>(admitted.tasks.size()));
  for (const auto& [id, share] : admitted.tasks) {
    writer.fixed(id.bytes);
    writer.u32(share.cpu_millis);
    writer.u32(share.memory_mib);
  }
  return writer.take();
}

Admitted decode_admitted(std::span<const uint8_t> bytes) {
  Reader reader(bytes);
  expect(reader, Form::admitted);
  Admitted admitted;
  const uint32_t count = get_count(reader, max_admitted_tasks);
  std::optional<model::TaskId> previous;
  for (uint32_t i = 0; i < count; ++i) {
    model::TaskId id{reader.fixed<16>()};
    // In key order, each once: one spelling per value.
    if (previous && !(*previous < id)) throw DecodeError("admitted tasks are not in order, or one is named twice");
    previous = id;
    TaskShare share;
    share.cpu_millis = reader.u32();
    share.memory_mib = reader.u32();
    admitted.tasks.emplace(id, share);
  }
  reader.finish();
  return admitted;
}

Bytes encode(const Decision& decision) {
  auto writer = begin(Form::decision);
  writer.u8(static_cast<uint8_t>(decision.verdict));
  writer.u8(static_cast<uint8_t>(decision.limit));
  put_usage(writer, decision.usage);
  return writer.take();
}

Decision decode_decision(std::span<const uint8_t> bytes) {
  Reader reader(bytes);
  expect(reader, Form::decision);
  Decision decision;
  const uint8_t verdict = reader.u8();
  if (verdict < static_cast<uint8_t>(Verdict::admitted) || verdict > static_cast<uint8_t>(Verdict::absent))
    throw DecodeError("verdict byte is not one the format defines");
  decision.verdict = static_cast<Verdict>(verdict);
  const uint8_t limit = reader.u8();
  if (limit > static_cast<uint8_t>(Limit::memory)) throw DecodeError("limit byte is not one the format defines");
  decision.limit = static_cast<Limit>(limit);
  if ((decision.verdict == Verdict::refused) != (decision.limit != Limit::none)) throw DecodeError("a limit is named only by a refusal");
  decision.usage = get_usage(reader);
  reader.finish();
  return decision;
}

dannan::agree::Change admit(std::vector<model::TaskId> tasks, TaskShare share, const model::Quota& quota) {
  if (tasks.empty()) throw std::invalid_argument("an admission names no task");
  if (std::set<model::TaskId>(tasks.begin(), tasks.end()).size() != tasks.size()) throw std::invalid_argument("an admission names a task twice");
  const model::Quota limits = quota;
  return guarded([tasks = std::move(tasks), share, limits](std::span<const std::optional<Bytes>> current) {
    auto admitted = admitted_of(current[0]);
    dannan::agree::Changed changed;
    changed.repeatable = true;
    changed.values = {current[0]};
    Decision decision;
    // All or none, so the first task found means this request already ran.
    if (admitted.tasks.contains(tasks.front())) {
      decision.verdict = Verdict::admitted;
      decision.usage = admitted.usage();
      changed.result = encode(decision);
      return changed;
    }
    const Usage before = admitted.usage();
    const uint64_t count = tasks.size();
    Usage after = before;
    after.tasks += count;
    after.cpu_millis += count * share.cpu_millis;
    after.memory_mib += count * share.memory_mib;
    if (after.tasks > limits.max_tasks || before.tasks + count > max_admitted_tasks)
      decision.limit = Limit::tasks;
    else if (after.cpu_millis > limits.max_cpu_millis)
      decision.limit = Limit::cpu;
    else if (after.memory_mib > limits.max_memory_mib)
      decision.limit = Limit::memory;
    if (decision.limit != Limit::none) {
      decision.verdict = Verdict::refused;
      decision.usage = before;
      changed.result = encode(decision);
      return changed;
    }
    for (const auto& id : tasks) admitted.tasks.emplace(id, share);
    decision.verdict = Verdict::admitted;
    decision.usage = after;
    changed.values = {encode(admitted)};
    changed.result = encode(decision);
    return changed;
  });
}

dannan::agree::Change release(model::TaskId task) {
  return guarded([task](std::span<const std::optional<Bytes>> current) {
    auto admitted = admitted_of(current[0]);
    dannan::agree::Changed changed;
    changed.repeatable = true;
    Decision decision;
    if (admitted.tasks.erase(task) == 0) {
      changed.values = {current[0]};
      decision.verdict = Verdict::absent;
    } else {
      changed.values = {encode(admitted)};
      decision.verdict = Verdict::released;
    }
    decision.usage = admitted.usage();
    changed.result = encode(decision);
    return changed;
  });
}

}  // namespace louhi::admit
