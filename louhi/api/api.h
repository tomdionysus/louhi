//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/json/value.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/status/log.h"
#include "louhi/api/auth.h"
#include "louhi/api/routes.h"
#include "louhi/model/ids.h"
#include "louhi/model/records.h"
#include "louhi/runtime/driver.h"
#include "louhi/store/store.h"

// The API's operations (design, "API and sign-on", the endpoint table), apart
// from HTTP so tests drive them directly: a request already signed in (its
// Grant) and parsed into a method, a path, a query and a body, answered with
// a status and a JSON body.
//
// Which operation a request is, and the scope it needs, come from the route
// table (routes.h): a path that fits no route is not found, a parameter that
// is not good is not found, a method the path does not take is not allowed,
// and then the route's scope is checked before anything about the project is
// said.
//
// Reads are of this node's copy of the store. A write completes when the
// store says it has, or with 503 when it fails or its deadline passes.
//
// What is decided on this node alone until agreement (Dannan 0.9.0, step 6):
// a family's next revision, and admission against a project's quota. Both are
// serialised on this node, counting what it has admitted but the store has
// not yet shown, so this node never over-admits; two nodes deciding at once
// can, and the design bounds that (Projects and quotas).
//
// A task is also admitted against this node's capacity: refused (409,
// "capacity") when it would not fit beside what every task placed here holds,
// of any project, and refused (503) until the node has reported what it has.
// A refusal, not a queue. A task run on its own that its node reports stopped
// holds nothing (model::finished).
//
// The executor rule: `done` is posted to the executor given at construction,
// exactly once, never called inline. Thread-safe. The Api outlives every
// request it was handed until that request's `done` has run.
namespace louhi::events {
class Events;
}

namespace louhi::api {

struct Request {
  std::string method;
  // The path alone, percent escapes left as sent.
  std::string path;
  // The query's parameters, as sent.
  std::map<std::string, std::string> query;
  std::string body;
};

struct Response {
  unsigned status = 200;
  boost::json::value body;
};

// The error body: {"error": code, "message": text}.
Response error_response(unsigned status, std::string_view code, std::string_view message);

class Api {
 public:
  // The most a list answers at once.
  static constexpr size_t max_list = 100;
  // How long a RunTask client token is remembered.
  static constexpr std::chrono::hours client_token_life{1};

  // How long registering a definition waits on the registries to resolve its
  // images.
  static constexpr runtime::Deadline resolve_deadline{30'000};
  // The most log lines one GetTaskLogs answers, and how long it waits on the
  // runtime.
  static constexpr uint32_t max_log_lines = 400;
  static constexpr runtime::Deadline logs_deadline{30'000};

  // The store, the clock and the runtime outlive this. `self` is this node:
  // tasks are placed on it until placement across nodes exists (step 6).
  // `images` resolves each container's image to a digest when a definition
  // is registered (design, "Images in detail"), and refuses a registry the
  // operator does not allow. `events`, when given, outlives this and is told
  // of each task run and each stop asked for, once its write has landed.
  Api(store::Store& store, const dannan::contract::WallClock& wall, model::NodeId self, runtime::Driver& images, store::Executor executor,
      dannan::status::ScopedLogger logger, store::Deadline write_deadline = store::Deadline(10'000), events::Events* events = nullptr);

  void handle(const Grant& grant, const Request& request, std::function<void(Response)> done);

 private:
  struct Path;
  struct Usage;
  using Done = std::function<void(Response)>;

  void answer(Done& done, Response response);
  void write(const std::string& key, store::Bytes value, Response success, Done done);

  // Each route's handler; its scope was checked before it is called.
  void put_project(const Path& path, const Request& request, Done done);
  void list_projects(const Grant& grant, const Request& request, Done done);
  void get_project(const Path& path, Done done);
  void put_quota(const Path& path, const Request& request, Done done);
  void get_quota(const Path& path, Done done);
  void register_task_definition(const Path& path, const Request& request, Done done);
  // Resolves containers[next..] of `definition` in turn, then admits it.
  void resolve_images(std::shared_ptr<model::TaskDefinition> definition, size_t next, Done done);
  void admit_task_definition(model::TaskDefinition definition, Done done);
  void list_families(const Path& path, const Request& request, Done done);
  void list_revisions(const Path& path, const Request& request, Done done);
  void describe_task_definition(const Path& path, Done done);
  void run_task(const Grant& grant, const Path& path, const Request& request, Done done);
  void list_tasks(const Path& path, const Request& request, Done done);
  void describe_task(const Path& path, Done done);
  void stop_task(const Grant& grant, const Path& path, const Request& request, Done done);
  // What one of a task's containers wrote, from this node's runtime: the
  // task's node only (409, elsewhere, naming it, from any other), while its
  // containers are kept (404 once retention has removed them).
  void get_task_logs(const Path& path, const Request& request, Done done);
  void list_nodes(const Request& request, Done done);
  // This node as it sees itself: its last report and the store's state, the
  // report's detail for louhi:admin only.
  void get_status(const Grant& grant, Done done);

  // The project's usage from the store and from what this node admitted that
  // the store does not show yet. Called with _admission held.
  Usage usage(const std::string& project) const DANNAN_REQUIRES(_admission);
  uint32_t highest_revision(const std::string& project, const std::string& family) const DANNAN_REQUIRES(_admission);
  // What the tasks placed on this node hold, of every project, with what this
  // node admitted that the store does not show yet. Called with _admission
  // held.
  Usage node_usage() const DANNAN_REQUIRES(_admission);
  // The task's status as the node it is placed on last reported it.
  std::optional<model::TaskStatus> status_on_its_node(const model::Task& task) const;
  void forget_tokens(uint64_t now) DANNAN_REQUIRES(_admission);

  store::Store& _store;
  const dannan::contract::WallClock& _wall;
  const model::NodeId _self;
  runtime::Driver& _images;
  store::Executor _executor;
  dannan::status::ScopedLogger _logger;
  const store::Deadline _write_deadline;
  events::Events* const _events;
  // GET /v1/openapi.json's answer, built once.
  const boost::json::value _openapi;

  mutable dannan::contract::Mutex _admission;
  // What this node admitted whose write has not completed, by key.
  std::map<std::string, model::Task> _pending_tasks DANNAN_GUARDED_BY(_admission);
  std::map<std::string, uint32_t> _pending_revisions DANNAN_GUARDED_BY(_admission);
  struct Remembered {
    std::vector<model::TaskId> tasks;
    uint64_t at_unix_ms = 0;
  };
  // By project and client token.
  std::map<std::pair<std::string, std::string>, Remembered> _client_tokens DANNAN_GUARDED_BY(_admission);
};

}  // namespace louhi::api
