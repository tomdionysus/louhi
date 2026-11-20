//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The Docker driver against a real daemon (runtime/docker/docker_driver.h):
// describe, pull with its digest, a scratch volume, a container made with the
// hardened profile as the daemon records it, start, list by label, the event
// stream, exit codes, stop and remove, and a refused name. These tests start
// containers, so they run only where LOUHI_DOCKER_SOCKET names a daemon's
// socket (a node of the build cluster, by hand); elsewhere they are skipped.
// They pull busybox:1.36, and remove everything they make.
//
// The second half runs everywhere, against a fake daemon on a socket in the
// build tree: what a daemon answers that a healthy one seldom does (a reply
// not understood, a status the API does not use for the call, a refusal, an
// event stream that ends), and what the driver refuses without asking.

#include <gtest/gtest.h>

#include <algorithm>
#include <boost/asio/io_context.hpp>
#include <boost/json.hpp>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "dannan/test/fixtures/temp_dir.h"
#include "louhi/model/ids.h"
#include "louhi/model/image.h"
#include "louhi/runtime/docker/api.h"
#include "louhi/runtime/docker/docker_driver.h"
#include "louhi/runtime/docker/local_http.h"
#include "louhi/runtime/labels.h"
#include "louhi/test/fixtures/fake_daemon.h"

using namespace louhi;
using namespace louhi::runtime;
namespace asio = boost::asio;
namespace json = boost::json;

namespace {

constexpr auto wait_limit = std::chrono::minutes(5);
const std::string image = "busybox:1.36";

const char* socket_path() { return std::getenv("LOUHI_DOCKER_SOCKET"); }

class DockerDriverTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (socket_path() == nullptr) GTEST_SKIP() << "LOUHI_DOCKER_SOCKET is not set";
    _thread = std::thread([this] { _io.run(); });
    driver = std::make_unique<docker::DockerDriver>(_io.get_executor(), socket_path(), Registries({"docker.io"}, {}));
    task = model::TaskId::random();
  }
  void TearDown() override {
    if (socket_path() == nullptr) return;
    // Leave nothing behind, whatever the test did.
    auto listed = wait<std::vector<ContainerInfo>>([&](auto done) { driver->list(Deadline(30000), executor(), done); });
    EXPECT_TRUE(listed.ok());
    if (listed.ok()) {
      for (const auto& info : listed.value()) {
        if (task_of(info.labels) == task) wait<Done>([&](auto done) { driver->remove(info.id, Deadline(30000), executor(), done); });
      }
    }
    wait<Done>([&](auto done) { driver->remove_volume(volume_name(task, "scratch"), Deadline(30000), executor(), done); });
    driver.reset();
    _work.reset();
    _io.stop();
    _thread.join();
  }

  Executor executor() { return _io.get_executor(); }

  template <class T, class Start>
  Result<T> wait(Start start) {
    std::promise<Result<T>> promise;
    auto future = promise.get_future();
    start(Completion<T>([&promise](Result<T> r) { promise.set_value(std::move(r)); }));
    if (future.wait_for(wait_limit) != std::future_status::ready) {
      ADD_FAILURE() << "no completion";
      return Error{Error::Kind::timed_out, "test wait"};
    }
    return future.get();
  }

  ContainerSpec spec(const std::string& container, std::vector<std::string> command) {
    ContainerSpec s;
    s.name = container_name(task, container);
    s.image = image;
    s.command = std::move(command);
    s.environment = {{"GREETING", "hello"}};
    s.cpu_millis = 250;
    s.memory_bytes = 64ull * 1024 * 1024;
    s.pids_limit = 64;
    s.read_only_root = true;
    s.mounts = {{volume_name(task, "scratch"), "/scratch", false}};
    s.labels = {{std::string(label_task), model::to_string(task)}, {std::string(label_container), container}};
    return s;
  }

  // The daemon's own record of a container, for what the driver does not
  // report (the profile).
  json::object raw_inspect(const std::string& id) {
    docker::LocalHttp http(executor(), socket_path());
    auto got = wait<docker::Reply>(
        [&](auto done) { http.request(boost::beast::http::verb::get, "/v1.41/containers/" + id + "/json", "", Deadline(30000), executor(), done); });
    EXPECT_TRUE(got.ok());
    return json::parse(got.value().body).as_object();
  }

  void make_volume() {
    VolumeSpec v{volume_name(task, "scratch"), VolumeKind::memory, 16ull * 1024 * 1024, {{std::string(label_task), model::to_string(task)}}};
    auto made = wait<Done>([&](auto done) { driver->create_volume(v, Deadline(30000), executor(), done); });
    ASSERT_TRUE(made.ok()) << made.error().message;
  }

  std::string make_container(const ContainerSpec& s) {
    auto pulled = wait<ImageInfo>([&](auto done) { driver->pull(image, Deadline(240000), executor(), done); });
    EXPECT_TRUE(pulled.ok()) << (pulled.ok() ? "" : pulled.error().message);
    auto created = wait<std::string>([&](auto done) { driver->create(s, Deadline(30000), executor(), done); });
    EXPECT_TRUE(created.ok()) << (created.ok() ? "" : created.error().message);
    return created.ok() ? created.value() : std::string();
  }

  std::unique_ptr<Driver> driver;
  model::TaskId task;

 private:
  asio::io_context _io;
  asio::executor_work_guard<asio::io_context::executor_type> _work = asio::make_work_guard(_io);
  std::thread _thread;
};

}  // namespace

