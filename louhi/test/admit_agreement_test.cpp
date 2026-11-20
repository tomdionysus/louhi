//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Louhi's registers on Dannan's real Agreement, under Dannan's simulation
// harness (five nodes; messages lost, duplicated and delayed; nodes crashing
// and restarting), with clients on every node asking again, with the same
// request, until answered (design, "Projects and quotas"):
// - every request is given one revision, and no two requests the same one:
//   the family's revisions are 1 to n with no gap while no request is
//   forgotten;
// - however the admissions race, a project never has more admitted than its
//   quota, and exactly as many as it allows when more are asked for;
// - a task released from several nodes gives its share back once, and what
//   it gave back is admitted again;
// - every answer is posted to the caller's executor, and is evidence.
// Each run is one seed: a failure names it, and the seed replays it.

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "dannan/test/fixtures/sim/agreement.h"
#include "louhi/admit/agreement.h"

namespace louhi::admit {
namespace {

using namespace std::chrono_literals;
using dannan::test::sim::Cluster;
using dannan::test::sim::NetworkFaults;
using dannan::test::sim::World;

// A five-node world that misbehaves, and a way to ask any node that is up.
class Simulation {
 public:
  explicit Simulation(uint64_t seed) : world(seed), cluster(world, 5, faults()) {}

  World world;
  Cluster cluster;

  // A node that is up, drawn from the seed; the cluster never has a majority
  // down (crash_now keeps it so), so there is always one.
  size_t any_up() {
    size_t at = world.random.below(cluster.size());
    while (!cluster.up(at)) at = (at + 1) % cluster.size();
    return at;
  }

  // Asks `call` on some node until it finishes, again with the same request
  // each time: after an answer that is not finished, and after a node that
  // crashed and so never answers. The call reports each answer, and is told
  // whether it is the one that counts: the current attempt's, the first
  // finished. An earlier attempt's answer arriving late is real, but counted
  // no more.
  using Report = std::function<bool(bool finished)>;
  using Call = std::function<void(Registers&, Report)>;
  void until_finished(Call call) {
    // The simulation owns each attempt; what it schedules holds it weakly, so
    // nothing keeps itself alive past the simulation.
    auto attempt = std::make_shared<std::function<void()>>();
    _attempts.push_back(attempt);
    std::weak_ptr<std::function<void()>> again = attempt;
    auto finished = std::make_shared<bool>(false);
    auto round = std::make_shared<uint64_t>(0);
    *attempt = [this, call, again, finished, round] {
      if (*finished) return;
      const uint64_t mine = ++*round;
      auto registers = std::make_shared<AgreementRegisters>(
          cluster.agreement(any_up()), [this](dannan::agree::Slot) { return Serving{cluster.ids(), Cluster::genesis_version}; }, Cluster::space);
      call(*registers, [this, again, finished, round, mine, registers](bool done) {
        if (*finished || *round != mine) return false;
        if (done) {
          *finished = true;
          return true;
        }
        world.after(50 + world.random.below(100), [again] {
          if (const auto next = again.lock()) (*next)();
        });
        return true;
      });
      // A node that crashes drops what it was asked: ask again past the deadline.
      world.after(2'500, [again, finished, round, mine] {
        if (*finished || *round != mine) return;
        if (const auto next = again.lock()) (*next)();
      });
    };
    (*attempt)();
    _finished.push_back(finished);
  }

  // One node at a time crashes for a while, over the first `for_ms`.
  void crash_now_and_then(uint64_t for_ms) {
    for (uint64_t at = 100; at < for_ms; at += 300) {
      world.after(at, [this] {
        const size_t node = world.random.below(cluster.size());
        if (!cluster.up(node)) return;
        for (size_t i = 0; i < cluster.size(); ++i)
          if (!cluster.up(i)) return;
        cluster.crash(node);
        world.after(150, [this, node] { cluster.restart(node); });
      });
    }
  }

  bool all_finished() const {
    for (const auto& f : _finished)
      if (!*f) return false;
    return true;
  }

  // Runs the world until every call has finished.
  bool run() {
    return world.run_until([this] { return all_finished(); }, 5'000'000);
  }

