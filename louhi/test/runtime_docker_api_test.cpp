//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The Docker Engine API as pure functions (runtime/docker/api.h; design, "The
// runtime"): a container is created with the hardened profile (no network of
// Docker's, no privilege, all capabilities dropped but the kept list, no new
// privileges, an init, private IPC, no restart policy, limits always set, no
// swap, volumes only); every parser takes the daemon's real replies and
// refuses what it does not understand; only Louhi's events are reported.

#include <gtest/gtest.h>

#include <boost/json.hpp>
#include <stdexcept>
#include <string>

#include "louhi/runtime/docker/api.h"
#include "louhi/runtime/labels.h"

using namespace louhi::runtime;
using namespace louhi::runtime::docker;
namespace json = boost::json;

namespace {

ContainerSpec spec() {
  ContainerSpec s;
  s.name = "louhi-" + std::string(32, '1') + "-app";
  s.image = "busybox:1.36";
  s.entry_point = {"/bin/sh", "-c"};
  s.command = {"echo hello"};
  s.environment = {{"MODE", "production"}, {"EMPTY", ""}};
  s.working_directory = "/srv";
  s.mounts = {{"louhi-vol", "/tmp/work", false}, {"louhi-ro", "/etc/app", true}};
  s.health_check = HealthCheck{{"/bin/true"}, std::chrono::seconds(10), std::chrono::seconds(3), 2, std::chrono::seconds(15)};
  s.cpu_millis = 750;
  s.memory_bytes = 384ull * 1024 * 1024;
  s.pids_limit = 512;
  s.read_only_root = true;
  s.labels = {{std::string(label_task), std::string(32, '1')}, {std::string(label_container), "app"}};
  return s;
}

json::object body_of(const ContainerSpec& s) { return json::parse(create_container_body(s)).as_object(); }

// A container as Docker 26.1 answers GET /containers/{id}/json, trimmed to the
// fields Louhi reads and a few it does not.
const std::string inspect_exited = R"({
  "Id": "4f66ad9a0b2e",
  "Created": "2026-11-15T17:39:30.000000000Z",
  "Name": "/louhi-11111111111111111111111111111111-app",
  "RestartCount": 0,
  "State": {"Status": "exited", "Running": false, "Paused": false, "OOMKilled": true, "Dead": false, "Pid": 0, "ExitCode": 137,
            "Error": "", "StartedAt": "2026-11-15T17:39:37.123456789Z", "FinishedAt": "2026-11-15T17:40:00.5Z",
            "Health": {"Status": "unhealthy", "FailingStreak": 3, "Log": []}},
  "Config": {"Image": "busybox:1.36", "Labels": {"louhi.task": "11111111111111111111111111111111", "louhi.container": "app", "other": "x"}},
  "HostConfig": {"NetworkMode": "none"}
})";

}  // namespace