TEST_F(DockerDriverTest, DescribesTheRuntime) {
  auto got = wait<RuntimeInfo>([&](auto done) { driver->describe(Deadline(30000), executor(), done); });
  ASSERT_TRUE(got.ok()) << got.error().message;
  EXPECT_EQ(got.value().name, "docker");
  EXPECT_FALSE(got.value().version.empty());
  EXPECT_GT(got.value().cpus, 0u);
  EXPECT_GT(got.value().memory_bytes, 0u);
  // Reported, not required here: whether the node's daemon remaps users.
  std::cout << "docker " << got.value().version << " api " << got.value().api_version << " cgroup v" << got.value().cgroup_version << " user namespaces "
            << (got.value().user_namespaces ? "on" : "off") << "\n";
}

TEST_F(DockerDriverTest, PullsAndResolvesADigest) {
  auto got = wait<ImageInfo>([&](auto done) { driver->pull(image, Deadline(240000), executor(), done); });
  ASSERT_TRUE(got.ok()) << got.error().message;
  EXPECT_TRUE(model::is_digest(got.value().digest)) << got.value().digest;
}

// Design, "Images in detail": the registry's digest for a tag, asked through
// the daemon; the same one the pull records.
TEST_F(DockerDriverTest, ResolvesATagToTheDigestItPulls) {
  auto resolved = wait<std::string>([&](auto done) { driver->resolve(image, Deadline(120000), executor(), done); });
  ASSERT_TRUE(resolved.ok()) << resolved.error().message;
  EXPECT_TRUE(model::is_digest(resolved.value())) << resolved.value();
  auto pulled = wait<ImageInfo>([&](auto done) { driver->pull(image, Deadline(240000), executor(), done); });
  ASSERT_TRUE(pulled.ok()) << pulled.error().message;
  EXPECT_EQ(pulled.value().digest, resolved.value());

  const std::string pinned = "busybox@" + resolved.value();
  auto again = wait<std::string>([&](auto done) { driver->resolve(pinned, Deadline(30000), executor(), done); });
  ASSERT_TRUE(again.ok());
  EXPECT_EQ(again.value(), resolved.value());
}

TEST_F(DockerDriverTest, ARegistryNotAllowedIsRefusedBeforeAnythingIsAsked) {
  auto resolved = wait<std::string>([&](auto done) { driver->resolve("ghcr.io/louhi/none:1", Deadline(30000), executor(), done); });
  ASSERT_FALSE(resolved.ok());
  EXPECT_EQ(resolved.error().kind, Error::Kind::refused);
  EXPECT_NE(resolved.error().message.find("ghcr.io"), std::string::npos) << resolved.error().message;
  auto pulled = wait<ImageInfo>([&](auto done) { driver->pull("ghcr.io/louhi/none:1", Deadline(30000), executor(), done); });
  ASSERT_FALSE(pulled.ok());
  EXPECT_EQ(pulled.error().kind, Error::Kind::refused);
}

