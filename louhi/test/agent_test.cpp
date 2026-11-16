//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The node agent (agent/agent.h; design, "The node"):
// - a task placed here is launched: images pulled, volumes made, containers
//   made with their specs and started in order, and reported pulling,
//   starting, then running; a task placed elsewhere is not;
// - a task run on its own stops when an essential container exits, is
//   reported stopped with the exit, and is not launched again;
// - a service's task is relaunched after a backoff, up to a budget within a
//   window, then parked and left until unparked (discipline 2);
// - a launch that fails is reported with why and retried after a backoff;
// - a launch that broke half way is removed and made whole again;
// - a task wanted stopped is stopped and removed with its volumes;
// - what nothing wants is removed, but only once the desired state is whole;
// - an agent started over containers already running adopts them and
//   launches nothing (disciplines 1 and 3);
// - a node that cannot enforce memory limits launches nothing unless allowed;
// - runtime events only prompt a pass; a lost event stream is watched again;
// - a status is reported only when it changes, each with a higher sequence.
// Driven by a fake runtime and stepped clocks: no sleep anywhere.

#include "louhi/agent/agent.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <boost/asio/io_context.hpp>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "dannan/test/fixtures/stepped_time.h"
#include "louhi/runtime/labels.h"
#include "louhi/test/fixtures/fake_driver.h"
#include "louhi/test/fixtures/records.h"
#include "louhi/test/fixtures/timers.h"

using namespace louhi;
using namespace louhi::agent;
namespace asio = boost::asio;
using std::chrono::milliseconds;
using std::chrono::seconds;

namespace {

class FakeDesired final : public Desired {
 public:
  bool ready() const override {
    std::lock_guard<std::mutex> lock(_mutex);
    return _ready;
  }
  std::vector<Assignment> assignments() const override {
    std::lock_guard<std::mutex> lock(_mutex);
    return _assignments;
  }
  void set(std::vector<Assignment> assignments, bool ready = true) {
    std::lock_guard<std::mutex> lock(_mutex);
    _assignments = std::move(assignments);
    _ready = ready;
  }

 private:
  mutable std::mutex _mutex;
  std::vector<Assignment> _assignments;
  bool _ready = true;
};

class RecordedReports final : public Reports {
 public:
  void report(const model::TaskStatus& status) override {
    std::lock_guard<std::mutex> lock(_mutex);
    _all.push_back(status);
  }
  std::vector<model::TaskStatus> all() {
    std::lock_guard<std::mutex> lock(_mutex);
    return _all;
  }
  model::TaskStatus last() {
    std::lock_guard<std::mutex> lock(_mutex);
    return _all.back();
  }
  std::vector<model::TaskState> states() {
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<model::TaskState> out;
    for (const auto& s : _all) out.push_back(s.state);
    return out;
  }

 private:
  std::mutex _mutex;
  std::vector<model::TaskStatus> _all;
};

const model::NodeId self_node = test::node_id(0x22);

Assignment assignment(uint8_t id, std::string service = "frontend") {
  Assignment a{test::task(), test::task_definition()};
  a.task.id = test::task_id(id);
  a.task.service = std::move(service);
  a.task.node = self_node;
  return a;
}

struct AgentTest : ::testing::Test {
  asio::io_context io;
  test::FakeDriver driver;
  FakeDesired desired;
  RecordedReports reports;
  dannan::test::SteppedTime time;
  dannan::test::SteppedWall wall;
  test::Timers timers{time};
  Policy policy = [] {
    Policy p;
    p.restart_budget = 3;
    p.restart_window = std::chrono::minutes(10);
    p.first_backoff = seconds(1);
    p.max_backoff = seconds(8);
    return p;
  }();
  std::shared_ptr<Agent> agent;

  void make_agent() {
    Clock clock{&time, &wall, [this](milliseconds delay, std::function<void()> fn) { timers.after(delay, std::move(fn)); }};
    agent = std::make_shared<Agent>(driver, desired, reports, self_node, clock, io.get_executor(),
                                    dannan::status::ScopedLogger(dannan::status::null_sink(), "agent"), policy);
  }
  // Runs everything ready, until nothing is.
  void settle() {
    io.restart();
    while (io.poll() > 0) {
    }
  }
  void start() {
    make_agent();
    agent->start();
    settle();
  }
  void pass() {
    agent->poke();
    settle();
  }
  void advance(milliseconds by) {
    time.advance(by);
    wall.advance(by);
    timers.fire_due();
    settle();
  }
  std::string container(uint8_t task, const std::string& name) { return runtime::container_name(test::task_id(task), name); }
  std::string id_of(uint8_t task, const std::string& name) { return driver.named(container(task, name))->info.id; }
};

}  // namespace