TEST(DockerApi, CreateBodyCarriesTheContainer) {
  const json::object body = body_of(spec());
  EXPECT_EQ(body.at("Image"), "busybox:1.36");
  EXPECT_EQ(body.at("Entrypoint"), (json::array{"/bin/sh", "-c"}));
  EXPECT_EQ(body.at("Cmd"), (json::array{"echo hello"}));
  EXPECT_EQ(body.at("Env"), (json::array{"MODE=production", "EMPTY="}));
  EXPECT_EQ(body.at("WorkingDir"), "/srv");
  EXPECT_EQ(body.at("Labels").at(std::string(label_container)), "app");
  const json::object& check = body.at("Healthcheck").as_object();
  EXPECT_EQ(check.at("Test"), (json::array{"CMD", "/bin/true"}));
  EXPECT_EQ(check.at("Interval").as_int64(), 10'000'000'000);
  EXPECT_EQ(check.at("Timeout").as_int64(), 3'000'000'000);
  EXPECT_EQ(check.at("Retries").to_number<int64_t>(), 2);
  EXPECT_EQ(check.at("StartPeriod").as_int64(), 15'000'000'000);
}

TEST(DockerApi, CreateBodyIsTheHardenedProfile) {
  const json::object body = body_of(spec());
  EXPECT_EQ(body.at("NetworkDisabled"), true);
  const json::object& host = body.at("HostConfig").as_object();
  EXPECT_EQ(host.at("NetworkMode"), "none");
  EXPECT_EQ(host.at("Privileged"), false);
  EXPECT_EQ(host.at("CapDrop"), (json::array{"ALL"}));
  json::array kept;
  for (const auto& capability : kept_capabilities()) kept.emplace_back(capability);
  EXPECT_EQ(host.at("CapAdd"), kept);
  EXPECT_EQ(host.at("SecurityOpt"), (json::array{"no-new-privileges:true"}));
  EXPECT_EQ(host.at("Init"), true);
  EXPECT_EQ(host.at("IpcMode"), "private");
  EXPECT_EQ(host.at("RestartPolicy").at("Name"), "no");
  EXPECT_EQ(host.at("ReadonlyRootfs"), true);
  EXPECT_EQ(host.at("NanoCpus").as_int64(), 750'000'000);
  EXPECT_EQ(host.at("Memory").as_int64(), 384ll * 1024 * 1024);
  EXPECT_EQ(host.at("MemorySwap").as_int64(), 384ll * 1024 * 1024);
  EXPECT_EQ(host.at("PidsLimit").as_int64(), 512);
  // No host path, ever: only volume mounts, and no Binds.
  EXPECT_FALSE(host.contains("Binds"));
  const json::array& mounts = host.at("Mounts").as_array();
  ASSERT_EQ(mounts.size(), 2u);
  for (const auto& mount : mounts) EXPECT_EQ(mount.at("Type"), "volume");
  EXPECT_EQ(mounts[1].at("Source"), "louhi-ro");
  EXPECT_EQ(mounts[1].at("Target"), "/etc/app");
  EXPECT_EQ(mounts[1].at("ReadOnly"), true);
  // Nothing that would loosen it.
  EXPECT_FALSE(host.contains("PidMode"));
  EXPECT_FALSE(host.contains("UsernsMode"));
  EXPECT_FALSE(host.contains("Devices"));
}

TEST(DockerApi, KeptCapabilitiesLeaveOutTheRisky) {
  for (const char* risky : {"NET_RAW", "MKNOD", "AUDIT_WRITE", "SETFCAP", "SETPCAP", "SYS_CHROOT", "SYS_ADMIN", "NET_ADMIN"}) {
    for (const auto& kept : kept_capabilities()) EXPECT_NE(kept, risky);
  }
}

TEST(DockerApi, EmptyListsAreLeftToTheImage) {
  auto s = spec();
  s.entry_point.clear();
  s.command.clear();
  s.working_directory.clear();
  s.health_check.reset();
  const json::object body = body_of(s);
  EXPECT_FALSE(body.contains("Entrypoint"));
  EXPECT_FALSE(body.contains("Cmd"));
  EXPECT_FALSE(body.contains("WorkingDir"));
  EXPECT_FALSE(body.contains("Healthcheck"));
}

TEST(DockerApi, ALimitIsNeverLeftUnset) {
  for (int field = 0; field < 3; ++field) {
    auto s = spec();
    if (field == 0) s.cpu_millis = 0;
    if (field == 1) s.memory_bytes = 0;
    if (field == 2) s.pids_limit = 0;
    EXPECT_THROW(create_container_body(s), std::invalid_argument) << field;
  }
  auto s = spec();
  s.image.clear();
  EXPECT_THROW(create_container_body(s), std::invalid_argument);
}

TEST(DockerApi, AMemoryVolumeIsBoundedTmpfs) {
  VolumeSpec v{"louhi-vol", VolumeKind::memory, 256ull * 1024 * 1024, {{std::string(label_task), std::string(32, '1')}}};
  const json::object body = json::parse(create_volume_body(v)).as_object();
  EXPECT_EQ(body.at("Name"), "louhi-vol");
  EXPECT_EQ(body.at("Driver"), "local");
  EXPECT_EQ(body.at("DriverOpts").at("type"), "tmpfs");
  EXPECT_EQ(body.at("DriverOpts").at("o"), "size=268435456");
  EXPECT_EQ(body.at("Labels").at(std::string(label_task)), json::value(std::string(32, '1')));
  v.size_bytes = 0;
  EXPECT_THROW(create_volume_body(v), std::invalid_argument);
}

// The local driver bounds a directory by an xfs project quota with its "size"
// option; without one (the operator allowed an unbounding runtime), none.
TEST(DockerApi, ADiskVolumeIsTheLocalDriversBoundedDirectory) {
  VolumeSpec v{"louhi-vol", VolumeKind::disk, 4ull << 30, {}};
  json::object body = json::parse(create_volume_body(v)).as_object();
  EXPECT_EQ(body.at("Driver"), "local");
  EXPECT_EQ(body.at("DriverOpts"), (json::object{{"size", "4294967296"}})) << "no tmpfs, no device, no host path";
  v.size_bytes = 0;
  body = json::parse(create_volume_body(v)).as_object();
  EXPECT_FALSE(body.contains("DriverOpts"));
}

TEST(DockerApi, TheWritableLayerIsBoundedWhenAsked) {
  auto s = spec();
  EXPECT_FALSE(body_of(s).at("HostConfig").as_object().contains("StorageOpt")) << "no bound asked for";
  s.storage_bytes = 1ull << 30;
  EXPECT_EQ(body_of(s).at("HostConfig").at("StorageOpt"), (json::object{{"size", "1073741824"}}));
}

// The daemon decodes X-Registry-Auth as base64url with padding.
TEST(DockerApi, RegistryAuthIsTheCredentialInBase64Url) {
  const auto decode = [](const std::string& text) {
    static const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    uint32_t group = 0;
    int bits = 0;
    for (const char c : text) {
      if (c == '=') break;
      const auto at = alphabet.find(c);
      EXPECT_NE(at, std::string::npos) << "only the URL alphabet";
      group = (group << 6) | static_cast<uint32_t>(at);
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        out.push_back(static_cast<char>((group >> bits) & 0xff));
      }
    }
    return out;
  };
  const std::string password = registry_auth({"robot", "p+s/w?rd>>", ""}, "registry.example.net:5000");
  EXPECT_EQ(password.size() % 4, 0u) << "padded";
  EXPECT_EQ(json::parse(decode(password)),
            (json::value(json::object{{"username", "robot"}, {"password", "p+s/w?rd>>"}, {"serveraddress", "registry.example.net:5000"}})));
  const std::string token = registry_auth({"", "", "tok"}, "ghcr.io");
  EXPECT_EQ(json::parse(decode(token)), (json::value(json::object{{"identitytoken", "tok"}, {"serveraddress", "ghcr.io"}})));
}

// Docker 26.1's GET /distribution/{name}/json, trimmed.
TEST(DockerApi, ParsesTheDistributionDigest) {
  const std::string digest = "sha256:" + std::string(64, 'f');
  const std::string reply = R"({"Descriptor": {"mediaType": "application/vnd.oci.image.index.v1+json", "digest": ")" + digest +
                            R"(", "size": 9218}, "Platforms": [{"architecture": "arm64", "os": "linux"}]})";
  EXPECT_EQ(parse_distribution(reply), digest);
  for (const std::string& bad : std::initializer_list<std::string>{"", "[]", "{}", R"({"Descriptor": null})", R"({"Descriptor": {"digest": 5}})",
                                                                   R"({"Descriptor": {"digest": "sha512:abc"}})", R"({"Descriptor": {"digest": "sha256:ABC"}})",
                                                                   R"({"Descriptor": {"digest": "sha256:)" + std::string(63, 'f') + R"("}})"}) {
    EXPECT_THROW(parse_distribution(bad), MalformedReply) << bad;
  }
}

