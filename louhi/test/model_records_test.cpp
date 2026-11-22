//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The records' rules (model/records.h; design, "Records"): every name is a
// name, every list and text is bounded, a task definition's containers fit in
// the task and at least one is essential, mounts name declared volumes, and
// state that cannot be (a stop reason on a running task, an exit code on a
// running container) is refused. Also task ids (model/ids.h): random, and
// written as thirty-two hex digits.

#include <gtest/gtest.h>

#include <set>
#include <stdexcept>
#include <string>

#include "louhi/model/ids.h"
#include "louhi/model/records.h"
#include "louhi/test/fixtures/records.h"

using namespace louhi::model;
namespace fixture = louhi::test;

TEST(Records, FixturesAreValid) {
  EXPECT_NO_THROW(fixture::project().validate());
  EXPECT_NO_THROW(fixture::quota().validate());
  EXPECT_NO_THROW(fixture::task_definition().validate());
  EXPECT_NO_THROW(fixture::service().validate());
  EXPECT_NO_THROW(fixture::task().validate());
  EXPECT_NO_THROW(fixture::task_status().validate());
  EXPECT_NO_THROW(fixture::node_status().validate());
}

TEST(Records, ProjectNeedsANameAndABoundedDescription) {
  auto p = fixture::project();
  p.name = "Web";
  EXPECT_THROW(p.validate(), std::invalid_argument);
  p = fixture::project();
  p.description = std::string(max_description + 1, 'x');
  EXPECT_THROW(p.validate(), std::invalid_argument);
}

TEST(TaskDefinitions, SizeIsBounded) {
  auto d = fixture::task_definition();
  d.cpu_millis = 0;
  EXPECT_THROW(d.validate(), std::invalid_argument);
  d = fixture::task_definition();
  d.cpu_millis = max_task_cpu_millis + 1;
  EXPECT_THROW(d.validate(), std::invalid_argument);
  d = fixture::task_definition();
  d.memory_mib = min_task_memory_mib - 1;
  EXPECT_THROW(d.validate(), std::invalid_argument);
  d = fixture::task_definition();
  d.revision = 0;
  EXPECT_THROW(d.validate(), std::invalid_argument);
}

TEST(TaskDefinitions, ContainersFitInTheTask) {
  auto d = fixture::task_definition();
  d.containers[1].cpu_millis = d.cpu_millis - d.containers[0].cpu_millis + 1;
  EXPECT_THROW(d.validate(), std::invalid_argument);
  d = fixture::task_definition();
  d.containers[1].memory_mib = d.memory_mib - d.containers[0].memory_mib + 1;
  EXPECT_THROW(d.validate(), std::invalid_argument);
  d = fixture::task_definition();
  d.containers[1].cpu_millis = d.cpu_millis - d.containers[0].cpu_millis;
  EXPECT_NO_THROW(d.validate());
}

TEST(TaskDefinitions, NeedOneToTenUniquelyNamedContainersOneEssential) {
  auto d = fixture::task_definition();
  d.containers.clear();
  EXPECT_THROW(d.validate(), std::invalid_argument);
  d = fixture::task_definition();
  d.containers[1].name = d.containers[0].name;
  EXPECT_THROW(d.validate(), std::invalid_argument);
  d = fixture::task_definition();
  d.containers[0].essential = false;
  EXPECT_THROW(d.validate(), std::invalid_argument);
  d = fixture::task_definition();
  while (d.containers.size() <= max_containers) {
    auto extra = d.containers[1];
    extra.name = "c" + std::to_string(d.containers.size());
    d.containers.push_back(extra);
  }
  EXPECT_THROW(d.validate(), std::invalid_argument);
}

TEST(TaskDefinitions, ImagesAreReferencesAndDigestsAreSha256) {
  auto d = fixture::task_definition();
  d.containers[0].image = "Not An Image";
  EXPECT_THROW(d.validate(), std::invalid_argument);
  d = fixture::task_definition();
  d.containers[0].image_digest = "sha256:short";
  EXPECT_THROW(d.validate(), std::invalid_argument);
  d = fixture::task_definition();
  d.containers[0].image_digest.clear();
  EXPECT_NO_THROW(d.validate());
}

TEST(TaskDefinitions, EnvironmentIsNamedOnceAndBounded) {
  auto d = fixture::task_definition();
  d.containers[0].environment.push_back({"MODE", "again"});
  EXPECT_THROW(d.validate(), std::invalid_argument);
  d = fixture::task_definition();
  d.containers[0].environment.push_back({"1BAD", "x"});
  EXPECT_THROW(d.validate(), std::invalid_argument);
  d = fixture::task_definition();
  d.containers[0].environment.push_back({"BIG", std::string(max_env_value + 1, 'x')});
  EXPECT_THROW(d.validate(), std::invalid_argument);
  d = fixture::task_definition();
  d.containers[0].environment.push_back({"NUL", std::string("a\0b", 3)});
  EXPECT_THROW(d.validate(), std::invalid_argument);
}

