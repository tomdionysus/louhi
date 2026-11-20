//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The registers' values and changes as pure functions (design, "Projects and
// quotas"; the maintainer, 2026-11-20):
// - a family's revisions start at 1 and go up by one per request; a request
//   run again is answered with the revision it was given, changing nothing;
// - a project admits tasks all or none while its tasks, CPU and memory stay
//   within its quota, naming the limit a refusal would pass; tasks admitted
//   again change nothing; a release gives a task's share back once, and a task
//   never admitted is absent;
// - every change says it is repeatable, and leaves a value that does not
//   decode as it was, with a result its caller refuses;
// - every value's codec is versioned, bounded and exact.

#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include "dannan/net/codec.h"
#include "louhi/admit/registers.h"

namespace louhi::admit {
namespace {

using dannan::agree::Changed;

Changed run(const dannan::agree::Change& change, const std::optional<Bytes>& value) {
  std::vector<std::optional<Bytes>> current{value};
  return change(current);
}

RequestId request(uint8_t n) {
  RequestId id;
  id.bytes[15] = n;
  return id;
}

model::TaskId task(uint8_t n) {
  model::TaskId id;
  id.bytes[15] = n;
  return id;
}

model::Quota quota(uint32_t tasks, uint32_t cpu, uint32_t memory) {
  model::Quota q;
  q.project = "web";
  q.max_tasks = tasks;
  q.max_cpu_millis = cpu;
  q.max_memory_mib = memory;
  return q;
}

TEST(AdmitRevisions, StartAtOneAndGoUpByOnePerRequest) {
  std::optional<Bytes> value;
  for (uint32_t expected = 1; expected <= 3; ++expected) {
    const auto changed = run(take_revision(request(static_cast<uint8_t>(expected))), value);
    EXPECT_TRUE(changed.repeatable);
    EXPECT_EQ(decode_revision_taken(changed.result).revision, expected);
    value = changed.values.at(0);
  }
  EXPECT_EQ(decode_revision_counter(*value).next, 4u);
}

TEST(AdmitRevisions, ARequestRunAgainIsGivenItsRevisionAndChangesNothing) {
  const auto first = run(take_revision(request(1)), std::nullopt);
  const auto second = run(take_revision(request(2)), first.values.at(0));
  const auto again = run(take_revision(request(1)), second.values.at(0));
  EXPECT_EQ(decode_revision_taken(again.result).revision, 1u);
  EXPECT_EQ(again.values.at(0), second.values.at(0));
}

TEST(AdmitRevisions, ARequestForgottenPastTheWindowIsGivenANewNumberNeverAnOldOne) {
  auto value = run(take_revision(request(0)), std::nullopt).values.at(0);
  for (size_t i = 0; i < remembered_requests; ++i) {
    RequestId other = request(1);
    other.bytes[0] = static_cast<uint8_t>(i);
    other.bytes[1] = static_cast<uint8_t>(i >> 8);
    value = run(take_revision(other), value).values.at(0);
  }
  EXPECT_EQ(decode_revision_counter(*value).given.size(), remembered_requests);
  const auto late = run(take_revision(request(0)), value);
  EXPECT_EQ(decode_revision_taken(late.result).revision, remembered_requests + 2) << "a gap, never a duplicate";
}

TEST(AdmitRevisions, AFamilyThatHasGivenEveryRevisionSaysSo) {
  RevisionCounter counter;
  counter.next = 0xffffffffu;
  const auto changed = run(take_revision(request(1)), encode(counter));
  EXPECT_FALSE(decode_revision_taken(changed.result).revision);
  EXPECT_EQ(changed.values.at(0), encode(counter));
}

TEST(AdmitQuota, AdmitsWithinTheQuotaAndCountsEveryTask) {
  const auto changed = run(admit({task(1), task(2)}, {250, 128}, quota(4, 1000, 1024)), std::nullopt);
  EXPECT_TRUE(changed.repeatable);
  const auto decision = decode_decision(changed.result);
  EXPECT_EQ(decision.verdict, Verdict::admitted);
  EXPECT_EQ(decision.usage, (Usage{2, 500, 256}));
  EXPECT_EQ(decode_admitted(*changed.values.at(0)).usage(), decision.usage);
}

TEST(AdmitQuota, RefusesAllOrNoneNamingTheLimitPassed) {
  const auto base = run(admit({task(1)}, {500, 512}, quota(3, 1000, 1024)), std::nullopt).values.at(0);
  struct Case {
    model::Quota quota;
    TaskShare share;
    Limit limit;
  };
  for (const auto& c : {Case{quota(2, 10'000, 10'000), {1, 1}, Limit::tasks}, Case{quota(10, 1000, 10'000), {300, 1}, Limit::cpu},
                        Case{quota(10, 10'000, 1024), {1, 300}, Limit::memory}}) {
    const auto changed = run(admit({task(2), task(3)}, c.share, c.quota), base);
    const auto decision = decode_decision(changed.result);
    EXPECT_EQ(decision.verdict, Verdict::refused);
    EXPECT_EQ(decision.limit, c.limit);
    EXPECT_EQ(decision.usage, (Usage{1, 500, 512})) << "the usage before, unchanged";
    EXPECT_EQ(changed.values.at(0), base) << "nothing admitted";
  }
}

TEST(AdmitQuota, AQuotaOfZeroAdmitsNothing) {
  const auto changed = run(admit({task(1)}, {0, 0}, quota(0, 0, 0)), std::nullopt);
  EXPECT_EQ(decode_decision(changed.result).verdict, Verdict::refused);
  EXPECT_FALSE(changed.values.at(0));
}

TEST(AdmitQuota, TasksAdmittedAgainChangeNothing) {
  const auto first = run(admit({task(1), task(2)}, {100, 100}, quota(2, 1000, 1000)), std::nullopt);
  // The quota is full now: the same tasks again are still admitted, not refused.
  const auto again = run(admit({task(1), task(2)}, {100, 100}, quota(2, 1000, 1000)), first.values.at(0));
  EXPECT_EQ(decode_decision(again.result).verdict, Verdict::admitted);
  EXPECT_EQ(again.values.at(0), first.values.at(0));
}

TEST(AdmitQuota, AReleaseGivesAShareBackOnce) {
  const auto admitted = run(admit({task(1), task(2)}, {100, 64}, quota(2, 1000, 1000)), std::nullopt).values.at(0);
  const auto released = run(release(task(1)), admitted);
  EXPECT_TRUE(released.repeatable);
  EXPECT_EQ(decode_decision(released.result).verdict, Verdict::released);
  EXPECT_EQ(decode_decision(released.result).usage, (Usage{1, 100, 64}));
  const auto twice = run(release(task(1)), released.values.at(0));
  EXPECT_EQ(decode_decision(twice.result).verdict, Verdict::absent);
  EXPECT_EQ(twice.values.at(0), released.values.at(0));
  EXPECT_EQ(decode_decision(run(release(task(9)), std::nullopt).result).verdict, Verdict::absent);
}

TEST(AdmitQuota, AnAdmissionNamesTasksEachOnce) {
  EXPECT_THROW(admit({}, {1, 1}, quota(1, 1, 1)), std::invalid_argument);
  EXPECT_THROW(admit({task(1), task(1)}, {1, 1}, quota(2, 2, 2)), std::invalid_argument);
}

TEST(AdmitRegisters, AValueThatDoesNotDecodeIsLeftAndTheCallerToldSo) {
  const Bytes junk{9, 9, 9};
  for (const auto& change : {take_revision(request(1)), admit({task(1)}, {1, 1}, quota(1, 1, 1)), release(task(1))}) {
    const auto changed = run(change, junk);
    EXPECT_EQ(changed.values.at(0), junk);
    EXPECT_TRUE(changed.result.empty());
  }
  EXPECT_THROW(decode_decision(Bytes{}), dannan::net::DecodeError);
  EXPECT_THROW(decode_revision_taken(Bytes{}), dannan::net::DecodeError);
}

TEST(AdmitRegisters, CodecsRoundTripExactlyAndRefuseWhatTheyDoNotDefine) {
  RevisionCounter counter;
  counter.next = 3;
  counter.given = {{request(1), 1}, {request(2), 2}};
  EXPECT_EQ(decode_revision_counter(encode(counter)), counter);
  Admitted admitted;
  admitted.tasks = {{task(1), {1, 2}}, {task(2), {3, 4}}};
  EXPECT_EQ(decode_admitted(encode(admitted)), admitted);
  const Decision decision{Verdict::refused, Limit::cpu, {1, 2, 3}};
  EXPECT_EQ(decode_decision(encode(decision)), decision);

  auto bytes = encode(counter);
  bytes[1] = 2;  // another format version
  EXPECT_THROW(decode_revision_counter(bytes), dannan::net::DecodeError);
  EXPECT_THROW(decode_admitted(encode(counter)), dannan::net::DecodeError) << "one register's bytes read as another's";
  auto trailing = encode(admitted);
  trailing.push_back(0);
  EXPECT_THROW(decode_admitted(trailing), dannan::net::DecodeError);
  // A revision given at or past the counter's next one is not a counter's.
  RevisionCounter ahead;
  ahead.next = 2;
  ahead.given = {{request(1), 2}};
  EXPECT_THROW(decode_revision_counter(encode(ahead)), dannan::net::DecodeError);
  // A refusal names its limit, and only a refusal does.
  EXPECT_THROW(decode_decision(encode(Decision{Verdict::admitted, Limit::cpu, {}})), dannan::net::DecodeError);
  EXPECT_THROW(decode_decision(encode(Decision{Verdict::refused, Limit::none, {}})), dannan::net::DecodeError);
  // A verdict the format does not define.
  for (const uint8_t verdict : {uint8_t{0}, uint8_t{99}}) {
    auto unknown = encode(Decision{Verdict::admitted, Limit::none, {}});
    unknown[2] = verdict;
    EXPECT_THROW(decode_decision(unknown), dannan::net::DecodeError) << int{verdict};
  }
}

// A request names itself apart from every other: fresh randomness each time.
TEST(AdmitRegisters, RequestIdsAreRandom) {
  const auto a = RequestId::random();
  const auto b = RequestId::random();
  EXPECT_FALSE(a == b);
  EXPECT_FALSE(a == RequestId{});
}

TEST(AdmitRegisters, AdmittedTasksAreDecodedOnlyInOrderEachOnce) {
  Admitted admitted;
  admitted.tasks = {{task(1), {1, 1}}, {task(2), {1, 1}}};
  auto bytes = encode(admitted);
  // Swap the two tasks' ids (each record is 16 + 8 bytes, after 2 + 4).
  std::swap(bytes[2 + 4 + 15], bytes[2 + 4 + 24 + 15]);
  EXPECT_THROW(decode_admitted(bytes), dannan::net::DecodeError);
}

}  // namespace
}  // namespace louhi::admit