TEST(DockerApi, FiltersSelectLouhisOwn) {
  EXPECT_EQ(url_encode("a b/{\"c\"}~"), "a%20b%2F%7B%22c%22%7D~");
  EXPECT_EQ(louhi_filter(), "filters=" + url_encode(R"({"label":["louhi.task"]})"));
  EXPECT_EQ(events_filter(), "filters=" + url_encode(R"({"type":["container"],"label":["louhi.task"]})"));
}

TEST(DockerApi, ParsesInspect) {
  const ContainerInfo info = parse_inspect(inspect_exited);
  EXPECT_EQ(info.id, "4f66ad9a0b2e");
  EXPECT_EQ(info.name, "louhi-11111111111111111111111111111111-app");
  EXPECT_EQ(info.image, "busybox:1.36");
  EXPECT_EQ(info.labels.at(std::string(label_container)), "app");
  EXPECT_EQ(info.labels.at("other"), "x");
  EXPECT_EQ(info.state, ContainerState::exited);
  EXPECT_EQ(info.exit_code, 137);
  EXPECT_TRUE(info.oom_killed);
  EXPECT_EQ(info.health, Health::unhealthy);
  EXPECT_EQ(info.started_unix_ms, 1'794'764'377'123u);
  EXPECT_EQ(info.finished_unix_ms, 1'794'764'400'500u);
}