TEST(TaskDefinitions, ArgumentsAreBounded) {
  auto d = fixture::task_definition();
  d.containers[0].command.assign(max_arguments + 1, "x");
  EXPECT_THROW(d.validate(), std::invalid_argument);
  d = fixture::task_definition();
  d.containers[0].entry_point = {std::string(max_argument + 1, 'x')};
  EXPECT_THROW(d.validate(), std::invalid_argument);
}

TEST(TaskDefinitions, PortsAreNonZeroAndMappedOnce) {
  auto d = fixture::task_definition();
  d.containers[0].ports.push_back({0, Protocol::tcp});
  EXPECT_THROW(d.validate(), std::invalid_argument);
  d = fixture::task_definition();
  d.containers[0].ports.push_back({8080, Protocol::tcp});
  EXPECT_THROW(d.validate(), std::invalid_argument);
  d = fixture::task_definition();
  d.containers[0].ports.push_back({8080, Protocol::udp});
  EXPECT_NO_THROW(d.validate());
}

TEST(TaskDefinitions, MountsNameDeclaredVolumesAtDistinctPaths) {
  auto d = fixture::task_definition();
  d.containers[0].mounts.push_back({"undeclared", "/data", false});
  EXPECT_THROW(d.validate(), std::invalid_argument);
  d = fixture::task_definition();
  d.containers[0].mounts.push_back({"scratch", "/tmp/work", true});
  EXPECT_THROW(d.validate(), std::invalid_argument);
  d = fixture::task_definition();
  d.containers[0].mounts.push_back({"scratch", "/", false});
  EXPECT_THROW(d.validate(), std::invalid_argument);
  d = fixture::task_definition();
  d.containers[0].mounts.push_back({"scratch", "relative", false});
  EXPECT_THROW(d.validate(), std::invalid_argument);
}

TEST(TaskDefinitions, VolumesAreNamedOnceAndSized) {
  auto d = fixture::task_definition();
  d.volumes.push_back(d.volumes[0]);
  EXPECT_THROW(d.validate(), std::invalid_argument);
  d = fixture::task_definition();
  d.volumes[0].size_mib = 0;
  EXPECT_THROW(d.validate(), std::invalid_argument);
  d = fixture::task_definition();
  d.volumes[0].kind = static_cast<VolumeKind>(3);
  EXPECT_THROW(d.validate(), std::invalid_argument);
}

// Design, "Ephemeral storage in detail": a memory volume's pages are charged
// to the task's memory, so its memory volumes together fit in it; a disk
// volume is bounded on disk, not in memory.
TEST(TaskDefinitions, MemoryVolumesFitInTheTasksMemory) {
  auto d = fixture::task_definition();
  d.volumes.push_back({"more", VolumeKind::memory, d.memory_mib - d.volumes[0].size_mib});
  EXPECT_NO_THROW(d.validate()) << "exactly the task's memory";
  d.volumes.back().size_mib += 1;
  EXPECT_THROW(d.validate(), std::invalid_argument);
  d.volumes.back().kind = VolumeKind::disk;
  d.volumes.back().size_mib = 100'000;
  EXPECT_NO_THROW(d.validate()) << "a disk volume is not counted in memory";
}

TEST(TaskDefinitions, EphemeralStorageIsBounded) {
  auto d = fixture::task_definition();
  d.ephemeral_storage_mib = min_ephemeral_storage_mib;
  EXPECT_NO_THROW(d.validate());
  d.ephemeral_storage_mib = min_ephemeral_storage_mib - 1;
  EXPECT_THROW(d.validate(), std::invalid_argument);
  d.ephemeral_storage_mib = 0;
  EXPECT_THROW(d.validate(), std::invalid_argument) << "there is no unbounded";
  d.ephemeral_storage_mib = max_ephemeral_storage_mib;
  EXPECT_NO_THROW(d.validate());
  d.ephemeral_storage_mib = max_ephemeral_storage_mib + 1;
  EXPECT_THROW(d.validate(), std::invalid_argument);
}

TEST(TaskDefinitions, HealthChecksAreBounded) {
  auto d = fixture::task_definition();
  d.containers[0].health_check->command.clear();
  EXPECT_THROW(d.validate(), std::invalid_argument);
  d = fixture::task_definition();
  d.containers[0].health_check->timeout_s = d.containers[0].health_check->interval_s;
  EXPECT_THROW(d.validate(), std::invalid_argument);
  d = fixture::task_definition();
  d.containers[0].health_check->retries = 0;
  EXPECT_THROW(d.validate(), std::invalid_argument);
  d = fixture::task_definition();
  d.containers[0].health_check->interval_s = 4;
  EXPECT_THROW(d.validate(), std::invalid_argument);
}

