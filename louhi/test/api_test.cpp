//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The API's operations (design, "API and sign-on", the endpoint table, and
// "Projects and quotas"): projects and quotas are put by louhi:admin only;
// every other operation needs its project's scope, and is refused for want
// of it before anything about the project is said; a family's revisions
// rise from 1; a task is run on this node until placement across nodes,
// admitted against its project's quota on this node's view, counting what
// this node has admitted that the store does not show yet; a client token
// makes a retry answer what the first made; answers are posted, never given
// inline.

#include "louhi/api/api.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "dannan/test/fixtures/stepped_time.h"
#include "louhi/model/codec.h"
#include "louhi/model/keys.h"
#include "louhi/store/memory_store.h"
#include "louhi/test/fixtures/fake_driver.h"
#include "louhi/test/fixtures/records.h"

using namespace louhi;
namespace asio = boost::asio;
namespace json = boost::json;

namespace {

dannan::status::ScopedLogger quiet() { return dannan::status::ScopedLogger(dannan::status::null_sink(), "test"); }

// A store whose writes are held until released, or failed: the Dagda store's
// writes complete later than a read on this node could see them.
class HeldStore final : public store::Store {
 public:
  bool hold = false;
  bool fail = false;
  bool ready() const override { return _inner.ready(); }
  std::optional<store::Bytes> get(const std::string& key) const override { return _inner.get(key); }
  std::vector<std::pair<std::string, store::Bytes>> list(const std::string& prefix) const override { return _inner.list(prefix); }
  void put(const std::string& key, store::Bytes value, store::Deadline deadline, store::Executor executor, store::Completion<store::Done> done) override {
    if (fail) {
      asio::post(executor, [done] { done(runtime::Error{runtime::Error::Kind::timed_out, "no answer"}); });
      return;
    }
    if (!hold) return _inner.put(key, std::move(value), deadline, std::move(executor), std::move(done));
    _held.push_back([this, key, value = std::move(value), deadline, executor, done]() mutable { _inner.put(key, std::move(value), deadline, executor, done); });
  }
  void erase(const std::string& key, store::Deadline deadline, store::Executor executor, store::Completion<store::Done> done) override {
    _inner.erase(key, deadline, std::move(executor), std::move(done));
  }
  uint64_t watch(const std::string& prefix, store::Executor executor, std::function<void()> changed) override {
    return _inner.watch(prefix, std::move(executor), std::move(changed));
  }
  void unwatch(uint64_t id) override { _inner.unwatch(id); }
  void release() {
    auto held = std::move(_held);
    _held.clear();
    for (auto& f : held) f();
  }

 private:
  store::MemoryStore _inner;
  std::vector<std::function<void()>> _held;
};

api::Grant grant_of(std::vector<std::string> scopes, std::string subject = "alice") { return api::Grant(std::move(subject), scopes, 0); }

const std::string alpine_digest = "sha256:" + std::string(64, 'b');

struct ApiTest : ::testing::Test {
  asio::io_context io;
  HeldStore store;
  dannan::test::SteppedWall wall{1'800'000'000'000};
  model::NodeId self = test::node_id(0x5e);
  test::FakeDriver images;
  api::Api api{store, wall, self, images, io.get_executor(), quiet()};
  api::Grant admin = grant_of({"louhi:admin"}, "root");
  api::Grant web = grant_of({"louhi:web:*"});

  ApiTest() { images.digests = {{"alpine", alpine_digest}, {"alpine:3.20", alpine_digest}}; }

  void settle() {
    io.restart();
    while (io.poll() > 0) {
    }
  }

  // The request's answer once everything posted has run.
  api::Response call(const api::Grant& grant, std::string method, std::string path, std::string body = "", std::map<std::string, std::string> query = {}) {
    std::optional<api::Response> got;
    api.handle(grant, {std::move(method), std::move(path), std::move(query), std::move(body)}, [&](api::Response r) { got = std::move(r); });
    EXPECT_FALSE(got) << "an answer is posted, never given inline";
    settle();
    EXPECT_TRUE(got) << "the answer came";
    return got.value_or(api::Response{0, {}});
  }