TEST(DockerApi, ARunningContainerHasNoExitCode) {
  const std::string running = R"({"Id": "abc", "Name": "/x", "State": {"Status": "running", "ExitCode": 0, "StartedAt": "2026-11-15T17:39:37Z",
                                  "FinishedAt": "0001-01-01T00:00:00Z"}, "Config": {"Labels": null}})";
  const ContainerInfo info = parse_inspect(running);
  EXPECT_EQ(info.state, ContainerState::running);
  EXPECT_FALSE(info.exit_code);
  EXPECT_EQ(info.health, Health::none);
  EXPECT_EQ(info.finished_unix_ms, 0u);
  EXPECT_TRUE(info.labels.empty());
}

TEST(DockerApi, InspectRefusesWhatItDoesNotUnderstand) {
  for (const std::string& bad : std::initializer_list<std::string>{
           "", "not json", "[]", R"({"State": {"Status": "running"}})", R"({"Id": 5, "State": {"Status": "running"}})", R"({"Id": "a"})",
           R"({"Id": "a", "State": {"Status": "sleeping"}})", R"({"Id": "a", "State": {"Status": "exited", "ExitCode": "1"}})",
           R"({"Id": "a", "State": {"Status": "exited", "ExitCode": 99999999999}})", R"({"Id": "a", "State": {"Status": "running", "OOMKilled": 1}})",
           R"({"Id": "a", "State": {"Status": "running", "StartedAt": "yesterday"}})",
           R"({"Id": "a", "State": {"Status": "running", "Health": {"Status": "poorly"}}})",
           R"({"Id": "a", "Config": {"Labels": {"k": 1}}, "State": {"Status": "running"}})"}) {
    EXPECT_THROW(parse_inspect(bad), MalformedReply) << bad;
  }
}

TEST(DockerApi, DeepNestingIsRefused) {
  std::string deep;
  for (int i = 0; i < 64; ++i) deep += "[";
  for (int i = 0; i < 64; ++i) deep += "]";
  EXPECT_THROW(parse_container_ids(deep), MalformedReply);
}

