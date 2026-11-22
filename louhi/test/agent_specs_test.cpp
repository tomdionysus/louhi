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
// volumes have their declared size and kind; ephemeral storage is shared
// equally; mounts name the task's own volumes.

#include <gtest/gtest.h>

#include <algorithm>
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
  Assignment a = assignment();
  a.definition.volumes.push_back({"cache", model::VolumeKind::disk, 4096});
  const auto volumes = volume_specs(a, {});
  ASSERT_EQ(volumes.size(), 2u);
  EXPECT_EQ(volumes[0].name, runtime::volume_name(a.task.id, "scratch"));
  EXPECT_EQ(volumes[0].kind, runtime::VolumeKind::memory);
  EXPECT_EQ(volumes[0].size_bytes, 256ull << 20);
  EXPECT_EQ(runtime::task_of(volumes[0].labels), a.task.id);
  EXPECT_EQ(volumes[1].kind, runtime::VolumeKind::disk);
  EXPECT_EQ(volumes[1].size_bytes, 4096ull << 20);
}

// Host mounts: a host volume carries the operator's directory name and the
// path in it, unsized, and is mounted read-only whatever the mount said; an
// output volume is sized like disk and owned by the runtime's root.
TEST(Specs, HostAndOutputVolumesCarryWhatTheDriverBinds) {
  Assignment a = assignment();
  a.definition.volumes.push_back({"src", model::VolumeKind::host, 0, "work", "web/src"});
  a.definition.volumes.push_back({"out", model::VolumeKind::output, 512});
  a.definition.containers[0].mounts.push_back({"src", "/src", false});
  a.definition.containers[0].mounts.push_back({"out", "/out", false});
  SpecPolicy policy;
  policy.root_uid = 100000;
  policy.root_gid = 100001;
  const auto volumes = volume_specs(a, policy);
  ASSERT_EQ(volumes.size(), 3u);
  EXPECT_EQ(volumes[1].kind, runtime::VolumeKind::host);
  EXPECT_EQ(volumes[1].host_directory, "work");
  EXPECT_EQ(volumes[1].host_path, "web/src");
  EXPECT_EQ(volumes[1].size_bytes, 0u);
  EXPECT_EQ(volumes[2].kind, runtime::VolumeKind::output);
  EXPECT_EQ(volumes[2].size_bytes, 512ull << 20);
  EXPECT_EQ(volumes[2].owner_uid, 100000u);
  EXPECT_EQ(volumes[2].owner_gid, 100001u);
  EXPECT_EQ(runtime::task_of(volumes[2].labels), a.task.id);

  const auto specs = container_specs(a, policy);
  const auto& mounts = specs[0].mounts;
  const auto at = [&](const std::string& target) { return *std::find_if(mounts.begin(), mounts.end(), [&](const auto& m) { return m.target == target; }); };
  EXPECT_TRUE(at("/src").read_only) << "a host directory is never writable";
  EXPECT_FALSE(at("/out").read_only);

  policy.bound_storage = false;
  EXPECT_EQ(volume_specs(a, policy)[2].size_bytes, 0u) << "unbounded only where the operator allows it";
}

// Design, "Ephemeral storage in detail": the task's ephemeral storage is
// shared equally by its containers, and bounded wherever the runtime can.
TEST(Specs, EphemeralStorageIsSharedEquallyAndBounded) {
  const Assignment a = assignment();
  const auto specs = container_specs(a, {});
  ASSERT_EQ(specs.size(), 2u);
  EXPECT_EQ(specs[0].storage_bytes, 1024ull << 20) << "2048 MiB between two";
  EXPECT_EQ(specs[1].storage_bytes, 1024ull << 20);
}

TEST(Specs, WhereTheRuntimeCannotBoundStorageNothingAsksItTo) {
  Assignment a = assignment();
  a.definition.volumes.push_back({"cache", model::VolumeKind::disk, 4096});
  SpecPolicy unbounded;
  unbounded.bound_storage = false;
  for (const auto& spec : container_specs(a, unbounded)) EXPECT_EQ(spec.storage_bytes, 0u);
  const auto volumes = volume_specs(a, unbounded);
  EXPECT_EQ(volumes[0].size_bytes, 256ull << 20) << "a memory volume is bounded in memory all the same";
  EXPECT_EQ(volumes[1].size_bytes, 0u);
}

TEST(Specs, TwoTasksNeverShareANameOnANode) {
  Assignment one = assignment();
  Assignment two = assignment();
  two.task.id = test::task_id(0x99);
  EXPECT_NE(container_specs(one, {})[0].name, container_specs(two, {})[0].name);
  EXPECT_NE(volume_specs(one, {})[0].name, volume_specs(two, {})[0].name);
}