 private:
  static NetworkFaults faults() {
    NetworkFaults f;
    f.drop_one_in = 8;
    f.duplicate_one_in = 10;
    f.max_delay_ms = 30;
    return f;
  }

  std::vector<std::shared_ptr<bool>> _finished;
  std::vector<std::shared_ptr<std::function<void()>>> _attempts;
};

RequestId request_numbered(size_t n) {
  RequestId id;
  id.bytes[14] = static_cast<uint8_t>(n >> 8);
  id.bytes[15] = static_cast<uint8_t>(n);
  return id;
}

model::TaskId task_numbered(size_t n) {
  model::TaskId id;
  id.bytes[14] = static_cast<uint8_t>(n >> 8);
  id.bytes[15] = static_cast<uint8_t>(n);
  return id;
}

constexpr size_t seeds = 8;

TEST(AdmitAgreement, EveryRequestIsGivenOneRevisionAndNoTwoTheSame) {
  for (uint64_t seed = 1; seed <= seeds; ++seed) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    Simulation sim(seed);
    constexpr size_t requests = 30;
    // Every revision each request was answered with, over all its attempts.
    std::map<size_t, std::set<uint32_t>> answered;
    for (size_t n = 0; n < requests; ++n) {
      sim.world.after(sim.world.random.below(2'000), [&sim, &answered, n] {
        sim.until_finished([&sim, &answered, n](Registers& registers, Simulation::Report report) {
          take_revision(registers, "web", "api", request_numbered(n), 1'000ms, sim.world.io.get_executor(), [&answered, n, report](RevisionEvidence evidence) {
            // Every answer is recorded, a late one too: none may differ.
            if (evidence.finished) {
              ASSERT_TRUE(evidence.revision) << "a family of thirty has revisions left";
              answered[n].insert(*evidence.revision);
            } else {
              EXPECT_FALSE(evidence.why.empty());
            }
            report(evidence.finished);
          });
        });
      });
    }
    sim.crash_now_and_then(2'000);
    // The requests are scheduled, not yet made: run until all are made and done.
    ASSERT_TRUE(sim.world.run_until([&] { return answered.size() == requests && sim.all_finished(); }, 5'000'000));
    std::set<uint32_t> given;
    for (const auto& [n, revisions] : answered) {
      EXPECT_EQ(revisions.size(), 1u) << "request " << n << " was given more than one revision";
      given.insert(revisions.begin(), revisions.end());
    }
    EXPECT_EQ(given.size(), requests) << "two requests were given one revision";
    EXPECT_EQ(*given.begin(), 1u);
    EXPECT_EQ(*given.rbegin(), requests) << "a gap, though no request was forgotten";
  }
}