  void make_project(const std::string& quota = R"({"max_tasks":4,"max_cpu_millis":1000,"max_memory_mib":1024})") {
    ASSERT_EQ(call(admin, "PUT", "/v1/projects/web", R"({"description":"The public site"})").status, 200u);
    ASSERT_EQ(call(admin, "PUT", "/v1/projects/web/quota", quota).status, 200u);
  }

  uint32_t register_definition(const std::string& memory = "128") {
    auto r = call(web, "POST", "/v1/projects/web/task-definitions",
                  R"({"family":"frontend","cpu_millis":250,"memory_mib":)" + memory + R"(,"containers":[{"name":"app","image":"alpine:3.20"}]})");
    EXPECT_EQ(r.status, 200u) << json::serialize(r.body);
    return r.status == 200 ? r.body.at("revision").to_number<uint32_t>() : 0;
  }

  std::string code(const api::Response& r) {
    return r.body.is_object() && r.body.as_object().contains("error") ? std::string(r.body.at("error").as_string()) : "";
  }
};

}  // namespace

TEST_F(ApiTest, ProjectsAndQuotasArePutByAdminOnly) {
  EXPECT_EQ(call(web, "PUT", "/v1/projects/web", "{}").status, 403u);
  EXPECT_EQ(call(admin, "PUT", "/v1/projects/web", R"({"description":"The public site"})").status, 200u);
  EXPECT_EQ(call(web, "PUT", "/v1/projects/web/quota", R"({"max_tasks":100})").status, 403u) << "a project never moves its own limits";
  EXPECT_EQ(call(admin, "PUT", "/v1/projects/web/quota", R"({"max_tasks":4})").status, 200u);
  EXPECT_EQ(call(admin, "PUT", "/v1/projects/none/quota", R"({"max_tasks":4})").status, 404u);

  auto project = call(web, "GET", "/v1/projects/web");
  EXPECT_EQ(project.status, 200u);
  EXPECT_EQ(project.body.at("description"), "The public site");
  EXPECT_EQ(call(web, "GET", "/v1/projects/web/quota").body.at("max_tasks"), 4);
}

TEST_F(ApiTest, PuttingAProjectAgainKeepsWhenItWasMade) {
  call(admin, "PUT", "/v1/projects/web", "{}");
  wall.advance(std::chrono::hours(1));
  auto again = call(admin, "PUT", "/v1/projects/web", R"({"description":"new"})");
  EXPECT_EQ(again.body.at("created_unix_ms"), 1'800'000'000'000ull);
  EXPECT_EQ(again.body.at("description"), "new");
}

TEST_F(ApiTest, AProjectsScopeIsCheckedBeforeAnythingAboutItIsSaid) {
  const auto other = grant_of({"louhi:batch:*"});
  EXPECT_EQ(call(other, "GET", "/v1/projects/web").status, 403u);
  EXPECT_EQ(call(other, "GET", "/v1/projects/nowhere").status, 403u) << "a missing project looks the same as another's";
  EXPECT_EQ(call(other, "GET", "/v1/projects/nowhere/tasks").status, 403u);
}

TEST_F(ApiTest, ProjectListsShowOnlyThoseTheTokenHolds) {
  call(admin, "PUT", "/v1/projects/web", "{}");
  call(admin, "PUT", "/v1/projects/batch", "{}");
  auto mine = call(web, "GET", "/v1/projects");
  ASSERT_EQ(mine.body.at("projects").as_array().size(), 1u);
  EXPECT_EQ(mine.body.at("projects").as_array()[0].at("name"), "web");
  EXPECT_EQ(call(admin, "GET", "/v1/projects").body.at("projects").as_array().size(), 2u);
}

