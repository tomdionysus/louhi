//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Louhi's events (design, "Events", the topic table): a task's status
// retained under its project, task and the node that observed it; a
// project's events, not retained, published on a change of state and when
// the API takes a run or a stop; a node's status retained, and cleared when
// it stops. The agent's reports still reach the store first.

#include "louhi/events/events.h"

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "dannan/test/fixtures/stepped_time.h"
#include "louhi/api/api.h"
#include "louhi/store/memory_store.h"
#include "louhi/test/fixtures/fake_driver.h"
#include "louhi/test/fixtures/records.h"

using namespace louhi;
namespace json = boost::json;

namespace {

struct Sent {
  std::string topic;
  std::string payload;
  bool retain = false;
};

class FakePublisher final : public events::Publisher {
 public:
  std::vector<Sent> sent;
  void publish(std::string topic, std::string payload, bool retain) override { sent.push_back({std::move(topic), std::move(payload), retain}); }
  std::vector<Sent> on(const std::string& topic) const {
    std::vector<Sent> out;
    for (const auto& s : sent)
      if (s.topic == topic) out.push_back(s);
    return out;
  }
};

class FakeReports final : public agent::Reports {
 public:
  std::vector<model::TaskStatus> reported;
  void report(const model::TaskStatus& status) override { reported.push_back(status); }
};

class FakeDesired final : public agent::Desired {
 public:
  std::vector<agent::Assignment> wanted;
  bool ready() const override { return true; }
  std::vector<agent::Assignment> assignments() const override { return wanted; }
};

struct EventsTest : ::testing::Test {
  FakePublisher publisher;
  dannan::test::SteppedWall wall{1'800'000'000'000};
  model::NodeId self = test::node_id(0x22);
  events::Events events{publisher, self, wall};

  model::TaskStatus status(model::TaskState state, std::string reason = "") {
    auto s = test::task_status();
    s.state = state;
    s.reason = std::move(reason);
    if (state != model::TaskState::running) s.containers.clear();
    return s;
  }
};

}  // namespace

TEST(EventTopics, AProjectsTopicsShareItsPrefix) {
  const auto task = test::task_id(0x11);
  const auto node = test::node_id(0x22);
  EXPECT_EQ(events::task_status_topic("web", task, node), "louhi/projects/web/tasks/" + model::to_string(task) + "/nodes/" + dannan::identity::to_string(node));
  EXPECT_EQ(events::events_topic("web"), "louhi/projects/web/events");
  EXPECT_EQ(events::node_topic(node), "louhi/nodes/" + dannan::identity::to_string(node));
}

TEST_F(EventsTest, AStatusIsRetainedUnderItsProjectTaskAndNode) {
  const auto s = status(model::TaskState::running);
  events.task_status("web", s);
  const auto retained = publisher.on(events::task_status_topic("web", s.task, s.node));
  ASSERT_EQ(retained.size(), 1u);
  EXPECT_TRUE(retained[0].retain);
  EXPECT_EQ(json::parse(retained[0].payload).at("state"), "running");
}

TEST_F(EventsTest, AnEventIsPublishedOnAChangeOfStateNotOnEveryReport) {
  events.task_status("web", status(model::TaskState::pending));
  EXPECT_TRUE(publisher.on("louhi/projects/web/events").empty()) << "pending is not an event";
  events.task_status("web", status(model::TaskState::running));
  events.task_status("web", status(model::TaskState::running));
  events.task_status("web", status(model::TaskState::stopped, "exited 0"));
  events.task_status("web", status(model::TaskState::parked, "restarts spent"));
  const auto sent = publisher.on("louhi/projects/web/events");
  ASSERT_EQ(sent.size(), 3u);
  std::vector<std::string> kinds;
  for (const auto& e : sent) {
    EXPECT_FALSE(e.retain);
    kinds.emplace_back(json::parse(e.payload).at("event").as_string());
  }
  EXPECT_EQ(kinds, (std::vector<std::string>{"task.started", "task.stopped", "task.parked"}));
  const auto stopped = json::parse(sent[1].payload);
  EXPECT_EQ(stopped.at("reason"), "exited 0");
  EXPECT_EQ(stopped.at("task"), json::value(model::to_string(test::task_id(0x11))));
  EXPECT_EQ(stopped.at("node"), json::value(dannan::identity::to_string(self)));
  EXPECT_EQ(stopped.at("at"), 1'800'000'000'000ull);
}