TEST_F(AgentTest, LaunchesATaskPlacedHere) {
  const Assignment a = assignment(1);
  desired.set({a});
  start();

  const auto app = driver.named(container(1, "app"));
  const auto shipper = driver.named(container(1, "log-shipper"));
  ASSERT_TRUE(app && shipper);
  EXPECT_EQ(app->info.state, runtime::ContainerState::running);
  EXPECT_EQ(shipper->info.state, runtime::ContainerState::running);
  EXPECT_EQ(app->spec, container_specs(a, policy.spec)[0]);
  EXPECT_EQ(driver.volumes(), (std::set<std::string>{runtime::volume_name(a.task.id, "scratch")}));

  // Pulled each image once, made the volume, made then started in order.
  const auto calls = driver.calls();
  const auto at = [&](const std::string& call) { return std::find(calls.begin(), calls.end(), call) - calls.begin(); };
  EXPECT_LT(at("pull " + image_of(a.definition.containers[0])), at("create_volume " + runtime::volume_name(a.task.id, "scratch")));
  EXPECT_LT(at("create_volume " + runtime::volume_name(a.task.id, "scratch")), at("create " + container(1, "app")));
  EXPECT_LT(at("create " + container(1, "log-shipper")), at("start " + app->info.id));
  EXPECT_LT(at("start " + app->info.id), at("start " + shipper->info.id));

  // The start events prompted a pass that saw it running.
  EXPECT_EQ(reports.states(), (std::vector<model::TaskState>{model::TaskState::pulling, model::TaskState::starting, model::TaskState::running}));
  const auto running = reports.last();
  EXPECT_EQ(running.task, a.task.id);
  EXPECT_EQ(running.node, self_node);
  ASSERT_EQ(running.containers.size(), 2u);
  EXPECT_EQ(running.containers[0].name, "app");
  EXPECT_EQ(running.containers[0].state, model::ContainerState::running);
  EXPECT_GT(running.started_unix_ms, 0u);
}

TEST_F(AgentTest, LeavesTasksPlacedElsewhere) {
  Assignment a = assignment(1);
  a.task.node = test::node_id(0x99);
  desired.set({a});
  start();
  EXPECT_TRUE(driver.containers().empty());
  EXPECT_TRUE(reports.all().empty());
}

TEST_F(AgentTest, AStatusIsReportedOnlyWhenItChangesWithARisingSequence) {
  desired.set({assignment(1)});
  start();
  const size_t reported = reports.all().size();
  pass();
  pass();
  EXPECT_EQ(reports.all().size(), reported);
  const auto all = reports.all();
  for (size_t i = 1; i < all.size(); ++i) EXPECT_GT(all[i].sequence, all[i - 1].sequence);
}

TEST_F(AgentTest, ATaskOnItsOwnStopsWhenAnEssentialContainerExits) {
  desired.set({assignment(1, "")});
  start();
  driver.exit(id_of(1, "app"), 3);
  settle();

  const auto stopped = reports.last();
  EXPECT_EQ(stopped.state, model::TaskState::stopped);
  EXPECT_EQ(stopped.reason, "essential container app exited with 3");
  EXPECT_EQ(stopped.containers[0].exit_code, 3);
  // Its other container was stopped and both are kept for inspection.
  EXPECT_EQ(driver.named(container(1, "log-shipper"))->info.state, runtime::ContainerState::exited);
  EXPECT_EQ(driver.containers().size(), 2u);

  // And it is never launched again.
  driver.clear_calls();
  advance(seconds(60));
  pass();
  for (const auto& call : driver.calls()) EXPECT_EQ(call.rfind("create ", 0), std::string::npos) << call;
}

TEST_F(AgentTest, ANonEssentialExitIsLeftAlone) {
  desired.set({assignment(1)});
  start();
  driver.exit(id_of(1, "log-shipper"), 0);
  settle();
  EXPECT_EQ(reports.last().state, model::TaskState::running);
  EXPECT_EQ(driver.named(container(1, "app"))->info.state, runtime::ContainerState::running);
}

TEST_F(AgentTest, AServiceTaskIsRelaunchedAfterABackoff) {
  desired.set({assignment(1)});
  start();
  const std::string first = id_of(1, "app");
  driver.exit(first, 1, true);
  settle();
  EXPECT_EQ(reports.last().state, model::TaskState::pending);
  EXPECT_EQ(reports.last().reason, "restarting: essential container app exited with 1, out of memory");
  // Removed, and not made again before the backoff.
  EXPECT_FALSE(driver.named(container(1, "app")));
  advance(milliseconds(999));
  EXPECT_FALSE(driver.named(container(1, "app")));
  advance(milliseconds(1));
  ASSERT_TRUE(driver.named(container(1, "app")));
  EXPECT_NE(id_of(1, "app"), first);
  EXPECT_EQ(reports.last().state, model::TaskState::running);
  EXPECT_EQ(reports.last().containers[0].restarts, 1u);
}

