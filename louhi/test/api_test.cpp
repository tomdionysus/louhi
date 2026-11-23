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
#include "louhi/runtime/labels.h"
#include "louhi/store/memory_store.h"
#include "louhi/test/fixtures/fake_driver.h"
#include "louhi/test/fixtures/records.h"
#include "louhi/version.h"

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
  bool unready = false;
  bool ready() const override { return !unready && _inner.ready(); }
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
    report_capacity(64'000, 65'536);
  }

  // This node's report of itself, with room for what a test runs unless it
  // says less.
  void report_capacity(uint32_t cpu_millis, uint32_t memory_mib) {
    auto status = test::node_status();
    status.node = self;
    status.total = {cpu_millis, memory_mib};
    put_raw(model::key(status), model::encode(status));
  }

  uint32_t register_definition(const std::string& memory = "128") {
    auto r = call(web, "POST", "/v1/projects/web/task-definitions",
                  R"({"family":"frontend","cpu_millis":250,"memory_mib":)" + memory + R"(,"containers":[{"name":"app","image":"alpine:3.20"}]})");
    EXPECT_EQ(r.status, 200u) << json::serialize(r.body);
    return r.status == 200 ? r.body.at("revision").to_number<uint32_t>() : 0;
  }

  // Bytes put under a key as they are, decodable or not.
  void put_raw(const std::string& key, store::Bytes bytes) {
    bool written = false;
    store.put(key, std::move(bytes), store::Deadline(1000), io.get_executor(), [&](auto r) { written = r.ok(); });
    settle();
    ASSERT_TRUE(written) << key;
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

TEST_F(ApiTest, AFinishedTaskNoLongerCountsAgainstTheQuota) {
  make_project(R"({"max_tasks":1,"max_cpu_millis":1000,"max_memory_mib":1000})");
  register_definition();
  auto first = call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend"})");
  const std::string id(first.body.at("tasks").as_array()[0].at("id").as_string());
  auto status = test::task_status();
  status.task = *model::TaskId::parse(id);
  status.node = self;
  status.state = model::TaskState::stopped;
  status.reason = "essential container app exited with 0";
  status.containers = {{"app", model::ContainerState::exited, 0, 0, model::Health::unknown}};
  status.node = test::node_id(0x77);
  put_raw(model::key(status), model::encode(status));
  EXPECT_EQ(call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend"})").status, 409u) << "another node's report finishes nothing";
  status.node = self;
  put_raw(model::key(status), model::encode(status));
  EXPECT_EQ(call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend"})").status, 200u);
}

// Node-capacity admission: a task is refused that this node cannot hold beside
// every task placed on it that holds resources, of any project, and what it
// has admitted that the store does not show yet; refused outright while the
// node has not reported what it has.
TEST_F(ApiTest, RunTaskWaitsForThisNodeToReportItsCapacity) {
  call(admin, "PUT", "/v1/projects/web", "{}");
  call(admin, "PUT", "/v1/projects/web/quota", R"({"max_tasks":4,"max_cpu_millis":1000,"max_memory_mib":1024})");
  register_definition();
  auto r = call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend"})");
  EXPECT_EQ(r.status, 503u);
  EXPECT_EQ(code(r), "capacity_unknown");
  report_capacity(4000, 8192);
  EXPECT_EQ(call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend"})").status, 200u);
}

TEST_F(ApiTest, AdmissionRefusesWhatThisNodeCannotHoldAndSaysWhich) {
  make_project(R"({"max_tasks":100,"max_cpu_millis":100000,"max_memory_mib":100000})");
  report_capacity(1000, 100'000);
  register_definition();
  // 250 millis each: four fill the node exactly.
  EXPECT_EQ(call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend","count":3})").status, 200u);
  auto r = call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend","count":2})");
  EXPECT_EQ(r.status, 409u);
  EXPECT_EQ(code(r), "capacity");
  EXPECT_NE(std::string(r.body.at("message").as_string()).find("CPU"), std::string::npos);
  EXPECT_EQ(call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend"})").status, 200u);

  report_capacity(100'000, 300);
  r = call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend"})");
  EXPECT_EQ(r.status, 409u);
  EXPECT_NE(std::string(r.body.at("message").as_string()).find("memory"), std::string::npos);
}

