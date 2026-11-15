//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// From records to the runtime's specs (agent/specs.h): names and labels tie
// every container and volume to its task; an image runs pinned to its digest
// when the revision has one; a container's declared CPU and memory are its
// own, the undeclaring ones share what is left and never get nothing; scratch
// volumes have their declared size; mounts name the task's own volumes.

#include <gtest/gtest.h>

#include <string>

#include "louhi/agent/specs.h"
#include "louhi/runtime/labels.h"
#include "louhi/test/fixtures/records.h"

using namespace louhi;
using namespace louhi::agent;

namespace {

Assignment assignment() { return {test::task(), test::task_definition()}; }

}  // namespace

TEST(Specs, ContainersCarryTheirTaskAndDefinition) {
  const Assignment a = assignment();
  const auto specs = container_specs(a, SpecPolicy{512});
  ASSERT_EQ(specs.size(), 2u);
  const auto& app = specs[0];
  EXPECT_EQ(app.name, runtime::container_name(a.task.id, "app"));
  EXPECT_EQ(app.image, "registry.example.net:5000/team/frontend:1.4.2@sha256:" + std::string(64, 'a'));
  EXPECT_EQ(app.entry_point, a.definition.containers[0].entry_point);
  EXPECT_EQ(app.command, a.definition.containers[0].command);
  EXPECT_EQ(app.environment, (std::vector<std::pair<std::string, std::string>>{{"MODE", "production"}, {"_HOME", "/srv"}}));
  EXPECT_EQ(app.working_directory, "/srv");
  EXPECT_TRUE(app.read_only_root);
  EXPECT_EQ(app.pids_limit, 512u);
  ASSERT_TRUE(app.health_check);
  EXPECT_EQ(app.health_check->interval, std::chrono::seconds(10));
  EXPECT_EQ(app.health_check->retries, 2u);
  ASSERT_EQ(app.mounts.size(), 1u);
  EXPECT_EQ(app.mounts[0], (runtime::Mount{runtime::volume_name(a.task.id, "scratch"), "/tmp/work", false}));
  EXPECT_EQ(runtime::task_of(app.labels), a.task.id);
  EXPECT_EQ(app.labels.at(std::string(runtime::label_container)), "app");
  EXPECT_EQ(app.labels.at(std::string(runtime::label_project)), "web");
  EXPECT_EQ(app.labels.at(std::string(runtime::label_family)), "frontend");
  EXPECT_EQ(app.labels.at(std::string(runtime::label_revision)), "3");
  // Without a digest, the reference as written.
  EXPECT_EQ(specs[1].image, "alpine:3.20");
}

TEST(Specs, DeclaredLimitsAreOwnAndTheRestIsShared) {
  Assignment a = assignment();
  // app declares 750 millis and 384 MiB of 1000 and 512; log-shipper none.
  auto specs = container_specs(a, SpecPolicy{});
  EXPECT_EQ(specs[0].cpu_millis, 750u);
  EXPECT_EQ(specs[0].memory_bytes, 384ull << 20);
  EXPECT_EQ(specs[1].cpu_millis, 250u);
  EXPECT_EQ(specs[1].memory_bytes, 128ull << 20);

  // Two that declare nothing split it.
  a.definition.containers[0].cpu_millis = 0;
  a.definition.containers[0].memory_mib = 0;
  specs = container_specs(a, SpecPolicy{});
  EXPECT_EQ(specs[0].cpu_millis, 500u);
  EXPECT_EQ(specs[1].memory_bytes, 256ull << 20);

  // Declared claims it all: the other still gets the least.
  a = assignment();
  a.definition.containers[0].cpu_millis = 1000;
  a.definition.containers[0].memory_mib = 512;
  specs = container_specs(a, SpecPolicy{});
  EXPECT_EQ(specs[1].cpu_millis, 1u);
  EXPECT_EQ(specs[1].memory_bytes, 4ull << 20);
}

TEST(Specs, VolumesAreTheTasksOwnAndSized) {
  const Assignment a = assignment();
  const auto volumes = volume_specs(a);
  ASSERT_EQ(volumes.size(), 1u);
  EXPECT_EQ(volumes[0].name, runtime::volume_name(a.task.id, "scratch"));
  EXPECT_EQ(volumes[0].size_bytes, 256ull << 20);
  EXPECT_EQ(runtime::task_of(volumes[0].labels), a.task.id);
}

TEST(Specs, TwoTasksNeverShareANameOnANode) {
  Assignment one = assignment();
  Assignment two = assignment();
  two.task.id = test::task_id(0x99);
  EXPECT_NE(container_specs(one, {})[0].name, container_specs(two, {})[0].name);
  EXPECT_NE(volume_specs(one)[0].name, volume_specs(two)[0].name);
}