TEST(AdmitAgreement, AProjectNeverHasMoreAdmittedThanItsQuota) {
  for (uint64_t seed = 1; seed <= seeds; ++seed) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    Simulation sim(seed);
    model::Quota quota;
    quota.project = "web";
    quota.max_tasks = 10;
    quota.max_cpu_millis = 100'000;
    quota.max_memory_mib = 100'000;
    constexpr size_t asked = 30;
    std::map<size_t, Verdict> verdicts;
    for (size_t n = 0; n < asked; ++n) {
      sim.world.after(sim.world.random.below(2'000), [&, n] {
        sim.until_finished([&, n](Registers& registers, Simulation::Report report) {
          admit_tasks(registers, quota, {task_numbered(n)}, {100, 64}, 1'000ms, sim.world.io.get_executor(), [&, n, report](AdmissionEvidence evidence) {
            // Every answer says the usage it left: never past the quota.
            if (evidence.finished) {
              EXPECT_LE(evidence.decision.usage.tasks, quota.max_tasks);
            }
            if (evidence.finished && evidence.decision.verdict == Verdict::refused) {
              EXPECT_EQ(evidence.decision.limit, Limit::tasks);
            }
            if (report(evidence.finished) && evidence.finished) verdicts[n] = evidence.decision.verdict;
          });
        });
      });
    }
    sim.crash_now_and_then(2'000);
    ASSERT_TRUE(sim.world.run_until([&] { return verdicts.size() == asked && sim.all_finished(); }, 5'000'000));
    std::vector<size_t> admitted;
    for (const auto& [n, verdict] : verdicts)
      if (verdict == Verdict::admitted) admitted.push_back(n);
    ASSERT_EQ(admitted.size(), quota.max_tasks) << "exactly as many as the quota allows";

    // Each admitted task released from two nodes at once. Either may be the
    // one that gave it back (and a retry of that one finds it gone), so what
    // shows the shares came back once each is the next admission.
    Usage after_releases{999, 0, 0};
    for (const auto n : admitted) {
      for (int twice = 0; twice < 2; ++twice) {
        sim.until_finished([&, n](Registers& registers, Simulation::Report report) {
          release_task(registers, "web", task_numbered(n), 1'000ms, sim.world.io.get_executor(), [&, report](AdmissionEvidence evidence) {
            if (evidence.finished) {
              EXPECT_TRUE(evidence.decision.verdict == Verdict::released || evidence.decision.verdict == Verdict::absent);
            }
            if (report(evidence.finished) && evidence.finished && evidence.decision.usage.tasks < after_releases.tasks)
              after_releases = evidence.decision.usage;
          });
        });
      }
    }
    ASSERT_TRUE(sim.run());
    EXPECT_EQ(after_releases, (Usage{0, 0, 0})) << "every share given back";

    size_t admitted_again = 0;
    for (size_t n = 0; n < asked; ++n) {
      if (verdicts[n] == Verdict::admitted) continue;
      sim.until_finished([&, n](Registers& registers, Simulation::Report report) {
        admit_tasks(registers, quota, {task_numbered(n)}, {100, 64}, 1'000ms, sim.world.io.get_executor(), [&, report](AdmissionEvidence evidence) {
          if (report(evidence.finished) && evidence.finished && evidence.decision.verdict == Verdict::admitted) ++admitted_again;
        });
      });
    }
    ASSERT_TRUE(sim.run());
    EXPECT_EQ(admitted_again, quota.max_tasks) << "what was given back is admitted again, and no more";
  }
}

// A register holding bytes that are not its value: the change leaves them as
// they are (registers.cpp, guarded), and the caller is told the value could
// not be read, never given a revision or a decision.
class OneRegister final : public Registers {
 public:
  explicit OneRegister(Bytes start) : value(std::move(start)) {}
  void change(const std::string&, dannan::agree::Change change, std::chrono::milliseconds, Executor executor,
              std::function<void(dannan::agree::Outcome)> done) override {
    std::vector<std::optional<Bytes>> current{value};
    auto changed = change(current);
    if (changed.values.at(0)) value = *changed.values.at(0);
    dannan::agree::Outcome outcome;
    outcome.committed = true;
    outcome.values = {value};
    outcome.result = std::move(changed.result);
    boost::asio::post(executor, [done = std::move(done), outcome = std::move(outcome)] { done(outcome); });
  }
  Bytes value;
};

TEST(AdmitEvidence, ARegisterValueThatDoesNotDecodeIsReportedNotRead) {
  boost::asio::io_context io;
  const Bytes junk{9, 9, 9};
  OneRegister registers(junk);
  std::optional<RevisionEvidence> revision;
  take_revision(registers, "web", "frontend", RequestId::random(), 1'000ms, io.get_executor(), [&](RevisionEvidence evidence) { revision = evidence; });
  model::Quota quota;
  quota.project = "web";
  quota.max_tasks = 1;
  quota.max_cpu_millis = 1;
  quota.max_memory_mib = 1;
  std::optional<AdmissionEvidence> admission;
  admit_tasks(registers, quota, {task_numbered(1)}, {1, 1}, 1'000ms, io.get_executor(), [&](AdmissionEvidence evidence) { admission = evidence; });
  io.run();
  ASSERT_TRUE(revision);
  EXPECT_FALSE(revision->finished);
  EXPECT_FALSE(revision->revision);
  EXPECT_FALSE(revision->unknown);
  EXPECT_NE(revision->why.find("could not be read"), std::string::npos) << revision->why;
  ASSERT_TRUE(admission);
  EXPECT_FALSE(admission->finished);
  EXPECT_NE(admission->why.find("could not be read"), std::string::npos) << admission->why;
  EXPECT_EQ(registers.value, junk) << "left as it was";
}

}  // namespace
}  // namespace louhi::admit