TEST_F(EventsTest, ANodesStatusIsRetainedAndClearedWhenItStops) {
  events.node_status(test::node_status());
  events.node_gone();
  const auto sent = publisher.on(events::node_topic(self));
  ASSERT_EQ(sent.size(), 2u);
  EXPECT_TRUE(sent[0].retain);
  EXPECT_EQ(json::parse(sent[0].payload).at("ready"), true);
  EXPECT_TRUE(sent[1].retain);
  EXPECT_EQ(sent[1].payload, "") << "an empty retained message clears it";
}

TEST_F(EventsTest, ReportsReachTheStoreAndArePublishedUnderTheTasksProject) {
  FakeReports inner;
  FakeDesired desired;
  desired.wanted.push_back({test::task(), test::task_definition()});
  events::EventReports reports(inner, desired, events);
  reports.report(status(model::TaskState::running));
  ASSERT_EQ(inner.reported.size(), 1u);
  EXPECT_EQ(publisher.on(events::task_status_topic("web", test::task_id(0x11), test::node_id(0x22))).size(), 1u);

  auto other = status(model::TaskState::running);
  other.task = test::task_id(0x33);
  reports.report(other);
  EXPECT_EQ(inner.reported.size(), 2u) << "a task with no assignment still reaches the store";
  EXPECT_EQ(publisher.sent.size(), 2u) << "and is not published: it names no project";
}

namespace {

struct ApiEventsTest : EventsTest {
  boost::asio::io_context io;
  store::MemoryStore store;
  test::FakeDriver images;
  api::Api api{store, wall, self, images, io.get_executor(), dannan::status::ScopedLogger(dannan::status::null_sink(), "test"), store::Deadline(1000), &events};
  api::Grant admin{"root", std::vector<std::string>{"louhi:admin"}, 0};

  ApiEventsTest() { images.digests = {{"alpine", "sha256:" + std::string(64, 'b')}}; }

  api::Response call(std::string method, std::string path, std::string body = "") {
    std::optional<api::Response> got;
    api.handle(admin, {std::move(method), std::move(path), {}, std::move(body)}, [&](api::Response r) { got = std::move(r); });
    io.restart();
    while (io.poll() > 0) {
    }
    return got.value_or(api::Response{0, {}});
  }

  std::vector<std::string> kinds() {
    std::vector<std::string> out;
    for (const auto& e : publisher.on("louhi/projects/web/events")) out.emplace_back(json::parse(e.payload).at("event").as_string());
    return out;
  }
};

}  // namespace

TEST_F(ApiEventsTest, TheApiPublishesARunAndAStopOnceTheirWritesLand) {
  call("PUT", "/v1/projects/web", "{}");
  call("PUT", "/v1/projects/web/quota", R"({"max_tasks":5,"max_cpu_millis":5000,"max_memory_mib":5000})");
  call("POST", "/v1/projects/web/task-definitions", R"({"family":"f","cpu_millis":1,"memory_mib":4,"containers":[{"name":"a","image":"alpine"}]})");
  const auto run = call("POST", "/v1/projects/web/tasks", R"({"family":"f","count":2})");
  ASSERT_EQ(run.status, 200u);
  EXPECT_EQ(kinds(), (std::vector<std::string>{"task.run", "task.run"}));
  const std::string id(run.body.at("tasks").as_array()[0].at("id").as_string());
  call("POST", "/v1/projects/web/tasks/" + id + "/stop", R"({"reason":"enough"})");
  ASSERT_EQ(kinds().size(), 3u);
  const auto stop = json::parse(publisher.on("louhi/projects/web/events").back().payload);
  EXPECT_EQ(stop.at("event"), "task.stop_requested");
  EXPECT_EQ(stop.at("reason"), "enough");
  EXPECT_EQ(stop.at("task"), json::value(id));
}

TEST_F(ApiEventsTest, ARefusedRunPublishesNothing) {
  call("PUT", "/v1/projects/web", "{}");
  call("POST", "/v1/projects/web/task-definitions", R"({"family":"f","cpu_millis":1,"memory_mib":4,"containers":[{"name":"a","image":"alpine"}]})");
  EXPECT_EQ(call("POST", "/v1/projects/web/tasks", R"({"family":"f"})").status, 409u) << "no quota";
  EXPECT_TRUE(kinds().empty());
}