TEST_F(ApiTest, AFamilysRevisionsRiseFromOne) {
  make_project();
  EXPECT_EQ(register_definition(), 1u);
  EXPECT_EQ(register_definition(), 2u);
  auto described = call(web, "GET", "/v1/projects/web/task-definitions/frontend/2");
  EXPECT_EQ(described.status, 200u);
  EXPECT_EQ(described.body.at("revision"), 2);
  EXPECT_EQ(described.body.at("registered_unix_ms"), 1'800'000'000'000ull);
  EXPECT_EQ(call(web, "GET", "/v1/projects/web/task-definitions/frontend").body.at("task_definitions").as_array().size(), 2u);
  EXPECT_EQ(call(web, "GET", "/v1/projects/web/task-definitions").body.at("families"), json::value(json::array{"frontend"}));
  EXPECT_EQ(call(web, "GET", "/v1/projects/web/task-definitions/frontend/3").status, 404u);
}

TEST_F(ApiTest, RevisionsAreNotGivenTwiceWhileAWriteIsInFlight) {
  make_project();
  store.hold = true;
  std::vector<uint32_t> revisions;
  for (int i = 0; i < 2; ++i)
    api.handle(
        web, {"POST", "/v1/projects/web/task-definitions", {}, R"({"family":"f","cpu_millis":1,"memory_mib":4,"containers":[{"name":"a","image":"alpine"}]})"},
        [&](api::Response r) { revisions.push_back(r.body.at("revision").to_number<uint32_t>()); });
  settle();
  store.release();
  settle();
  EXPECT_EQ(revisions, (std::vector<uint32_t>{1, 2}));
}

// Design, "Images in detail": a tag is resolved to a digest when the revision
// is registered, so every task of it runs the same bytes; a reference that
// carries a digest keeps it; a tag moved later changes nothing registered.
TEST_F(ApiTest, RegisteringPinsEachImageToADigest) {
  make_project();
  const std::string pinned = "sha256:" + std::string(64, 'c');
  auto r = call(
      web, "POST", "/v1/projects/web/task-definitions",
      R"({"family":"f","cpu_millis":10,"memory_mib":8,"containers":[{"name":"a","image":"alpine:3.20"},{"name":"b","image":"busybox@)" + pinned + R"("}]})");
  ASSERT_EQ(r.status, 200u) << json::serialize(r.body);
  EXPECT_EQ(r.body.at("containers").at(0).at("image_digest"), json::value(alpine_digest));
  EXPECT_EQ(r.body.at("containers").at(1).at("image_digest"), json::value(pinned));

  images.digests["alpine:3.20"] = "sha256:" + std::string(64, 'd');
  const auto described = call(web, "GET", "/v1/projects/web/task-definitions/f/1");
  EXPECT_EQ(described.body.at("containers").at(0).at("image_digest"), json::value(alpine_digest)) << "a moved tag changes nothing registered";
}

TEST_F(ApiTest, ADigestTheCallerSendsIsNotTaken) {
  make_project();
  auto r = call(
      web, "POST", "/v1/projects/web/task-definitions",
      R"({"family":"f","cpu_millis":10,"memory_mib":8,"containers":[{"name":"a","image":"alpine","image_digest":"sha256:)" + std::string(64, 'e') + R"("}]})");
  ASSERT_EQ(r.status, 200u) << json::serialize(r.body);
  EXPECT_EQ(r.body.at("containers").at(0).at("image_digest"), json::value(alpine_digest)) << "the digest is the system's";
}

TEST_F(ApiTest, AnImageThatCannotBeResolvedRegistersNothing) {
  make_project();
  const std::string body = R"({"family":"f","cpu_millis":10,"memory_mib":8,"containers":[{"name":"a","image":"alpine"},{"name":"b","image":"nothing:1"}]})";
  auto r = call(web, "POST", "/v1/projects/web/task-definitions", body);
  EXPECT_EQ(r.status, 400u);
  EXPECT_EQ(code(r), "image_not_found");
  EXPECT_EQ(call(web, "GET", "/v1/projects/web/task-definitions/f/1").status, 404u);

  images.fail_next("resolve", runtime::Error{runtime::Error::Kind::refused, "the registry quay.io is not one this node is allowed to use"});
  r = call(web, "POST", "/v1/projects/web/task-definitions", body);
  EXPECT_EQ(r.status, 400u);
  EXPECT_EQ(code(r), "image_refused");

  images.fail_next("resolve", runtime::Error{runtime::Error::Kind::timed_out, "no answer"});
  r = call(web, "POST", "/v1/projects/web/task-definitions", body);
  EXPECT_EQ(r.status, 503u);
  EXPECT_EQ(code(r), "image_unresolved");

  images.digests["nothing:1"] = alpine_digest;
  r = call(web, "POST", "/v1/projects/web/task-definitions", body);
  ASSERT_EQ(r.status, 200u) << json::serialize(r.body);
  EXPECT_EQ(r.body.at("revision"), 1) << "no revision was spent on a refusal";
}