TEST_F(DockerDriverTest, ResolvingNothingFailsWithoutAMalformedReply) {
  auto resolved = wait<std::string>([&](auto done) { driver->resolve("louhi-no-such-image-anywhere:0", Deadline(120000), executor(), done); });
  ASSERT_FALSE(resolved.ok());
  EXPECT_NE(resolved.error().kind, Error::Kind::malformed) << resolved.error().message;
}

// A storage bound is kept where the runtime says it can be, and fails the
// create, saying why, where it cannot: never dropped silently. Docker 26.1
// answers the refusal with a 500, so its kind is the daemon's, not ours to
// assert.
TEST_F(DockerDriverTest, AStorageBoundIsKeptOrRefusedNeverDropped) {
  auto described = wait<RuntimeInfo>([&](auto done) { driver->describe(Deadline(30000), executor(), done); });
  ASSERT_TRUE(described.ok());
  make_volume();
  ContainerSpec s = spec("app", {"true"});
  s.storage_bytes = 64ull << 20;
  auto pulled = wait<ImageInfo>([&](auto done) { driver->pull(image, Deadline(240000), executor(), done); });
  ASSERT_TRUE(pulled.ok()) << pulled.error().message;
  auto created = wait<std::string>([&](auto done) { driver->create(s, Deadline(30000), executor(), done); });
  if (described.value().limits_storage) {
    ASSERT_TRUE(created.ok()) << created.error().message;
    EXPECT_EQ(raw_inspect(created.value()).at("HostConfig").at("StorageOpt").at("size"), "67108864");
  } else {
    ASSERT_FALSE(created.ok()) << "this daemon cannot bound storage, yet took a bound";
    EXPECT_NE(created.error().kind, Error::Kind::malformed) << created.error().message;
    EXPECT_NE(created.error().message.find("storage-opt"), std::string::npos) << "the daemon's reason reaches the caller: " << created.error().message;
    std::cout << "storage bound refused, as expected here: " << created.error().message << "\n";
  }
}

// A disk volume's size is kept by the local driver's quota where the runtime
// can bound storage: a write past it fails.
TEST_F(DockerDriverTest, ADiskVolumesSizeIsKeptWhereStorageIsBounded) {
  auto described = wait<RuntimeInfo>([&](auto done) { driver->describe(Deadline(30000), executor(), done); });
  ASSERT_TRUE(described.ok());
  if (!described.value().limits_storage) GTEST_SKIP() << "this daemon cannot bound storage";
  VolumeSpec v{volume_name(task, "scratch"), VolumeKind::disk, 8ull << 20, {{std::string(label_task), model::to_string(task)}}};
  auto made = wait<Done>([&](auto done) { driver->create_volume(v, Deadline(30000), executor(), done); });
  ASSERT_TRUE(made.ok()) << made.error().message;
  // Writes 16 MiB into an 8 MiB volume: dd fails, and the shell says so.
  const std::string id = make_container(spec("app", {"sh", "-c", "dd if=/dev/zero of=/scratch/fill bs=1M count=16 2>/dev/null && exit 0 || exit 7"}));
  ASSERT_FALSE(id.empty());
  std::promise<int32_t> exited;
  auto future = exited.get_future();
  const uint64_t watching = driver->watch(
      executor(),
      [&, set = std::make_shared<std::atomic<bool>>(false)](ContainerEvent event) {
        if (event.id == id && event.action == "die" && !set->exchange(true)) exited.set_value(event.exit_code.value_or(-1));
      },
      [](Error) {});
  ASSERT_TRUE(wait<Done>([&](auto done) { driver->start(id, Deadline(30000), executor(), done); }).ok());
  ASSERT_EQ(future.wait_for(std::chrono::seconds(60)), std::future_status::ready);
  driver->unwatch(watching);
  EXPECT_EQ(future.get(), 7) << "the write past the volume's size failed";
}