TEST(DockerApi, ParsesListsAndCreatedIds) {
  EXPECT_EQ(parse_container_ids(R"([{"Id": "a1", "Names": ["/x"]}, {"Id": "b2"}])"), (std::vector<std::string>{"a1", "b2"}));
  EXPECT_TRUE(parse_container_ids("[]").empty());
  EXPECT_THROW(parse_container_ids(R"([{"Names": []}])"), MalformedReply);
  EXPECT_EQ(parse_created_id(R"({"Id": "e90e34656806", "Warnings": []})"), "e90e34656806");
  EXPECT_THROW(parse_created_id(R"({"Id": ""})"), MalformedReply);
}

TEST(DockerApi, ParsesVolumes) {
  const auto volumes = parse_volumes(R"({"Volumes": [{"Name": "louhi-a", "Driver": "local", "Labels": {"louhi.task": "t"}}], "Warnings": null})");
  ASSERT_EQ(volumes.size(), 1u);
  EXPECT_EQ(volumes[0].name, "louhi-a");
  EXPECT_EQ(volumes[0].labels.at(std::string(label_task)), "t");
  EXPECT_TRUE(parse_volumes(R"({"Volumes": null})").empty());
}

TEST(DockerApi, ParsesTheRuntime) {
  const std::string version = R"({"Version": "26.1.5+dfsg1", "ApiVersion": "1.45", "MinAPIVersion": "1.24", "Os": "linux", "Arch": "arm64"})";
  const std::string info = R"({"NCPU": 4, "MemTotal": 8443518976, "CgroupVersion": "2", "MemoryLimit": false, "SwapLimit": false,
                               "CpuCfsQuota": true, "PidsLimit": true,
                               "SecurityOptions": ["name=apparmor", "name=seccomp,profile=builtin", "name=cgroupns"]})";
  RuntimeInfo r = parse_runtime(version, info);
  EXPECT_EQ(r.name, "docker");
  EXPECT_EQ(r.version, "26.1.5+dfsg1");
  EXPECT_EQ(r.api_version, "1.45");
  EXPECT_FALSE(r.user_namespaces);
  EXPECT_EQ(r.cgroup_version, 2);
  // As a Raspberry Pi with the memory cgroup disabled answers.
  EXPECT_FALSE(r.limits_memory);
  EXPECT_FALSE(r.limits_swap);
  EXPECT_TRUE(r.limits_cpu);
  EXPECT_TRUE(r.limits_pids);
  EXPECT_EQ(r.cpus, 4u);
  EXPECT_EQ(r.memory_bytes, 8443518976u);
  const std::string userns = R"({"SecurityOptions": ["name=seccomp,profile=builtin", "name=userns"]})";
  EXPECT_TRUE(parse_runtime(version, userns).user_namespaces);
  EXPECT_THROW(parse_runtime(R"({"Version": "1"})", info), MalformedReply);
}

// With user namespaces, root in a container is the user the daemon's root
// directory is named for; an output directory is made that user's.
TEST(DockerApi, TheRemappedRootIsReadFromTheDaemonsRootDirectory) {
  const std::string version = R"({"Version": "26.1.5", "ApiVersion": "1.45"})";
  const auto root = [&](const std::string& options, const std::string& dir) {
    const auto r = parse_runtime(version, R"({"SecurityOptions": [)" + options + R"(], "DockerRootDir": ")" + dir + R"("})");
    return std::make_pair(r.root_uid, r.root_gid);
  };
  EXPECT_EQ(root(R"("name=userns")", "/var/lib/docker/100000.100000"), std::make_pair(100000u, 100000u));
  EXPECT_EQ(root(R"("name=userns")", "/srv/docker/231072.231073"), std::make_pair(231072u, 231073u));
  EXPECT_EQ(root("", "/var/lib/docker/100000.100000"), std::make_pair(0u, 0u)) << "no user namespaces, no remapping";
  EXPECT_EQ(root(R"("name=userns")", "/var/lib/docker"), std::make_pair(0u, 0u));
  EXPECT_EQ(root(R"("name=userns")", "/var/lib/docker/1.x"), std::make_pair(0u, 0u));
  EXPECT_EQ(root(R"("name=userns")", "/var/lib/docker/99999999999.1"), std::make_pair(0u, 0u)) << "past 32 bits";
}

