//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Store keys (model/keys.h): a project's records of one kind share a prefix,
// a family's revisions sort in revision order, a task's statuses are one per
// node, and no key can be made from a project or family that is not a name.

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "louhi/model/keys.h"
#include "louhi/test/fixtures/records.h"

using namespace louhi::model;
namespace fixture = louhi::test;

namespace {

bool starts_with(const std::string& text, const std::string& prefix) { return text.compare(0, prefix.size(), prefix) == 0; }

}  // namespace

TEST(Keys, AreAsTheFormatGivesThem) {
  EXPECT_EQ(key(fixture::project()), "project/web");
  EXPECT_EQ(key(fixture::quota()), "quota/web");
  EXPECT_EQ(key(fixture::task_definition()), "taskdef/web/frontend/0000000003");
  EXPECT_EQ(key(fixture::service()), "service/web/frontend");
  EXPECT_EQ(key(fixture::task()), "task/web/" + std::string(32, '1'));
  EXPECT_EQ(key(fixture::task_status()), "taskstatus/" + std::string(32, '1') + "/" + std::string(32, '2'));
  EXPECT_EQ(key(fixture::node_status()), "node/" + std::string(32, '2'));
}

TEST(Keys, RecordsShareTheirListingPrefix) {
  EXPECT_TRUE(starts_with(key(fixture::task_definition()), task_definitions_prefix("web", "frontend")));
  EXPECT_TRUE(starts_with(key(fixture::service()), services_prefix("web")));
  EXPECT_TRUE(starts_with(key(fixture::task()), tasks_prefix("web")));
  EXPECT_TRUE(starts_with(key(fixture::task_status()), task_statuses_prefix(fixture::task_id(0x11))));
}

TEST(Keys, AProjectsPrefixHoldsNoOtherProjects) {
  auto other = fixture::service();
  other.project = "web2";
  EXPECT_FALSE(starts_with(key(other), services_prefix("web")));
}

TEST(Keys, RevisionsSortInOrder) {
  auto a = fixture::task_definition();
  auto b = fixture::task_definition();
  a.revision = 9;
  b.revision = 10;
  EXPECT_LT(key(a), key(b));
  b.revision = 4'294'967'295u;
  EXPECT_LT(key(a), key(b));
}

TEST(Keys, AStatusPerNode) {
  auto a = fixture::task_status();
  auto b = fixture::task_status();
  b.node = fixture::node_id(0x33);
  EXPECT_NE(key(a), key(b));
}

TEST(Keys, RefuseWhatIsNotAName) {
  EXPECT_THROW(services_prefix("Web"), std::invalid_argument);
  EXPECT_THROW(tasks_prefix("a/b"), std::invalid_argument);
  EXPECT_THROW(task_definitions_prefix("web", ""), std::invalid_argument);
}