TEST_F(DockerDriverTest, APullOfNothingIsRefused) {
  auto got = wait<ImageInfo>([&](auto done) { driver->pull("louhi-no-such-image-anywhere:0", Deadline(120000), executor(), done); });
  ASSERT_FALSE(got.ok());
  EXPECT_NE(got.error().kind, Error::Kind::malformed) << got.error().message;
}

TEST_F(DockerDriverTest, AContainerRunsWithTheHardenedProfile) {
  make_volume();
  const std::string id = make_container(spec("app", {"sh", "-c", "echo $GREETING > /scratch/out && sleep 30"}));
  ASSERT_FALSE(id.empty());

  const json::object record = raw_inspect(id);
  const json::object& host = record.at("HostConfig").as_object();
  EXPECT_EQ(host.at("NetworkMode"), "none");
  EXPECT_EQ(host.at("Privileged"), false);
  EXPECT_EQ(host.at("ReadonlyRootfs"), true);
  EXPECT_EQ(host.at("CapDrop"), (json::array{"ALL"}));
  EXPECT_EQ(host.at("SecurityOpt"), (json::array{"no-new-privileges:true"}));
  EXPECT_EQ(host.at("NanoCpus").as_int64(), 250'000'000);
  EXPECT_EQ(host.at("PidsLimit").as_int64(), 64);
  // A runtime that cannot enforce a memory limit discards it without an
  // error: the driver must say so, which is how a node knows to refuse tasks.
  auto runtime = wait<RuntimeInfo>([&](auto done) { driver->describe(Deadline(30000), executor(), done); });
  ASSERT_TRUE(runtime.ok());
  if (runtime.value().limits_memory) {
    EXPECT_EQ(host.at("Memory").as_int64(), 64ll * 1024 * 1024);
    EXPECT_EQ(host.at("MemorySwap").as_int64(), 64ll * 1024 * 1024);
  } else {
    EXPECT_EQ(host.at("Memory").as_int64(), 0) << "the runtime says it cannot limit memory, yet recorded a limit";
    std::cout << "this node does not enforce memory limits (the kernel's memory cgroup is disabled)\n";
  }
  EXPECT_EQ(host.at("Init"), true);
  EXPECT_EQ(host.at("RestartPolicy").at("Name"), "no");

  auto started = wait<Done>([&](auto done) { driver->start(id, Deadline(30000), executor(), done); });
  ASSERT_TRUE(started.ok()) << started.error().message;
  // Starting it again succeeds.
  ASSERT_TRUE(wait<Done>([&](auto done) { driver->start(id, Deadline(30000), executor(), done); }).ok());

  auto listed = wait<std::vector<ContainerInfo>>([&](auto done) { driver->list(Deadline(30000), executor(), done); });
  ASSERT_TRUE(listed.ok()) << listed.error().message;
  bool found = false;
  for (const auto& info : listed.value()) {
    if (info.id != id) continue;
    found = true;
    EXPECT_EQ(task_of(info.labels), task);
    EXPECT_EQ(info.name, container_name(task, "app"));
    EXPECT_EQ(info.state, ContainerState::running);
    EXPECT_GT(info.started_unix_ms, 0u);
  }
  EXPECT_TRUE(found);

  auto stopped = wait<Done>([&](auto done) { driver->stop(id, std::chrono::seconds(1), Deadline(30000), executor(), done); });
  ASSERT_TRUE(stopped.ok()) << stopped.error().message;
  auto removed = wait<Done>([&](auto done) { driver->remove(id, Deadline(30000), executor(), done); });
  ASSERT_TRUE(removed.ok()) << removed.error().message;
  // Removing it again succeeds.
  ASSERT_TRUE(wait<Done>([&](auto done) { driver->remove(id, Deadline(30000), executor(), done); }).ok());
}

