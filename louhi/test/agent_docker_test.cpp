//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The agent on a real node (step 3's gate): a task placed on the node is
// launched with Docker and reported running; when its essential container
// exits it is reported stopped with the exit code; wanted stopped, it is
// removed with its volume. Runs only where LOUHI_DOCKER_SOCKET names a
// daemon's socket, by hand on a build Pi; elsewhere skipped. The node's lack
// of a memory controller is allowed here, as the operator would allow it.

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include "dannan/contract/time_source.h"
#include "louhi/agent/agent.h"
#include "louhi/runtime/docker/docker_driver.h"
#include "louhi/runtime/labels.h"

using namespace louhi;
using namespace louhi::agent;
namespace asio = boost::asio;

namespace {

class StaticDesired final : public Desired {
 public:
  bool ready() const override { return true; }
  std::vector<Assignment> assignments() const override {
    std::lock_guard<std::mutex> lock(_mutex);
    return _assignments;
  }
  void set(std::vector<Assignment> a) {
    std::lock_guard<std::mutex> lock(_mutex);
    _assignments = std::move(a);
  }

 private:
  mutable std::mutex _mutex;
  std::vector<Assignment> _assignments;
};

// Statuses as they come, and a wait for one that satisfies a test.
class Waiting final : public Reports {
 public:
  void report(const model::TaskStatus& status) override {
    std::lock_guard<std::mutex> lock(_mutex);
    _all.push_back(status);
    _changed.notify_all();
  }
  template <class Predicate>
  std::optional<model::TaskStatus> wait_for(Predicate predicate, std::chrono::seconds limit) {
    std::unique_lock<std::mutex> lock(_mutex);
    std::optional<model::TaskStatus> found;
    _changed.wait_for(lock, limit, [&] {
      for (const auto& s : _all) {
        if (predicate(s)) {
          found = s;
          return true;
        }
      }
      return false;
    });
    return found;
  }

 private:
  std::mutex _mutex;
  std::condition_variable _changed;
  std::vector<model::TaskStatus> _all;
};

}  // namespace

TEST(AgentOnDocker, RunsAndStopsARealTask) {
  const char* socket = std::getenv("LOUHI_DOCKER_SOCKET");
  if (socket == nullptr) GTEST_SKIP() << "LOUHI_DOCKER_SOCKET is not set";

  asio::io_context io;
  auto work = asio::make_work_guard(io);
  std::thread runner([&] { io.run(); });

  runtime::docker::DockerDriver driver(io.get_executor(), socket, runtime::Registries({"docker.io"}, {}));
  StaticDesired desired;
  Waiting reports;
  const model::NodeId self = model::NodeId::random();

  Assignment a;
  a.task.id = model::TaskId::random();
  a.task.project = "gate";
  a.task.family = "job";
  a.task.revision = 1;
  a.task.node = self;
  a.task.placement_epoch = 1;
  a.task.started_by = "agent_docker_test";
  a.definition.project = "gate";
  a.definition.family = "job";
  a.definition.revision = 1;
  a.definition.cpu_millis = 250;
  a.definition.memory_mib = 64;
  a.definition.ephemeral_storage_mib = 64;
  a.definition.volumes = {{"scratch", model::VolumeKind::memory, 8}};
  model::ContainerDefinition c;
  c.name = "main";
  c.image = "busybox:1.36";
  c.command = {"sh", "-c", "echo ran > /scratch/out && sleep 3 && exit 4"};
  c.mounts = {{"scratch", "/scratch", false}};
  c.read_only_root = true;
  a.definition.containers = {c};
  a.definition.validate();
  a.task.validate();
  desired.set({a});

  Policy policy;
  policy.allow_unenforced_memory = true;
  // Not every test Pi's Docker has user namespaces; the refusal is tested
  // against the fake.
  policy.allow_without_user_namespaces = true;
  // Nor can any bound storage yet (ext4); the refusal is tested against the
  // fake, and the runtime's refusal of a bound by the driver's test.
  policy.allow_unbounded_storage = true;
  Clock clock{&dannan::contract::steady_time_source(), &dannan::contract::system_wall_clock(),
              [&io](std::chrono::milliseconds delay, std::function<void()> fn) {
                auto timer = std::make_shared<asio::steady_timer>(io, delay);
                timer->async_wait([timer, fn = std::move(fn)](const boost::system::error_code& failed) {
                  if (!failed) fn();
                });
              }};
  auto agent = std::make_shared<Agent>(
      driver, desired, reports, self, clock, io.get_executor(),
      dannan::status::ScopedLogger(dannan::status::console_sink(dannan::status::LogFormat::text, dannan::status::LogLevel::info), "agent"), policy);
  agent->start();

  const auto running =
      reports.wait_for([&](const model::TaskStatus& s) { return s.task == a.task.id && s.state == model::TaskState::running; }, std::chrono::seconds(240));
  ASSERT_TRUE(running) << "never reported running";
  EXPECT_EQ(running->node, self);
  ASSERT_EQ(running->containers.size(), 1u);
  EXPECT_EQ(running->containers[0].name, "main");

  const auto stopped =
      reports.wait_for([&](const model::TaskStatus& s) { return s.task == a.task.id && s.state == model::TaskState::stopped; }, std::chrono::seconds(60));
  ASSERT_TRUE(stopped) << "never reported stopped";
  EXPECT_EQ(stopped->reason, "essential container main exited with 4");
  EXPECT_EQ(stopped->containers[0].exit_code, 4);

  // Wanted stopped: removed, with its volume.
  a.task.desired = model::DesiredState::stopped;
  a.task.stop_reason = "gate done";
  desired.set({a});
  agent->poke();
  const auto removed =
      reports.wait_for([&](const model::TaskStatus& s) { return s.task == a.task.id && s.state == model::TaskState::stopped && s.reason == "gate done"; },
                       std::chrono::seconds(60));
  ASSERT_TRUE(removed);

  std::mutex mutex;
  std::condition_variable cv;
  bool listed = false;
  std::vector<runtime::ContainerInfo> left;
  std::vector<runtime::VolumeInfo> left_volumes;
  driver.list(runtime::Deadline(30000), io.get_executor(), [&](runtime::Result<std::vector<runtime::ContainerInfo>> got) {
    driver.list_volumes(runtime::Deadline(30000), io.get_executor(), [&, got](runtime::Result<std::vector<runtime::VolumeInfo>> volumes) {
      std::lock_guard<std::mutex> lock(mutex);
      if (got.ok()) left = got.value();
      if (volumes.ok()) left_volumes = volumes.value();
      listed = true;
      cv.notify_all();
    });
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(60), [&] { return listed; }));
  }
  for (const auto& info : left) EXPECT_NE(runtime::task_of(info.labels), a.task.id) << "container left: " << info.name;
  for (const auto& v : left_volumes) EXPECT_NE(runtime::task_of(v.labels), a.task.id) << "volume left: " << v.name;

  bool stopped_agent = false;
  agent->stop([&] {
    std::lock_guard<std::mutex> lock(mutex);
    stopped_agent = true;
    cv.notify_all();
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait_for(lock, std::chrono::seconds(30), [&] { return stopped_agent; });
  }
  agent.reset();
  work.reset();
  io.stop();
  runner.join();
}