TEST_F(AgentTest, PastItsBudgetAServiceTaskIsParkedUntilUnparked) {
  desired.set({assignment(1)});
  start();
  // Backoffs of 1, 2 and 4 seconds, then the budget of three is spent.
  for (const auto wait : {seconds(1), seconds(2), seconds(4)}) {
    driver.exit(id_of(1, "app"), 1);
    settle();
    advance(wait);
    ASSERT_TRUE(driver.named(container(1, "app"))) << "relaunched after " << wait.count() << "s";
  }
  driver.exit(id_of(1, "app"), 1);
  settle();
  EXPECT_EQ(reports.last().state, model::TaskState::parked);
  EXPECT_NE(reports.last().reason.find("restarted 3 times"), std::string::npos) << reports.last().reason;

  // Nothing more happens to it, however long.
  driver.clear_calls();
  advance(std::chrono::minutes(30));
  pass();
  for (const auto& call : driver.calls()) EXPECT_EQ(call.rfind("create ", 0), std::string::npos) << call;

  // The operator's word gives it a clean slate: its exited containers are
  // removed, and after the first backoff it runs again.
  agent->unpark(test::task_id(1));
  settle();
  advance(seconds(1));
  EXPECT_EQ(driver.named(container(1, "app"))->info.state, runtime::ContainerState::running);
  EXPECT_EQ(reports.last().state, model::TaskState::running);
}

TEST_F(AgentTest, AFailedLaunchIsReportedAndRetriedAfterABackoff) {
  const Assignment a = assignment(1);
  desired.set({a});
  driver.fail_next("pull", {runtime::Error::Kind::refused, "pull access denied"});
  start();
  EXPECT_EQ(reports.last().state, model::TaskState::pending);
  EXPECT_EQ(reports.last().reason, "launch failed: pull access denied");
  EXPECT_TRUE(driver.containers().empty());
  advance(seconds(1));
  EXPECT_EQ(reports.last().state, model::TaskState::running);
}

TEST_F(AgentTest, AHalfMadeTaskIsRemovedAndMadeWhole) {
  const Assignment a = assignment(1);
  desired.set({a});
  // A launch that made one container and then failed.
  driver.add(container_specs(a, policy.spec)[0], runtime::ContainerState::created);
  start();
  ASSERT_TRUE(driver.named(container(1, "app")));
  ASSERT_TRUE(driver.named(container(1, "log-shipper")));
  EXPECT_EQ(reports.last().state, model::TaskState::running);
}

TEST_F(AgentTest, MadeButNotStartedIsStarted) {
  const Assignment a = assignment(1);
  desired.set({a});
  for (const auto& spec : container_specs(a, policy.spec)) driver.add(spec, runtime::ContainerState::created);
  start();
  EXPECT_EQ(driver.named(container(1, "app"))->info.state, runtime::ContainerState::running);
  for (const auto& call : driver.calls()) EXPECT_EQ(call.rfind("create ", 0), std::string::npos) << call;
}

TEST_F(AgentTest, ATaskWantedStoppedIsRemovedWithItsVolumes) {
  Assignment a = assignment(1);
  desired.set({a});
  start();
  a.task.desired = model::DesiredState::stopped;
  a.task.stop_reason = "scaled in";
  desired.set({a});
  pass();
  EXPECT_TRUE(driver.containers().empty());
  EXPECT_TRUE(driver.volumes().empty());
  EXPECT_EQ(reports.last().state, model::TaskState::stopped);
  EXPECT_EQ(reports.last().reason, "scaled in");
}

TEST_F(AgentTest, WhatNothingWantsIsRemovedOnlyOnceTheDesiredStateIsWhole) {
  const Assignment gone = assignment(7);
  for (const auto& spec : container_specs(gone, policy.spec)) driver.add(spec, runtime::ContainerState::running);
  for (const auto& volume : volume_specs(gone, policy.spec)) driver.add_volume(volume);
  desired.set({}, false);
  start();
  EXPECT_EQ(driver.containers().size(), 2u);
  desired.set({}, true);
  pass();
  EXPECT_TRUE(driver.containers().empty());
  EXPECT_TRUE(driver.volumes().empty());
}