TEST_F(DockerDriverTest, AnExitIsSeenOnTheEventStreamAndInTheList) {
  make_volume();
  const std::string id = make_container(spec("job", {"sh", "-c", "exit 3"}));
  ASSERT_FALSE(id.empty());

  std::mutex mutex;
  std::promise<ContainerEvent> died;
  bool told = false;
  const uint64_t watch = driver->watch(
      executor(),
      [&](ContainerEvent event) {
        std::lock_guard<std::mutex> lock(mutex);
        if (event.id == id && event.action == "die" && !told) {
          told = true;
          died.set_value(event);
        }
      },
      [](Error lost) { ADD_FAILURE() << "event stream lost: " << lost.message; });

  ASSERT_TRUE(wait<Done>([&](auto done) { driver->start(id, Deadline(30000), executor(), done); }).ok());
  auto future = died.get_future();
  ASSERT_EQ(future.wait_for(std::chrono::seconds(60)), std::future_status::ready);
  const ContainerEvent event = future.get();
  EXPECT_EQ(event.exit_code, 3);
  EXPECT_EQ(event.labels.at(std::string(label_container)), "job");
  driver->unwatch(watch);

  auto listed = wait<std::vector<ContainerInfo>>([&](auto done) { driver->list(Deadline(30000), executor(), done); });
  ASSERT_TRUE(listed.ok());
  bool found = false;
  for (const auto& info : listed.value()) {
    if (info.id != id) continue;
    found = true;
    EXPECT_EQ(info.state, ContainerState::exited);
    EXPECT_EQ(info.exit_code, 3);
    EXPECT_GT(info.finished_unix_ms, 0u);
  }
  EXPECT_TRUE(found);
}

TEST_F(DockerDriverTest, ANameInUseIsRefused) {
  make_volume();
  const auto s = spec("app", {"true"});
  ASSERT_FALSE(make_container(s).empty());
  auto again = wait<std::string>([&](auto done) { driver->create(s, Deadline(30000), executor(), done); });
  ASSERT_FALSE(again.ok());
  EXPECT_EQ(again.error().kind, Error::Kind::refused) << again.error().message;
}

TEST_F(DockerDriverTest, ANameThatCouldChangeThePathIsRefusedUnsent) {
  auto got = wait<Done>([&](auto done) { driver->start("../../images/x", Deadline(30000), executor(), done); });
  ASSERT_FALSE(got.ok());
  EXPECT_EQ(got.error().kind, Error::Kind::refused);
  auto image_got = wait<ImageInfo>([&](auto done) { driver->pull("busybox?x=1", Deadline(30000), executor(), done); });
  ASSERT_FALSE(image_got.ok());
  EXPECT_EQ(image_got.error().kind, Error::Kind::refused);
}

namespace {

std::string reply(const std::string& body, unsigned status = 200) {
  return "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

class DockerDriverRepliesTest : public ::testing::Test {
 protected:
  ~DockerDriverRepliesTest() override {
    _work.reset();
    _io.stop();
    _thread.join();
  }

  Executor executor() { return _io.get_executor(); }

  template <class T, class Start>
  Result<T> wait(Start start) {
    std::promise<Result<T>> promise;
    auto future = promise.get_future();
    start(Completion<T>([&promise](Result<T> r) { promise.set_value(std::move(r)); }));
    if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
      ADD_FAILURE() << "no completion";
      return Error{Error::Kind::timed_out, "test wait"};
    }
    return future.get();
  }

  // The first loss a watch reports, and the events before it.
  struct Watched {
    std::vector<ContainerEvent> events;
    std::optional<Error> lost;
    int losses = 0;
  };
  Watched watch_until_lost(Driver& on) {
    auto watched = std::make_shared<Watched>();
    auto mutex = std::make_shared<std::mutex>();
    auto ended = std::make_shared<std::promise<void>>();
    const uint64_t id = on.watch(
        executor(),
        [watched, mutex](ContainerEvent event) {
          std::lock_guard<std::mutex> lock(*mutex);
          watched->events.push_back(std::move(event));
        },
        [watched, mutex, ended](Error lost) {
          std::lock_guard<std::mutex> lock(*mutex);
          if (watched->losses++ == 0) {
            watched->lost = std::move(lost);
            ended->set_value();
          }
        });
    EXPECT_EQ(ended->get_future().wait_for(std::chrono::seconds(10)), std::future_status::ready);
    on.unwatch(id);
    std::lock_guard<std::mutex> lock(*mutex);
    return *watched;
  }

  ContainerSpec spec() {
    ContainerSpec s;
    s.name = "louhi-c";
    s.image = "busybox:1.36";
    s.cpu_millis = 250;
    s.memory_bytes = 64ull << 20;
    s.pids_limit = 64;
    return s;
  }

  dannan::test::TempDir dir;
  std::string socket = (dir.path() / "d.sock").string();
  test::FakeDaemon daemon{socket};

 private:
  asio::io_context _io;
  asio::executor_work_guard<asio::io_context::executor_type> _work = asio::make_work_guard(_io);
  std::thread _thread{[this] { _io.run(); }};

 protected:
  docker::DockerDriver driver{_io.get_executor(), socket,
                              Registries({"docker.io", "registry.example.net"}, {{"registry.example.net", {"robot", "secret", ""}}})};
};

}  // namespace

