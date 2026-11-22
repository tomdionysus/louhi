//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The agent's inputs over the store (agent/store_bindings.h): the tasks placed
// on this node, each with its revision and this node's last status of it, so
// what the agent and the node's report decide by comes from the records
// (disciplines 1 and 3), never from memory a restart loses.

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>

#include "louhi/agent/store_bindings.h"
#include "louhi/model/codec.h"
#include "louhi/model/keys.h"
#include "louhi/store/memory_store.h"
#include "louhi/test/fixtures/records.h"

using namespace louhi;
namespace asio = boost::asio;

namespace {

dannan::status::ScopedLogger quiet() { return dannan::status::ScopedLogger(dannan::status::null_sink(), "test"); }

struct StoreDesiredTest : ::testing::Test {
  asio::io_context io;
  store::MemoryStore store;
  model::NodeId self = test::node_id(0x22);
  agent::StoreDesired desired{store, self, quiet()};

  void put(const std::string& key, store::Bytes bytes) {
    bool written = false;
    store.put(key, std::move(bytes), store::Deadline(1000), io.get_executor(), [&](auto r) { written = r.ok(); });
    io.restart();
    io.run();
    ASSERT_TRUE(written) << key;
  }

  model::Task placed(uint8_t id, model::NodeId node) {
    auto t = test::task();
    t.id = test::task_id(id);
    t.node = node;
    put(model::key(t), model::encode(t));
    return t;
  }
};

}  // namespace

TEST_F(StoreDesiredTest, AssignmentsAreTheTasksPlacedHereWithTheirRevisions) {
  put(model::key(test::task_definition()), model::encode(test::task_definition()));
  placed(1, self);
  placed(2, test::node_id(0x77));
  const auto got = desired.assignments();
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].task.id, test::task_id(1));
  EXPECT_EQ(got[0].definition, test::task_definition());
  EXPECT_FALSE(got[0].status) << "nothing reported yet";
}

TEST_F(StoreDesiredTest, AnAssignmentCarriesThisNodesStatusOfItAndNoOther) {
  put(model::key(test::task_definition()), model::encode(test::task_definition()));
  placed(1, self);
  auto mine = test::task_status();
  mine.task = test::task_id(1);
  mine.node = self;
  mine.state = model::TaskState::stopped;
  mine.containers.clear();
  auto theirs = mine;
  theirs.node = test::node_id(0x77);
  theirs.state = model::TaskState::running;
  put(model::key(theirs), model::encode(theirs));
  ASSERT_FALSE(desired.assignments().at(0).status) << "another node's status is not this node's";
  put(model::key(mine), model::encode(mine));
  const auto got = desired.assignments();
  ASSERT_EQ(got.size(), 1u);
  ASSERT_TRUE(got[0].status);
  EXPECT_EQ(*got[0].status, mine);
}

TEST_F(StoreDesiredTest, AStatusThatDoesNotDecodeIsLeftOutAndTheTaskKept) {
  put(model::key(test::task_definition()), model::encode(test::task_definition()));
  placed(1, self);
  auto mine = test::task_status();
  mine.task = test::task_id(1);
  mine.node = self;
  put(model::key(mine), store::Bytes{0xff, 0x00});
  const auto got = desired.assignments();
  ASSERT_EQ(got.size(), 1u);
  EXPECT_FALSE(got[0].status);
}
