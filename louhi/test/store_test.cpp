//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The store as Louhi uses it (store/store.h), in memory: records by key,
// listed by prefix in key order, written with a completion posted to the
// caller's executor, and watched by prefix with a prompt, never the record.
// And the agent over it (agent/store_bindings.h): the tasks placed on this
// node with their revisions are what it runs; a task whose revision is
// missing or a record that does not decode is left out; what it observes is
// written under the status's own key.

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <string>
#include <vector>

#include "dannan/test/fixtures/stepped_time.h"
#include "louhi/agent/store_bindings.h"
#include "louhi/model/keys.h"
#include "louhi/store/memory_store.h"
#include "louhi/test/fixtures/fake_driver.h"
#include "louhi/test/fixtures/records.h"

using namespace louhi;
namespace asio = boost::asio;

namespace {

void settle(asio::io_context& io) {
  io.restart();
  while (io.poll() > 0) {
  }
}

void put(store::Store& s, asio::io_context& io, const std::string& key, model::Bytes value) {
  bool done = false;
  s.put(key, std::move(value), store::Deadline(1000), io.get_executor(), [&](runtime::Result<runtime::Done> r) { done = r.ok(); });
  settle(io);
  ASSERT_TRUE(done);
}

dannan::status::ScopedLogger quiet() { return dannan::status::ScopedLogger(dannan::status::null_sink(), "test"); }

}  // namespace

TEST(MemoryStore, GetListPutErase) {
  asio::io_context io;
  store::MemoryStore s;
  EXPECT_TRUE(s.ready());
  EXPECT_FALSE(s.get("a/1"));
  put(s, io, "a/2", {2});
  put(s, io, "a/1", {1});
  put(s, io, "b/1", {3});
  EXPECT_EQ(s.get("a/1"), (model::Bytes{1}));
  const auto listed = s.list("a/");
  ASSERT_EQ(listed.size(), 2u);
  EXPECT_EQ(listed[0].first, "a/1");
  EXPECT_EQ(listed[1].first, "a/2");
  EXPECT_EQ(s.list("").size(), 3u);
  bool erased = false;
  s.erase("a/1", store::Deadline(1000), io.get_executor(), [&](runtime::Result<runtime::Done> r) { erased = r.ok(); });
  s.erase("never", store::Deadline(1000), io.get_executor(), [&](runtime::Result<runtime::Done> r) { EXPECT_TRUE(r.ok()); });
  settle(io);
  EXPECT_TRUE(erased);
  EXPECT_FALSE(s.get("a/1"));
}

TEST(MemoryStore, WatchesPromptByPrefixOnly) {
  asio::io_context io;
  store::MemoryStore s;
  int a = 0;
  int b = 0;
  const uint64_t wa = s.watch("a/", io.get_executor(), [&] { ++a; });
  s.watch("b/", io.get_executor(), [&] { ++b; });
  put(s, io, "a/1", {1});
  EXPECT_EQ(a, 1);
  EXPECT_EQ(b, 0);
  s.unwatch(wa);
  put(s, io, "a/2", {1});
  EXPECT_EQ(a, 1);
}

TEST(StoreDesired, RunsWhatIsPlacedHereWithItsRevision) {
  asio::io_context io;
  store::MemoryStore s;
  const auto definition = test::task_definition();
  auto here = test::task();
  here.node = test::node_id(0x22);
  auto elsewhere = test::task();
  elsewhere.id = test::task_id(0x33);
  elsewhere.node = test::node_id(0x44);
  auto missing_revision = test::task();
  missing_revision.id = test::task_id(0x55);
  missing_revision.revision = 99;
  missing_revision.node = here.node;
  put(s, io, model::key(definition), model::encode(definition));
  put(s, io, model::key(here), model::encode(here));
  put(s, io, model::key(elsewhere), model::encode(elsewhere));
  put(s, io, model::key(missing_revision), model::encode(missing_revision));
  put(s, io, "task/web/" + std::string(32, '9'), {0xff, 0x00});

  agent::StoreDesired desired(s, test::node_id(0x22), quiet());
  EXPECT_TRUE(desired.ready());
  const auto assignments = desired.assignments();
  ASSERT_EQ(assignments.size(), 1u);
  EXPECT_EQ(assignments[0].task, here);
  EXPECT_EQ(assignments[0].definition, definition);
}

TEST(StoreReports, WritesUnderTheStatusKey) {
  asio::io_context io;
  store::MemoryStore s;
  agent::StoreReports reports(s, io.get_executor(), quiet());
  const auto status = test::task_status();
  reports.report(status);
  settle(io);
  const auto stored = s.get(model::key(status));
  ASSERT_TRUE(stored);
  EXPECT_EQ(model::decode_task_status(*stored), status);
}

TEST(AgentOverTheStore, ATaskWrittenToTheStoreRunsAndItsStatusComesBack) {
  asio::io_context io;
  store::MemoryStore s;
  test::FakeDriver driver;
  dannan::test::SteppedTime time;
  dannan::test::SteppedWall wall;
  const model::NodeId self = test::node_id(0x22);
  agent::StoreDesired desired(s, self, quiet());
  agent::StoreReports reports(s, io.get_executor(), quiet());
  agent::Clock clock{&time, &wall, [](std::chrono::milliseconds, std::function<void()>) {}};
  auto a = std::make_shared<agent::Agent>(driver, desired, reports, self, clock, io.get_executor(), quiet(), agent::Policy{});
  s.watch("task/", io.get_executor(), [a] { a->poke(); });
  a->start();
  settle(io);
  EXPECT_TRUE(driver.containers().empty());

  const auto definition = test::task_definition();
  auto task = test::task();
  task.node = self;
  put(s, io, model::key(definition), model::encode(definition));
  put(s, io, model::key(task), model::encode(task));
  settle(io);

  EXPECT_EQ(driver.containers().size(), 2u);
  model::TaskStatus probe;
  probe.task = task.id;
  probe.node = self;
  const auto stored = s.get(model::key(probe));
  ASSERT_TRUE(stored);
  EXPECT_EQ(model::decode_task_status(*stored).state, model::TaskState::running);
}