TEST_F(ApiTest, CapacityCountsEveryProjectsTasksHereAndNoneElsewhere) {
  make_project(R"({"max_tasks":100,"max_cpu_millis":100000,"max_memory_mib":100000})");
  report_capacity(1000, 100'000);
  register_definition();
  // Another project's task here holds 500; one placed on another node holds nothing here.
  for (const auto& [id, node] : {std::pair{0x31, self}, std::pair{0x32, test::node_id(0x77)}}) {
    auto other = test::task();
    other.id = test::task_id(static_cast<uint8_t>(id));
    other.project = "shop";
    other.service.clear();
    other.node = node;
    auto definition = test::task_definition();
    definition.project = "shop";
    definition.family = other.family;
    definition.revision = other.revision;
    definition.cpu_millis = 500;
    for (auto& container : definition.containers) container.cpu_millis = 0;
    put_raw(model::key(definition), model::encode(definition));
    put_raw(model::key(other), model::encode(other));
  }
  EXPECT_EQ(call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend","count":2})").status, 200u);
  EXPECT_EQ(code(call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend"})")), "capacity");
}

TEST_F(ApiTest, AFinishedTaskFreesItsNodesCapacity) {
  make_project(R"({"max_tasks":100,"max_cpu_millis":100000,"max_memory_mib":100000})");
  report_capacity(250, 100'000);
  register_definition();
  auto first = call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend"})");
  ASSERT_EQ(first.status, 200u);
  EXPECT_EQ(code(call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend"})")), "capacity");
  auto status = test::task_status();
  status.task = *model::TaskId::parse(std::string(first.body.at("tasks").as_array()[0].at("id").as_string()));
  status.node = self;
  status.state = model::TaskState::stopped;
  status.containers = {{"app", model::ContainerState::exited, 0, 0, model::Health::unknown}};
  put_raw(model::key(status), model::encode(status));
  EXPECT_EQ(call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend"})").status, 200u);
}