// A host directory is bound read-only, the output directory writable; both
// nosuid and nodev, and only at a path the driver has resolved.
TEST(DockerApi, HostAndOutputVolumesBindTheirDirectories) {
  VolumeSpec host{"louhi-src", VolumeKind::host};
  host.host_directory = "work";
  host.host_path = "web/src";
  json::object body = json::parse(create_volume_body(host, "/srv/louhi/work/web/src")).as_object();
  EXPECT_EQ(body.at("Driver"), "local");
  EXPECT_EQ(body.at("DriverOpts"), (json::object{{"type", "none"}, {"device", "/srv/louhi/work/web/src"}, {"o", "bind,ro,nosuid,nodev"}}));

  VolumeSpec output{"louhi-out", VolumeKind::output, 1ull << 30};
  body = json::parse(create_volume_body(output, "/srv/louhi/output/louhi-out")).as_object();
  EXPECT_EQ(body.at("DriverOpts"), (json::object{{"type", "none"}, {"device", "/srv/louhi/output/louhi-out"}, {"o", "bind,nosuid,nodev"}}));

  EXPECT_THROW(create_volume_body(host, "relative"), std::invalid_argument);
  EXPECT_THROW(create_volume_body(host), std::invalid_argument);
  EXPECT_THROW(create_volume_body(output), std::invalid_argument);
}

// Docker bounds the writable layer only with overlay2 on xfs.
TEST(DockerApi, StorageIsBoundedOnlyByOverlayOnXfs) {
  const std::string version = R"({"Version": "26.1.5", "ApiVersion": "1.45"})";
  const auto on = [&](const std::string& driver, const std::string& backing) {
    return parse_runtime(version,
                         R"({"Driver": ")" + driver + R"(", "DriverStatus": [["Backing Filesystem", ")" + backing + R"("], ["Supports d_type", "true"]]})")
        .limits_storage;
  };
  EXPECT_TRUE(on("overlay2", "xfs"));
  EXPECT_FALSE(on("overlay2", "extfs")) << "as the Pis answer";
  EXPECT_FALSE(on("vfs", "xfs"));
  EXPECT_FALSE(parse_runtime(version, "{}").limits_storage);
  EXPECT_THROW(parse_runtime(version, R"({"Driver": "overlay2", "DriverStatus": [["Backing Filesystem"]]})"), MalformedReply);
  EXPECT_THROW(parse_runtime(version, R"({"Driver": "overlay2", "DriverStatus": "xfs"})"), MalformedReply);
}

TEST(DockerApi, FindsTheDigestForTheName) {
  const std::string image = R"({"Id": "sha256:aaaa", "RepoTags": ["busybox:1.36"],
    "RepoDigests": ["other@sha256:)" +
                            std::string(64, 'c') + R"(", "busybox@sha256:)" + std::string(64, 'd') + R"("]})";
  EXPECT_EQ(parse_image_digest(image, "busybox:1.36"), "sha256:" + std::string(64, 'd'));
  EXPECT_EQ(parse_image_digest(image, "busybox"), "sha256:" + std::string(64, 'd'));
  EXPECT_EQ(parse_image_digest(image, "nginx:1"), "");
  // A reference with a digest is its own answer.
  EXPECT_EQ(parse_image_digest("{}", "app@sha256:" + std::string(64, 'e')), "sha256:" + std::string(64, 'e'));
  EXPECT_EQ(parse_image_digest(R"({"RepoDigests": null})", "busybox"), "");
  EXPECT_THROW(parse_image_digest(image, "Not An Image"), std::invalid_argument);
}