TEST_F(AgentTest, ARestartedAgentAdoptsWhatIsRunning) {
  const Assignment a = assignment(1);
  desired.set({a});
  for (const auto& spec : container_specs(a, policy.spec)) driver.add(spec, runtime::ContainerState::running);
  for (const auto& volume : volume_specs(a, policy.spec)) driver.add_volume(volume);
  start();
  EXPECT_EQ(reports.states(), (std::vector<model::TaskState>{model::TaskState::running}));
  for (const auto& call : driver.calls()) {
    EXPECT_EQ(call.rfind("create", 0), std::string::npos) << call;
    EXPECT_EQ(call.rfind("start", 0), std::string::npos) << call;
    EXPECT_EQ(call.rfind("stop", 0), std::string::npos) << call;
  }
}

TEST_F(AgentTest, ANodeThatCannotLimitMemoryLaunchesNothing) {
  driver.info.limits_memory = false;
  desired.set({assignment(1)});
  start();
  EXPECT_TRUE(driver.containers().empty());
  EXPECT_EQ(reports.last().state, model::TaskState::pending);
  EXPECT_EQ(reports.last().reason, "not launched: this node's runtime cannot enforce memory limits");
}

TEST_F(AgentTest, UnlessTheOperatorAllowsIt) {
  driver.info.limits_memory = false;
  policy.allow_unenforced_memory = true;
  desired.set({assignment(1)});
  start();
  EXPECT_EQ(reports.last().state, model::TaskState::running);
}

// Without user namespaces root in a container is root on the node (the maintainer,
// 2026-11-15: enforced, as memory is).
TEST_F(AgentTest, ANodeWithoutUserNamespacesLaunchesNothing) {
  driver.info.user_namespaces = false;
  desired.set({assignment(1)});
  start();
  EXPECT_TRUE(driver.containers().empty());
  EXPECT_EQ(reports.last().state, model::TaskState::pending);
  EXPECT_EQ(reports.last().reason, "not launched: this node's runtime has no user namespaces");
}

TEST_F(AgentTest, UnlessTheOperatorAllowsTasksWithoutUserNamespaces) {
  driver.info.user_namespaces = false;
  policy.allow_without_user_namespaces = true;
  desired.set({assignment(1)});
  start();
  EXPECT_EQ(reports.last().state, model::TaskState::running);
}

// Without a storage bound a task can fill the node's disk (the maintainer, 2026-11-16:
// refused unless the operator allows it, as memory is).
TEST_F(AgentTest, ANodeThatCannotBoundStorageLaunchesNothing) {
  driver.info.limits_storage = false;
  desired.set({assignment(1)});
  start();
  EXPECT_TRUE(driver.containers().empty());
  EXPECT_EQ(reports.last().state, model::TaskState::pending);
  EXPECT_EQ(reports.last().reason, "not launched: this node's runtime cannot bound storage");
}

TEST_F(AgentTest, UnlessTheOperatorAllowsUnboundedStorageWhenNothingAsksForABound) {
  driver.info.limits_storage = false;
  policy.allow_unbounded_storage = true;
  Assignment a = assignment(1);
  a.definition.volumes.push_back({"cache", model::VolumeKind::disk, 4096});
  desired.set({a});
  start();
  EXPECT_EQ(reports.last().state, model::TaskState::running);
  // The runtime would refuse a bound it cannot keep.
  for (const auto& c : driver.containers()) EXPECT_EQ(c.spec.storage_bytes, 0u) << c.info.name;
  const auto cache = driver.volume(runtime::volume_name(a.task.id, "cache"));
  ASSERT_TRUE(cache);
  EXPECT_EQ(cache->kind, runtime::VolumeKind::disk);
  EXPECT_EQ(cache->size_bytes, 0u);
}

TEST_F(AgentTest, ContainersAreLaunchedWithTheirStorageBound) {
  desired.set({assignment(1)});
  start();
  ASSERT_EQ(driver.containers().size(), 2u);
  for (const auto& c : driver.containers()) EXPECT_EQ(c.spec.storage_bytes, 1024ull << 20) << c.info.name;
}

TEST_F(AgentTest, ALostEventStreamIsWatchedAgain) {
  desired.set({assignment(1)});
  start();
  EXPECT_EQ(driver.watchers(), 1u);
  driver.lose_watchers();
  settle();
  EXPECT_EQ(driver.watchers(), 0u);
  advance(seconds(1));
  EXPECT_EQ(driver.watchers(), 1u);
  // And it still hears exits.
  driver.exit(id_of(1, "app"), 2);
  settle();
  EXPECT_EQ(reports.last().state, model::TaskState::pending);
}

TEST_F(AgentTest, StopLeavesTasksRunning) {
  desired.set({assignment(1)});
  start();
  bool stopped = false;
  agent->stop([&] { stopped = true; });
  settle();
  EXPECT_TRUE(stopped);
  EXPECT_EQ(driver.watchers(), 0u);
  EXPECT_EQ(driver.named(container(1, "app"))->info.state, runtime::ContainerState::running);
}