// Design, "Images in detail": the operator's credential for a registry signs
// in to that registry, and is never sent for another.
TEST_F(DockerDriverRepliesTest, ARegistrysCredentialGoesOnlyToThatRegistry) {
  const std::string digest = "sha256:" + std::string(64, 'a');
  const std::string distribution = R"({"Descriptor": {"digest": ")" + digest + R"("}})";
  daemon.script({reply(distribution), reply(distribution)});
  auto signed_in = wait<std::string>([&](auto done) { driver.resolve("registry.example.net/app:1", Deadline(5000), executor(), done); });
  ASSERT_TRUE(signed_in.ok()) << signed_in.error().message;
  EXPECT_EQ(signed_in.value(), digest);
  auto hub = wait<std::string>([&](auto done) { driver.resolve("busybox:1.36", Deadline(5000), executor(), done); });
  ASSERT_TRUE(hub.ok()) << hub.error().message;
  const auto seen = daemon.requests();
  ASSERT_EQ(seen.size(), 2u);
  EXPECT_NE(lower(seen[0]).find("x-registry-auth: "), std::string::npos) << seen[0];
  EXPECT_EQ(lower(seen[1]).find("x-registry-auth"), std::string::npos) << seen[1];
}

TEST_F(DockerDriverRepliesTest, AReferenceThatIsNotOneIsRefusedUnsent) {
  auto got = wait<std::string>([&](auto done) { driver.resolve("busybox?x=1", Deadline(5000), executor(), done); });
  ASSERT_FALSE(got.ok());
  EXPECT_EQ(got.error().kind, Error::Kind::refused);
  EXPECT_TRUE(daemon.requests().empty());
}

// Every bound is always set (driver.h): a spec without one is refused before
// the runtime could take it without the bound.
TEST_F(DockerDriverRepliesTest, ASpecWithoutItsBoundsIsRefusedUnsent) {
  const VolumeSpec unbounded{"louhi-v", VolumeKind::memory, 0, {}};
  auto volume = wait<Done>([&](auto done) { driver.create_volume(unbounded, Deadline(5000), executor(), done); });
  ASSERT_FALSE(volume.ok());
  EXPECT_EQ(volume.error().kind, Error::Kind::refused);
  auto no_pids = spec();
  no_pids.pids_limit = 0;
  auto container = wait<std::string>([&](auto done) { driver.create(no_pids, Deadline(5000), executor(), done); });
  ASSERT_FALSE(container.ok());
  EXPECT_EQ(container.error().kind, Error::Kind::refused);
  EXPECT_TRUE(daemon.requests().empty());
}

TEST_F(DockerDriverRepliesTest, ABodyNotUnderstoodIsMalformed) {
  daemon.script({reply("not json"), reply(R"({"Version": "1"})"), reply("{}")});
  auto volumes = wait<std::vector<VolumeInfo>>([&](auto done) { driver.list_volumes(Deadline(5000), executor(), done); });
  ASSERT_FALSE(volumes.ok());
  EXPECT_EQ(volumes.error().kind, Error::Kind::malformed);
  auto described = wait<RuntimeInfo>([&](auto done) { driver.describe(Deadline(5000), executor(), done); });
  ASSERT_FALSE(described.ok());
  EXPECT_EQ(described.error().kind, Error::Kind::malformed);
  EXPECT_EQ(described.error().message.rfind("describing the runtime", 0), 0u) << described.error().message;
}