TEST(Services, CountsAndPercentagesAreBounded) {
  auto s = fixture::service();
  s.desired_count = max_desired_count + 1;
  EXPECT_THROW(s.validate(), std::invalid_argument);
  s = fixture::service();
  s.deployment.minimum_healthy_percent = 101;
  EXPECT_THROW(s.validate(), std::invalid_argument);
  s = fixture::service();
  s.deployment.maximum_percent = 99;
  EXPECT_THROW(s.validate(), std::invalid_argument);
  s = fixture::service();
  s.deployment.maximum_percent = 201;
  EXPECT_THROW(s.validate(), std::invalid_argument);
  s = fixture::service();
  s.desired_count = 0;
  EXPECT_NO_THROW(s.validate());
}

TEST(Tasks, AStopReasonOnlyWhenStopped) {
  auto t = fixture::task();
  t.stop_reason = "scaled in";
  EXPECT_THROW(t.validate(), std::invalid_argument);
  t.desired = DesiredState::stopped;
  EXPECT_NO_THROW(t.validate());
}

// A task run on its own is done when its own node reports it stopped, and
// then holds nothing; a service's task, or a report from another node, or
// any other state, finishes nothing.
TEST(Tasks, ATaskRunOnItsOwnIsFinishedWhenItsNodeReportsItStopped) {
  auto t = fixture::task();
  t.service.clear();
  auto s = fixture::task_status();
  s.task = t.id;
  s.node = *t.node;
  EXPECT_FALSE(finished(t, std::nullopt));
  EXPECT_TRUE(holds_resources(t, std::nullopt));
  EXPECT_FALSE(finished(t, s)) << "running";
  s.state = TaskState::parked;
  EXPECT_FALSE(finished(t, s)) << "parked waits on an operator, and holds what it had";
  s.state = TaskState::stopped;
  EXPECT_TRUE(finished(t, s));
  EXPECT_FALSE(holds_resources(t, s));

  auto elsewhere = s;
  elsewhere.node = fixture::node_id(0x77);
  EXPECT_FALSE(finished(t, elsewhere)) << "only the node it is placed on says";
  auto other = s;
  other.task = fixture::task_id(0x99);
  EXPECT_FALSE(finished(t, other));

  auto of_service = t;
  of_service.service = "frontend";
  EXPECT_FALSE(finished(of_service, s)) << "a service's task is relaunched, never finished";
  EXPECT_TRUE(holds_resources(of_service, s));

  auto stopped = t;
  stopped.desired = DesiredState::stopped;
  stopped.stop_reason = "done";
  EXPECT_FALSE(holds_resources(stopped, std::nullopt));
}

TEST(Tasks, AnEpochOnlyWithANode) {
  auto t = fixture::task();
  t.node.reset();
  EXPECT_THROW(t.validate(), std::invalid_argument);
  t.placement_epoch = 0;
  EXPECT_NO_THROW(t.validate());
}

TEST(Tasks, ServiceIsEmptyOrAName) {
  auto t = fixture::task();
  t.service = "";
  EXPECT_NO_THROW(t.validate());
  t.service = "Bad";
  EXPECT_THROW(t.validate(), std::invalid_argument);
}

TEST(TaskStatuses, AnExitCodeOnlyOnceExited) {
  auto s = fixture::task_status();
  s.containers[0].exit_code = 0;
  EXPECT_THROW(s.validate(), std::invalid_argument);
}

TEST(TaskStatuses, ContainersNamedOnce) {
  auto s = fixture::task_status();
  s.containers[1].name = s.containers[0].name;
  EXPECT_THROW(s.validate(), std::invalid_argument);
}

TEST(NodeStatuses, ConditionsAreBounded) {
  auto s = fixture::node_status();
  s.conditions.assign(max_conditions + 1, "x");
  EXPECT_THROW(s.validate(), std::invalid_argument);
  s = fixture::node_status();
  s.conditions = {std::string(max_condition + 1, 'x')};
  EXPECT_THROW(s.validate(), std::invalid_argument);
}

TEST(TaskIds, PrintAndParseBack) {
  const auto id = fixture::task_id(0xab);
  EXPECT_EQ(to_string(id), "abababababababababababababababab");
  EXPECT_EQ(TaskId::parse(to_string(id)), id);
  EXPECT_EQ(TaskId::parse("ABABABABABABABABABABABABABABABAB"), id);
  EXPECT_FALSE(TaskId::parse("ab"));
  EXPECT_FALSE(TaskId::parse(std::string(32, 'g')));
  EXPECT_FALSE(TaskId::parse(std::string(33, 'a')));
}

TEST(TaskIds, AreRandom) {
  std::set<TaskId> seen;
  for (int i = 0; i < 64; ++i) seen.insert(TaskId::random());
  EXPECT_EQ(seen.size(), 64u);
}