TEST_F(ApiTest, RegisteringNeedsTheRegisterScopeAndTheProject) {
  EXPECT_EQ(call(grant_of({"louhi:web:read", "louhi:web:run"}), "POST", "/v1/projects/web/task-definitions", "{}").status, 403u);
  EXPECT_EQ(
      call(web, "POST", "/v1/projects/web/task-definitions", R"({"family":"f","cpu_millis":1,"memory_mib":4,"containers":[{"name":"a","image":"alpine"}]})")
          .status,
      404u)
      << "no project yet";
  auto r = call(web, "POST", "/v1/projects/web/task-definitions", R"({"family":"f","containers":[]})");
  EXPECT_EQ(r.status, 400u);
  EXPECT_EQ(code(r), "invalid");
}

TEST_F(ApiTest, ARunTaskIsPlacedOnThisNodeAndRecordsWhoAsked) {
  make_project();
  register_definition();
  auto r = call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend"})");
  ASSERT_EQ(r.status, 200u) << json::serialize(r.body);
  const auto& tasks = r.body.at("tasks").as_array();
  ASSERT_EQ(tasks.size(), 1u);
  const auto id = model::TaskId::parse(tasks[0].at("id").as_string().c_str());
  ASSERT_TRUE(id);
  model::Task probe;
  probe.id = *id;
  probe.project = "web";
  const auto stored = store.get(model::key(probe));
  ASSERT_TRUE(stored);
  const auto task = model::decode_task(*stored);
  EXPECT_EQ(task.node, self);
  EXPECT_EQ(task.placement_epoch, 1u);
  EXPECT_EQ(task.revision, 1u) << "the latest when none is named";
  EXPECT_EQ(task.started_by, "alice");
  EXPECT_EQ(task.desired, model::DesiredState::running);
}

TEST_F(ApiTest, ARunTaskOfARevisionThatDoesNotExistIsNotFound) {
  make_project();
  EXPECT_EQ(call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend"})").status, 404u);
  register_definition();
  EXPECT_EQ(call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend","revision":2})").status, 404u);
}

TEST_F(ApiTest, AProjectWithNoQuotaMayRunNothing) {
  call(admin, "PUT", "/v1/projects/web", "{}");
  register_definition();
  auto r = call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend"})");
  EXPECT_EQ(r.status, 409u);
  EXPECT_EQ(code(r), "quota");
}

TEST_F(ApiTest, AdmissionRefusesWhatWouldPassEachLimitAndSaysWhich) {
  make_project(R"({"max_tasks":3,"max_cpu_millis":1000,"max_memory_mib":300})");
  register_definition("100");
  EXPECT_EQ(
      call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend","count":4})").body.at("message").as_string().find("max_tasks") != json::string::npos,
      true);
  // 250 millis each: four would pass 1000; three would not, but 3 x 100 MiB is the memory limit exactly.
  EXPECT_EQ(call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend","count":3})").status, 200u);
  auto r = call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend"})");
  EXPECT_EQ(r.status, 409u);
  EXPECT_NE(std::string(r.body.at("message").as_string()).find("max_"), std::string::npos);
}