TEST_F(ApiTest, CapacityCountsWhatThisNodeAdmittedThatTheStoreDoesNotShowYet) {
  make_project(R"({"max_tasks":100,"max_cpu_millis":100000,"max_memory_mib":100000})");
  report_capacity(250, 100'000);
  register_definition();
  store.hold = true;
  std::vector<std::string> codes;
  for (int i = 0; i < 2; ++i)
    api.handle(web, {"POST", "/v1/projects/web/tasks", {}, R"({"family":"frontend"})"}, [&](api::Response r) { codes.push_back(code(r)); });
  settle();
  store.release();
  settle();
  std::sort(codes.begin(), codes.end());
  EXPECT_EQ(codes, (std::vector<std::string>{"", "capacity"}));
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

// Every route's scope comes from the route table and is checked before its
// handler runs: a token without it is refused with the scope named, and one
// holding only another project's scopes learns nothing of this one.
TEST_F(ApiTest, EveryRoutesScopeIsCheckedFromTheTable) {
  const auto other = grant_of({"louhi:batch:*"});
  for (const api::Route& route : api::routes()) {
    std::string path(route.path);
    for (const auto& [name, value] : std::vector<std::pair<std::string, std::string>>{
             {"{project}", "web"}, {"{family}", "frontend"}, {"{revision}", "1"}, {"{task}", std::string(32, '0')}})
      if (const auto at = path.find(name); at != std::string::npos) path.replace(at, name.size(), value);
    const auto r = call(other, std::string(route.method), path, "{}");
    if (route.scope == api::Scope::any) {
      EXPECT_NE(r.status, 403u) << route.method << " " << path;
      continue;
    }
    EXPECT_EQ(r.status, 403u) << route.method << " " << path;
    EXPECT_EQ(code(r), "forbidden");
    EXPECT_NE(std::string(r.body.at("message").as_string()).find(api::scope_text(route.scope, "web")), std::string::npos) << json::serialize(r.body);
  }
}

// Dashboard design, "The API the dashboard reads": the OpenAPI document is
// answered to any signed-in token, and to none without one (api_http_test).
TEST_F(ApiTest, TheOpenApiDocumentIsAnsweredToAnySignedInToken) {
  auto r = call(grant_of({"louhi:batch:read"}), "GET", "/v1/openapi.json");
  ASSERT_EQ(r.status, 200u);
  EXPECT_EQ(r.body.at("openapi"), "3.1.0");
  EXPECT_TRUE(r.body.at("paths").as_object().contains("/v1/status"));
  EXPECT_EQ(call(admin, "POST", "/v1/openapi.json").status, 405u);
}

// GET /v1/status: this node as it sees itself, to any signed-in token: its
// id, Louhi's version, whether its last report said it was ready (not before
// its first), and the store's state; louhi:admin also sees how many tasks it
// runs and the report itself.
TEST_F(ApiTest, StatusAnswersThisNodesViewOfItself) {
  auto before = call(web, "GET", "/v1/status");
  ASSERT_EQ(before.status, 200u);
  EXPECT_EQ(std::string(before.body.at("node").as_string()), dannan::identity::to_string(self));
  EXPECT_EQ(before.body.at("version"), louhi::kVersion);
  EXPECT_FALSE(before.body.at("ready").as_bool()) << "no report yet";
  EXPECT_TRUE(before.body.at("store").at("ready").as_bool());
  auto admin_before = call(admin, "GET", "/v1/status");
  EXPECT_EQ(admin_before.body.at("tasks"), 0);
  EXPECT_TRUE(admin_before.body.at("report").is_null());

  auto report = test::node_status();
  report.node = self;
  report.tasks = 3;
  bool written = false;
  store.put(model::key(report), model::encode(report), store::Deadline(1000), io.get_executor(), [&](auto r) { written = r.ok(); });
  settle();
  ASSERT_TRUE(written);

  auto seen = call(web, "GET", "/v1/status");
  EXPECT_TRUE(seen.body.at("ready").as_bool());
  EXPECT_FALSE(seen.body.as_object().contains("tasks")) << "the detail is admin's";
  EXPECT_FALSE(seen.body.as_object().contains("report"));
  auto everything = call(admin, "GET", "/v1/status");
  EXPECT_EQ(everything.body.at("tasks"), 3);
  EXPECT_EQ(everything.body.at("report").at("conditions"), json::value(json::array{"disk 80% full"}));
}

TEST_F(ApiTest, StatusIsOfThisNodeAloneAndShowsTheStoresState) {
  auto elsewhere = test::node_status();
  ASSERT_FALSE(elsewhere.node == self);
  bool written = false;
  store.put(model::key(elsewhere), model::encode(elsewhere), store::Deadline(1000), io.get_executor(), [&](auto r) { written = r.ok(); });
  settle();
  ASSERT_TRUE(written);
  auto r = call(admin, "GET", "/v1/status");
  EXPECT_FALSE(r.body.at("ready").as_bool()) << "another node's report is not this node's";
  EXPECT_TRUE(r.body.at("report").is_null());

  store.unready = true;
  EXPECT_FALSE(call(web, "GET", "/v1/status").body.at("store").at("ready").as_bool());
}

// A record that does not decode is damage in the store, not the caller's
// fault: every list skips it and answers the rest, and admission still counts
// a running task whose revision does not decode, without its resources.
TEST_F(ApiTest, ARecordThatDoesNotDecodeIsSkippedByEveryList) {
  make_project(R"({"max_tasks":3,"max_cpu_millis":1000,"max_memory_mib":1024})");
  register_definition();
  auto run = call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend"})");
  ASSERT_EQ(run.status, 200u) << json::serialize(run.body);
  const std::string id(run.body.at("tasks").as_array()[0].at("id").as_string());

  const store::Bytes garbage{0xff, 0x00};
  model::Project other;
  other.name = "zzz";
  put_raw(model::key(other), garbage);
  put_raw(model::task_definition_key("web", "frontend", 3), garbage);
  // Running revision 3, whose record does not decode.
  put_raw(model::key(test::task()), model::encode(test::task()));
  model::Task broken;
  broken.id = test::task_id(0x77);
  broken.project = "web";
  put_raw(model::key(broken), garbage);
  auto status = test::task_status();
  status.task = *model::TaskId::parse(id);
  status.node = test::node_id(0x99);
  put_raw(model::key(status), garbage);
  auto node = test::node_status();
  node.node = test::node_id(0x99);
  put_raw(model::key(node), garbage);

  auto projects = call(admin, "GET", "/v1/projects");
  ASSERT_EQ(projects.status, 200u);
  EXPECT_EQ(projects.body.at("projects").as_array().size(), 1u);
  auto revisions = call(web, "GET", "/v1/projects/web/task-definitions/frontend");
  ASSERT_EQ(revisions.status, 200u);
  EXPECT_EQ(revisions.body.at("task_definitions").as_array().size(), 1u);
  auto tasks = call(web, "GET", "/v1/projects/web/tasks");
  ASSERT_EQ(tasks.status, 200u);
  EXPECT_EQ(tasks.body.at("tasks").as_array().size(), 2u);
  auto described = call(web, "GET", "/v1/projects/web/tasks/" + id);
  ASSERT_EQ(described.status, 200u);
  EXPECT_TRUE(described.body.at("statuses").as_array().empty());
  auto nodes = call(admin, "GET", "/v1/nodes");
  ASSERT_EQ(nodes.status, 200u);
  ASSERT_EQ(nodes.body.at("nodes").as_array().size(), 1u) << "this node's own report, which decodes; the damaged one is skipped";
  EXPECT_EQ(nodes.body.at("nodes").as_array()[0].at("node"), json::value(dannan::identity::to_string(self)));

  // Revision 1 named: the latest, 3, is the damaged one, and running "the
  // latest" must not quietly run another.
  auto two = call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend","revision":1,"count":2})");
  EXPECT_EQ(two.status, 409u);
  EXPECT_NE(std::string(two.body.at("message").as_string()).find("max_tasks"), std::string::npos) << json::serialize(two.body);
  EXPECT_EQ(call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend","revision":1})").status, 200u);
  // The latest is the damaged revision: it fails inside Louhi, not elsewhere.
  EXPECT_EQ(call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend"})").status, 500u);
}

