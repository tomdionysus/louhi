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

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/json.hpp>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "louhi/model/ids.h"
#include "louhi/model/image.h"
#include "louhi/runtime/docker/api.h"
#include "louhi/runtime/docker/docker_driver.h"
#include "louhi/runtime/docker/local_http.h"
#include "louhi/runtime/labels.h"

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