TEST_F(ApiTest, AdmissionCountsMemoryAndCpu) {
  make_project(R"({"max_tasks":10,"max_cpu_millis":500,"max_memory_mib":10000})");
  register_definition();
  auto r = call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend","count":3})");
  EXPECT_EQ(r.status, 409u);
  EXPECT_NE(std::string(r.body.at("message").as_string()).find("max_cpu_millis"), std::string::npos);

  make_project(R"({"max_tasks":10,"max_cpu_millis":10000,"max_memory_mib":200})");
  r = call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend","count":2})");
  EXPECT_EQ(r.status, 409u);
  EXPECT_NE(std::string(r.body.at("message").as_string()).find("max_memory_mib"), std::string::npos);
}

TEST_F(ApiTest, AStoppedTaskNoLongerCountsAgainstTheQuota) {
  make_project(R"({"max_tasks":1,"max_cpu_millis":1000,"max_memory_mib":1000})");
  register_definition();
  auto first = call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend"})");
  EXPECT_EQ(call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend"})").status, 409u);
  const std::string id(first.body.at("tasks").as_array()[0].at("id").as_string());
  EXPECT_EQ(call(web, "POST", "/v1/projects/web/tasks/" + id + "/stop", "").status, 200u);
  EXPECT_EQ(call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend"})").status, 200u);
}

TEST_F(ApiTest, AdmissionCountsWhatThisNodeAdmittedThatTheStoreDoesNotShowYet) {
  make_project(R"({"max_tasks":1,"max_cpu_millis":1000,"max_memory_mib":1000})");
  register_definition();
  store.hold = true;
  std::vector<unsigned> statuses;
  for (int i = 0; i < 2; ++i)
    api.handle(web, {"POST", "/v1/projects/web/tasks", {}, R"({"family":"frontend"})"}, [&](api::Response r) { statuses.push_back(r.status); });
  settle();
  store.release();
  settle();
  std::sort(statuses.begin(), statuses.end());
  EXPECT_EQ(statuses, (std::vector<unsigned>{200, 409})) << "the second is refused though the first's write has not landed";
}

TEST_F(ApiTest, AClientTokenMakesARetryAnswerWhatTheFirstMade) {
  make_project();
  register_definition();
  const std::string body = R"({"family":"frontend","count":2,"client_token":"retry-1"})";
  auto first = call(web, "POST", "/v1/projects/web/tasks", body);
  auto again = call(web, "POST", "/v1/projects/web/tasks", body);
  ASSERT_EQ(first.status, 200u);
  ASSERT_EQ(again.status, 200u);
  EXPECT_EQ(first.body.at("tasks"), again.body.at("tasks"));
  EXPECT_EQ(store.list(model::tasks_prefix("web")).size(), 2u);

  wall.advance(api::Api::client_token_life);
  auto later = call(web, "POST", "/v1/projects/web/tasks", body);
  EXPECT_NE(later.body.at("tasks"), first.body.at("tasks")) << "a token is remembered for an hour";
}

TEST_F(ApiTest, StoppingATaskRecordsWhyAndStoppingItAgainSucceeds) {
  make_project();
  register_definition();
  auto run = call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend"})");
  const std::string id(run.body.at("tasks").as_array()[0].at("id").as_string());
  auto stopped = call(web, "POST", "/v1/projects/web/tasks/" + id + "/stop", R"({"reason":"done with it"})");
  EXPECT_EQ(stopped.status, 200u);
  EXPECT_EQ(stopped.body.at("desired"), "stopped");
  EXPECT_EQ(stopped.body.at("stop_reason"), "done with it");
  auto again = call(web, "POST", "/v1/projects/web/tasks/" + id + "/stop", R"({"reason":"other"})");
  EXPECT_EQ(again.status, 200u);
  EXPECT_EQ(again.body.at("stop_reason"), "done with it");
  EXPECT_EQ(call(grant_of({"louhi:web:read"}), "POST", "/v1/projects/web/tasks/" + id + "/stop").status, 403u);
  EXPECT_EQ(call(web, "POST", "/v1/projects/web/tasks/" + std::string(32, '0') + "/stop").status, 404u);
}