// The one record an operation reads does not decode: it fails inside Louhi,
// saying nothing of the record; putting the project again replaces it.
TEST_F(ApiTest, ARecordAnOperationNeedsThatDoesNotDecodeFailsInsideLouhi) {
  put_raw(model::key(test::project()), store::Bytes{0xff});
  auto r = call(web, "GET", "/v1/projects/web");
  EXPECT_EQ(r.status, 500u);
  EXPECT_EQ(code(r), "internal");
  auto put = call(admin, "PUT", "/v1/projects/web", R"({"description":"again"})");
  ASSERT_EQ(put.status, 200u) << json::serialize(put.body);
  EXPECT_EQ(put.body.at("created_unix_ms"), 1'800'000'000'000ull) << "made now: when it was first made is lost";
  EXPECT_EQ(call(web, "GET", "/v1/projects/web").status, 200u);
}

TEST_F(ApiTest, StatusWithAReportThatDoesNotDecodeIsNotReady) {
  model::NodeStatus probe;
  probe.node = self;
  put_raw(model::key(probe), store::Bytes{0xff});
  auto r = call(admin, "GET", "/v1/status");
  ASSERT_EQ(r.status, 200u);
  EXPECT_FALSE(r.body.at("ready").as_bool());
  EXPECT_TRUE(r.body.at("report").is_null());
}

TEST_F(ApiTest, AClientTokenRetryWhileTheFirstsWritesAreInFlightAnswersTheSame) {
  make_project();
  register_definition();
  const std::string body = R"({"family":"frontend","count":2,"client_token":"retry-1"})";
  store.hold = true;
  std::optional<api::Response> first;
  api.handle(web, {"POST", "/v1/projects/web/tasks", {}, body}, [&](api::Response r) { first = std::move(r); });
  settle();
  EXPECT_FALSE(first) << "its writes are held";
  auto again = call(web, "POST", "/v1/projects/web/tasks", body);
  ASSERT_EQ(again.status, 200u);
  store.release();
  settle();
  ASSERT_TRUE(first);
  ASSERT_EQ(first->status, 200u);
  EXPECT_EQ(first->body.at("tasks"), again.body.at("tasks"));
  EXPECT_EQ(store.list(model::tasks_prefix("web")).size(), 2u);
}

// The runtime's resolve answers what is not a digest: nothing is registered
// and no revision is spent.
TEST_F(ApiTest, ADigestThatIsNotOneRegistersNothing) {
  make_project();
  images.digests["alpine:3.20"] = "latest";
  auto r = call(web, "POST", "/v1/projects/web/task-definitions",
                R"({"family":"frontend","cpu_millis":250,"memory_mib":128,"containers":[{"name":"app","image":"alpine:3.20"}]})");
  EXPECT_EQ(r.status, 500u);
  EXPECT_EQ(code(r), "internal");
  images.digests["alpine:3.20"] = alpine_digest;
  EXPECT_EQ(register_definition(), 1u);
}

// GetTaskLogs: what one of a task's containers wrote, from the node it runs
// on, while its containers are kept; the container named, or the only one;
// paged by the last line's time.
namespace {

struct LogsTest : ApiTest {
  std::string id;

  void SetUp() override {
    make_project();
    register_definition();
    auto run = call(web, "POST", "/v1/projects/web/tasks", R"({"family":"frontend"})");
    id = std::string(run.body.at("tasks").as_array()[0].at("id").as_string());
  }

  // The task's container as the agent would have made it.
  std::string launched(const std::string& container) {
    runtime::ContainerSpec spec;
    spec.name = runtime::container_name(*model::TaskId::parse(id), container);
    spec.image = "alpine:3.20";
    spec.labels = {{std::string(runtime::label_task), id}, {std::string(runtime::label_container), container}};
    return images.add(spec, runtime::ContainerState::exited);
  }
};

}  // namespace

