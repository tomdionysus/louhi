//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// What a node reports of itself (design, "The node"; records, NodeStatus):
// its capacity from the runtime, what is allocated to the tasks placed on it
// and wanted running, and whether it can run tasks at all, with every reason
// it cannot or is degraded named as a condition. A node that cannot reach its
// runtime, or whose runtime cannot enforce memory limits, is not ready unless
// the operator allows the latter. The record always validates, whatever the
// runtime said.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "louhi/model/codec.h"
#include "louhi/node/status.h"
#include "louhi/test/fixtures/records.h"
#include "louhi/version.h"

using namespace louhi;

namespace {

runtime::RuntimeInfo capable() {
  runtime::RuntimeInfo r;
  r.name = "docker";
  r.version = "26.1.5";
  r.api_version = "1.41";
  r.user_namespaces = true;
  r.cgroup_version = 2;
  r.limits_memory = r.limits_swap = r.limits_cpu = r.limits_pids = true;
  r.cpus = 4;
  r.memory_bytes = 8ull << 30;
  return r;
}

agent::Assignment placed(model::NodeId node, uint32_t cpu_millis, uint32_t memory_mib, model::DesiredState desired = model::DesiredState::running) {
  agent::Assignment a{test::task(), test::task_definition()};
  a.task.id = model::TaskId::random();
  a.task.node = node;
  a.task.desired = desired;
  a.definition.cpu_millis = cpu_millis;
  a.definition.memory_mib = memory_mib;
  return a;
}

bool has_condition(const model::NodeStatus& s, const std::string& fragment) {
  return std::any_of(s.conditions.begin(), s.conditions.end(), [&](const std::string& c) { return c.find(fragment) != std::string::npos; });
}

}  // namespace

TEST(NodeStatus, ACapableNodeIsReadyWithItsCapacity) {
  node::Facts facts;
  facts.self = test::node_id(1);
  facts.runtime = capable();
  const auto s = node::node_status(facts, {}, 7, 1000);
  EXPECT_EQ(s.node, facts.self);
  EXPECT_EQ(s.sequence, 7u);
  EXPECT_EQ(s.observed_unix_ms, 1000u);
  EXPECT_EQ(s.louhi_version, kVersion);
  EXPECT_EQ(s.runtime, "docker");
  EXPECT_EQ(s.runtime_version, "26.1.5");
  EXPECT_EQ(s.total.cpu_millis, 4000u);
  EXPECT_EQ(s.total.memory_mib, 8192u);
  EXPECT_EQ(s.allocated, model::Capacity{});
  EXPECT_EQ(s.tasks, 0u);
  EXPECT_TRUE(s.ready);
  EXPECT_TRUE(s.conditions.empty());
  EXPECT_NO_THROW(model::encode(s));
}

TEST(NodeStatus, AllocatedCountsOnlyTasksWantedRunning) {
  node::Facts facts;
  facts.self = test::node_id(1);
  facts.runtime = capable();
  const auto s = node::node_status(
      facts, {placed(facts.self, 500, 256), placed(facts.self, 250, 128), placed(facts.self, 1000, 1024, model::DesiredState::stopped)}, 1, 1);
  EXPECT_EQ(s.allocated.cpu_millis, 750u);
  EXPECT_EQ(s.allocated.memory_mib, 384u);
  EXPECT_EQ(s.tasks, 2u);
}

TEST(NodeStatus, AllocationSaturatesRatherThanWraps) {
  node::Facts facts;
  facts.self = test::node_id(1);
  facts.runtime = capable();
  std::vector<agent::Assignment> many;
  for (int i = 0; i < 20000; ++i) many.push_back(placed(facts.self, model::max_task_cpu_millis, model::max_task_memory_mib));
  const auto s = node::node_status(facts, many, 1, 1);
  EXPECT_EQ(s.allocated.cpu_millis, UINT32_MAX);
  EXPECT_EQ(s.allocated.memory_mib, UINT32_MAX);
}