TEST_F(ApiTest, DescribingATaskShowsEveryNodesStatusOfIt) {
  make_project();
  register_definition();
  auto run = call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend"})");
  const std::string id(run.body.at("tasks").as_array()[0].at("id").as_string());
  auto status = test::task_status();
  status.task = *model::TaskId::parse(id);
  status.node = self;
  bool written = false;
  store.put(model::key(status), model::encode(status), store::Deadline(1000), io.get_executor(), [&](auto r) { written = r.ok(); });
  settle();
  ASSERT_TRUE(written);
  auto described = call(grant_of({"louhi:web:read"}), "GET", "/v1/projects/web/tasks/" + id);
  EXPECT_EQ(described.status, 200u);
  ASSERT_EQ(described.body.at("statuses").as_array().size(), 1u);
  EXPECT_EQ(described.body.at("statuses").as_array()[0].at("state"), "running");
}

TEST_F(ApiTest, ListsArePagedByLimitAndNext) {
  make_project(R"({"max_tasks":10,"max_cpu_millis":10000,"max_memory_mib":10000})");
  register_definition();
  call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend","count":5})");
  auto first = call(web, "GET", "/v1/projects/web/tasks", "", {{"limit", "3"}});
  ASSERT_EQ(first.body.at("tasks").as_array().size(), 3u);
  const std::string next(first.body.at("next").as_string());
  auto second = call(web, "GET", "/v1/projects/web/tasks", "", {{"limit", "3"}, {"next", next}});
  EXPECT_EQ(second.body.at("tasks").as_array().size(), 2u);
  EXPECT_FALSE(second.body.as_object().contains("next"));
  EXPECT_EQ(call(web, "GET", "/v1/projects/web/tasks", "", {{"limit", "0"}}).status, 400u);
  EXPECT_EQ(call(web, "GET", "/v1/projects/web/tasks", "", {{"limit", "101"}}).status, 400u);
}

TEST_F(ApiTest, NodesAreListedForAdminOnly) {
  auto node = test::node_status();
  bool written = false;
  store.put(model::key(node), model::encode(node), store::Deadline(1000), io.get_executor(), [&](auto r) { written = r.ok(); });
  settle();
  ASSERT_TRUE(written);
  EXPECT_EQ(call(web, "GET", "/v1/nodes").status, 403u);
  auto nodes = call(admin, "GET", "/v1/nodes");
  EXPECT_EQ(nodes.status, 200u);
  EXPECT_EQ(nodes.body.at("nodes").as_array().size(), 1u);
}

TEST_F(ApiTest, AFailedWriteIsAnsweredAsNotWritten) {
  call(admin, "PUT", "/v1/projects/web", "{}");
  store.fail = true;
  auto r = call(admin, "PUT", "/v1/projects/web/quota", R"({"max_tasks":1})");
  EXPECT_EQ(r.status, 503u);
}

TEST_F(ApiTest, UnknownPathsAndMethodsAreRefused) {
  EXPECT_EQ(call(admin, "GET", "/").status, 404u);
  EXPECT_EQ(call(admin, "GET", "/v2/projects").status, 404u);
  EXPECT_EQ(call(admin, "GET", "/v1/projects/Web").status, 404u);
  EXPECT_EQ(call(admin, "GET", "/v1/projects/web/tasks/not-an-id").status, 404u);
  EXPECT_EQ(call(admin, "GET", "/v1/projects/web/task-definitions/f/0").status, 404u);
  EXPECT_EQ(call(admin, "GET", "/v1/projects/web/elsewhere").status, 404u);
  EXPECT_EQ(call(admin, "DELETE", "/v1/projects/web").status, 405u);
  EXPECT_EQ(call(admin, "PUT", "/v1/nodes").status, 405u);
}

TEST_F(ApiTest, ABodyThatIsNotJsonIsInvalid) {
  call(admin, "PUT", "/v1/projects/web", "{}");
  auto r = call(admin, "PUT", "/v1/projects/web/quota", "{not json");
  EXPECT_EQ(r.status, 400u);
  EXPECT_EQ(code(r), "invalid");
}