TEST_F(LogsTest, TheOnlyContainersLinesArePagedByTime) {
  const std::string container = launched("app");
  images.write_logs(container, {{runtime::LogStream::out, 1'000, "one", false},
                                {runtime::LogStream::err, 2'000, "two", false},
                                {runtime::LogStream::out, 3'000, std::string(10, 'x'), true}});
  auto r = call(grant_of({"louhi:web:read"}), "GET", "/v1/projects/web/tasks/" + id + "/logs", "", {{"tail", "2"}});
  ASSERT_EQ(r.status, 200u) << json::serialize(r.body);
  EXPECT_EQ(r.body.at("container"), "app");
  const auto& lines = r.body.at("lines").as_array();
  ASSERT_EQ(lines.size(), 2u) << "the last two";
  EXPECT_EQ(lines[0].at("stream"), "stderr");
  EXPECT_EQ(lines[0].at("text"), "two");
  EXPECT_EQ(lines[1].at("truncated"), true);
  EXPECT_FALSE(lines[0].as_object().contains("truncated"));
  EXPECT_EQ(r.body.at("next"), "3000");

  auto after = call(grant_of({"louhi:web:read"}), "GET", "/v1/projects/web/tasks/" + id + "/logs", "", {{"after", "1000"}});
  EXPECT_EQ(after.body.at("lines").as_array().size(), 2u);
  auto none = call(grant_of({"louhi:web:read"}), "GET", "/v1/projects/web/tasks/" + id + "/logs", "", {{"after", "3000"}});
  EXPECT_TRUE(none.body.at("lines").as_array().empty());
  EXPECT_FALSE(none.body.as_object().contains("next")) << "nothing new: the caller keeps its after";
}

TEST_F(LogsTest, AContainerIsNamedWhenTheTaskHasMore) {
  // The definition registered has one container; a revision of two needs a name.
  auto two = call(
      web, "POST", "/v1/projects/web/task-definitions",
      R"({"family":"pair","cpu_millis":250,"memory_mib":128,"containers":[{"name":"app","image":"alpine:3.20"},{"name":"side","image":"alpine:3.20","essential":false}]})");
  ASSERT_EQ(two.status, 200u) << json::serialize(two.body);
  auto run = call(web, "POST", "/v1/projects/web/tasks", R"({"family":"pair"})");
  id = std::string(run.body.at("tasks").as_array()[0].at("id").as_string());
  images.write_logs(launched("side"), {{runtime::LogStream::out, 5, "from the side", false}});
  launched("app");
  EXPECT_EQ(call(web, "GET", "/v1/projects/web/tasks/" + id + "/logs").status, 400u);
  auto side = call(web, "GET", "/v1/projects/web/tasks/" + id + "/logs", "", {{"container", "side"}});
  ASSERT_EQ(side.status, 200u);
  EXPECT_EQ(side.body.at("lines").as_array()[0].at("text"), "from the side");
  EXPECT_EQ(call(web, "GET", "/v1/projects/web/tasks/" + id + "/logs", "", {{"container", "Side"}}).status, 400u);
}

TEST_F(LogsTest, AContainerNotHereIsNotFoundAndBoundsAreKept) {
  EXPECT_EQ(call(web, "GET", "/v1/projects/web/tasks/" + id + "/logs").status, 404u) << "never launched, or removed";
  launched("app");
  for (const char* tail : {"0", "401", "x"}) EXPECT_EQ(call(web, "GET", "/v1/projects/web/tasks/" + id + "/logs", "", {{"tail", tail}}).status, 400u) << tail;
  EXPECT_EQ(call(web, "GET", "/v1/projects/web/tasks/" + id + "/logs", "", {{"after", "-1"}}).status, 400u);
  EXPECT_EQ(call(grant_of({"louhi:shop:read"}), "GET", "/v1/projects/web/tasks/" + id + "/logs").status, 403u);
}

TEST_F(LogsTest, ATaskOnAnotherNodeIsAskedThere) {
  auto task = model::decode_task(*store.get(model::key([&] {
    model::Task probe;
    probe.id = *model::TaskId::parse(id);
    probe.project = "web";
    return probe;
  }())));
  task.node = test::node_id(0x77);
  put_raw(model::key(task), model::encode(task));
  auto r = call(web, "GET", "/v1/projects/web/tasks/" + id + "/logs");
  EXPECT_EQ(r.status, 409u);
  EXPECT_EQ(code(r), "elsewhere");
  EXPECT_NE(std::string(r.body.at("message").as_string()).find(dannan::identity::to_string(test::node_id(0x77))), std::string::npos);
}