TEST(NodeStatus, AnUnreachableRuntimeIsNotReadyAndSaysWhy) {
  node::Facts facts;
  facts.self = test::node_id(1);
  facts.runtime_error = "connecting to /var/run/docker.sock: no such file";
  const auto s = node::node_status(facts, {}, 1, 1);
  EXPECT_FALSE(s.ready);
  EXPECT_TRUE(has_condition(s, "runtime unreachable"));
  EXPECT_TRUE(has_condition(s, "no such file"));
  EXPECT_EQ(s.total, model::Capacity{});
  EXPECT_NO_THROW(model::encode(s));
}

TEST(NodeStatus, NoMemoryLimitsIsNotReadyUnlessTheOperatorAllows) {
  node::Facts facts;
  facts.self = test::node_id(1);
  facts.runtime = capable();
  facts.runtime->limits_memory = false;
  facts.runtime->limits_swap = false;
  auto s = node::node_status(facts, {}, 1, 1);
  EXPECT_FALSE(s.ready);
  EXPECT_TRUE(has_condition(s, "memory limits"));

  facts.allow_unenforced_memory = true;
  s = node::node_status(facts, {}, 1, 1);
  EXPECT_TRUE(s.ready);
  EXPECT_TRUE(has_condition(s, "memory limits")) << "allowed, but still reported";
}

TEST(NodeStatus, MissingLimitsAndUserNamespacesAreReported) {
  node::Facts facts;
  facts.self = test::node_id(1);
  facts.runtime = capable();
  facts.runtime->limits_cpu = false;
  facts.runtime->limits_pids = false;
  facts.runtime->user_namespaces = false;
  const auto s = node::node_status(facts, {}, 1, 1);
  EXPECT_TRUE(has_condition(s, "CPU limits"));
  EXPECT_TRUE(has_condition(s, "process limits"));
  EXPECT_TRUE(has_condition(s, "user namespaces"));
}

TEST(NodeStatus, NoUserNamespacesIsNotReadyUnlessTheOperatorAllows) {
  node::Facts facts;
  facts.self = test::node_id(1);
  facts.runtime = capable();
  facts.runtime->user_namespaces = false;
  auto s = node::node_status(facts, {}, 1, 1);
  EXPECT_FALSE(s.ready);
  EXPECT_TRUE(has_condition(s, "user namespaces"));

  facts.allow_without_user_namespaces = true;
  s = node::node_status(facts, {}, 1, 1);
  EXPECT_TRUE(s.ready);
  EXPECT_TRUE(has_condition(s, "user namespaces")) << "allowed, but still reported";
}

TEST(NodeStatus, AStoreThatIsNotWholeIsReported) {
  node::Facts facts;
  facts.self = test::node_id(1);
  facts.runtime = capable();
  facts.store_whole = false;
  const auto s = node::node_status(facts, {}, 1, 1);
  EXPECT_TRUE(s.ready);
  EXPECT_TRUE(has_condition(s, "store"));
}

TEST(NodeStatus, WhatTheRuntimeSaysIsBoundedSoTheRecordValidates) {
  node::Facts facts;
  facts.self = test::node_id(1);
  facts.runtime = capable();
  facts.runtime->name = std::string(500, 'n');
  facts.runtime->version = std::string(500, 'v') + std::string(1, '\0');
  facts.runtime->cpus = UINT32_MAX;
  facts.runtime->memory_bytes = UINT64_MAX;
  auto s = node::node_status(facts, {}, 1, 1);
  EXPECT_EQ(s.runtime.size(), model::max_version_text);
  EXPECT_LE(s.runtime_version.size(), model::max_version_text);
  EXPECT_EQ(s.total.cpu_millis, UINT32_MAX);
  EXPECT_EQ(s.total.memory_mib, UINT32_MAX);
  EXPECT_NO_THROW(model::encode(s));

  facts.runtime.reset();
  facts.runtime_error = std::string(5000, 'e');
  s = node::node_status(facts, {}, 1, 1);
  EXPECT_NO_THROW(model::encode(s));
}

TEST(NodeStatus, SameFactsSameReportApartFromSequenceAndTime) {
  node::Facts facts;
  facts.self = test::node_id(1);
  facts.runtime = capable();
  auto a = node::node_status(facts, {}, 1, 1);
  auto b = node::node_status(facts, {}, 2, 2);
  EXPECT_TRUE(node::same_report(a, b));
  facts.runtime->limits_pids = false;
  b = node::node_status(facts, {}, 3, 3);
  EXPECT_FALSE(node::same_report(a, b));
}