// Docker answers 201 for a container made: another success is not one this
// call has, so it is not understood rather than taken.
TEST_F(DockerDriverRepliesTest, ASuccessTheCallDoesNotHaveIsMalformed) {
  daemon.script({reply(R"({"Id": "abc"})", 200)});
  auto created = wait<std::string>([&](auto done) { driver.create(spec(), Deadline(5000), executor(), done); });
  ASSERT_FALSE(created.ok());
  EXPECT_EQ(created.error().kind, Error::Kind::malformed);
}

TEST_F(DockerDriverRepliesTest, TheDaemonsRefusalKeepsItsKindAndReason) {
  daemon.script({reply(R"({"message": "volume is in use"})", 409), reply(R"({"message": "it broke"})", 500)});
  auto removed = wait<Done>([&](auto done) { driver.remove_volume("louhi-v", Deadline(5000), executor(), done); });
  ASSERT_FALSE(removed.ok());
  EXPECT_EQ(removed.error().kind, Error::Kind::refused);
  EXPECT_NE(removed.error().message.find("volume is in use"), std::string::npos) << removed.error().message;
  auto started = wait<Done>([&](auto done) { driver.start("abc", Deadline(5000), executor(), done); });
  ASSERT_FALSE(started.ok());
  EXPECT_EQ(started.error().kind, Error::Kind::unavailable);
  EXPECT_NE(started.error().message.find("it broke"), std::string::npos) << started.error().message;
}

TEST_F(DockerDriverRepliesTest, APullWhoseProgressIsNotUnderstoodFails) {
  daemon.script({"HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\ngarbage\n"});
  auto pulled = wait<ImageInfo>([&](auto done) { driver.pull("busybox:1.36", Deadline(5000), executor(), done); });
  ASSERT_FALSE(pulled.ok());
  EXPECT_EQ(pulled.error().kind, Error::Kind::refused);
  EXPECT_NE(pulled.error().message.find("not understood"), std::string::npos) << pulled.error().message;
  EXPECT_EQ(daemon.requests().size(), 1u) << "nothing is inspected after a failed pull";
}

// Events only prompt a re-list: one not understood is skipped, and the end of
// the stream is a loss, told once.
TEST_F(DockerDriverRepliesTest, AnEventNotUnderstoodIsSkippedAndTheStreamsEndIsALoss) {
  const std::string die = R"({"Type": "container", "Action": "die", "Actor": {"ID": "abc", "Attributes": {"exitCode": "3", "louhi.task": "t1"}}})";
  const std::string body = "garbage\n" + die + "\n";
  daemon.script({"HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body});
  const auto watched = watch_until_lost(driver);
  ASSERT_EQ(watched.events.size(), 1u);
  EXPECT_EQ(watched.events[0].id, "abc");
  EXPECT_EQ(watched.events[0].exit_code, 3);
  ASSERT_TRUE(watched.lost);
  EXPECT_EQ(watched.lost->kind, Error::Kind::unavailable);
  EXPECT_EQ(watched.losses, 1);
}

TEST_F(DockerDriverRepliesTest, AnEventStreamTheDaemonRefusesIsALoss) {
  daemon.script({reply(R"({"message": "no"})", 500)});
  const auto watched = watch_until_lost(driver);
  EXPECT_TRUE(watched.events.empty());
  ASSERT_TRUE(watched.lost);
  EXPECT_EQ(watched.lost->kind, Error::Kind::unavailable);
  EXPECT_NE(watched.lost->message.find("watching events"), std::string::npos) << watched.lost->message;
}

TEST_F(DockerDriverRepliesTest, ARuntimeThatIsNotThereIsALoss) {
  docker::DockerDriver nowhere(executor(), (dir.path() / "none.sock").string(), Registries());
  const auto watched = watch_until_lost(nowhere);
  ASSERT_TRUE(watched.lost);
  EXPECT_EQ(watched.lost->kind, Error::Kind::unavailable);
}