TEST(DockerApi, PullLinesReportErrors) {
  EXPECT_FALSE(parse_pull_line(R"({"status": "Pulling from library/busybox", "id": "1.36"})"));
  EXPECT_EQ(parse_pull_line(R"({"errorDetail": {"message": "manifest unknown"}, "error": "manifest unknown"})"), "manifest unknown");
  EXPECT_EQ(parse_pull_line(R"({"errorDetail": {"message": "denied"}})"), "denied");
  EXPECT_THROW(parse_pull_line("garbage"), MalformedReply);
}

TEST(DockerApi, EventsAreLouhisContainersOnly) {
  const std::string die = R"({"status": "die", "id": "abc", "Type": "container", "Action": "die",
    "Actor": {"ID": "abc", "Attributes": {"exitCode": "-1", "image": "busybox:1.36", "louhi.task": "t1", "louhi.container": "app", "name": "x"}},
    "scope": "local", "time": 1763228400, "timeNano": 1763228400000000000})";
  const auto event = parse_event(die);
  ASSERT_TRUE(event);
  EXPECT_EQ(event->id, "abc");
  EXPECT_EQ(event->action, "die");
  EXPECT_EQ(event->exit_code, -1);
  // Only louhi.* attributes are labels.
  EXPECT_EQ(event->labels, (Labels{{"louhi.container", "app"}, {"louhi.task", "t1"}}));

  const std::string health = R"({"Type": "container", "Action": "health_status: healthy", "Actor": {"ID": "abc", "Attributes": {"louhi.task": "t1"}}})";
  EXPECT_EQ(parse_event(health)->action, "health_status");
  EXPECT_FALSE(parse_event(health)->exit_code);

  EXPECT_FALSE(parse_event(R"({"Type": "network", "Action": "connect", "Actor": {"ID": "n", "Attributes": {"louhi.task": "t1"}}})"));
  EXPECT_FALSE(parse_event(R"({"Type": "container", "Action": "die", "Actor": {"ID": "n", "Attributes": {"name": "other"}}})"));
  EXPECT_THROW(parse_event(R"({"Type": "container", "Action": "die", "Actor": {"Attributes": {"louhi.task": "t"}}})"), MalformedReply);
  EXPECT_THROW(parse_event(std::string(max_stream_line + 1, ' ')), MalformedReply);
}

TEST(DockerApi, ErrorMessagesAreBoundedAndNeverThrow) {
  EXPECT_EQ(parse_error_message(R"({"message": "No such container: x"})"), "No such container: x");
  EXPECT_EQ(parse_error_message("not json"), "");
  EXPECT_EQ(parse_error_message(R"({"message": 5})"), "");
  EXPECT_EQ(parse_error_message(R"({"message": ")" + std::string(4096, 'x') + R"("})").size(), max_message);
}

TEST(DockerApi, Times) {
  EXPECT_EQ(parse_time(""), 0u);
  EXPECT_EQ(parse_time("0001-01-01T00:00:00Z"), 0u);
  EXPECT_EQ(parse_time("1970-01-01T00:00:00Z"), 0u);
  EXPECT_EQ(parse_time("1970-01-01T00:00:01.5Z"), 1500u);
  EXPECT_EQ(parse_time("2000-03-01T00:00:00Z"), 951'868'800'000u);
  // 20772 days and 63577 seconds after the epoch.
  EXPECT_EQ(parse_time("2026-11-15T17:39:37.123456789Z"), 1'794'764'377'123u);
  for (const char* bad : {"2026-11-15", "2026-11-15T17:39:37", "2026-11-15T17:39:37+01:00", "2026-13-15T17:39:37Z", "2026-11-15T25:39:37Z",
                          "2026-11-15T17:39:37.Z", "2026-11-15T17:39:37.1234567890Z", "2026-1x-15T17:39:37Z"}) {
    EXPECT_THROW(parse_time(bad), MalformedReply) << bad;
  }
}
