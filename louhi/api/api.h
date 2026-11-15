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
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/status/log.h"
#include "louhi/api/auth.h"
#include "louhi/model/ids.h"
#include "louhi/store/store.h"

// The API's operations (design, "API and sign-on", the endpoint table), apart
// from HTTP so tests drive them directly: a request already signed in (its
// Grant) and parsed into a method, a path, a query and a body, answered with
// a status and a JSON body.
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

  // The store and the clock outlive this. `self` is this node: tasks are
  // placed on it until placement across nodes exists (step 6). `events`,
  // when given, outlives this and is told of each task run and each stop
  // asked for, once its write has landed.
  Api(store::Store& store, const dannan::contract::WallClock& wall, model::NodeId self, store::Executor executor, dannan::status::ScopedLogger logger,
      store::Deadline write_deadline = store::Deadline(10'000), events::Events* events = nullptr);

  void handle(const Grant& grant, const Request& request, std::function<void(Response)> done);

 private:
  struct Path;
  struct Usage;
  using Done = std::function<void(Response)>;

  void answer(Done& done, Response response);
  void write(const std::string& key, store::Bytes value, Response success, Done done);

  void put_project(const Grant& grant, const Path& path, const Request& request, Done done);
  void list_projects(const Grant& grant, const Request& request, Done done);
  void get_project(const Grant& grant, const Path& path, Done done);
  void put_quota(const Grant& grant, const Path& path, const Request& request, Done done);
  void get_quota(const Grant& grant, const Path& path, Done done);
  void register_task_definition(const Grant& grant, const Path& path, const Request& request, Done done);
  void list_families(const Grant& grant, const Path& path, const Request& request, Done done);
  void list_revisions(const Grant& grant, const Path& path, const Request& request, Done done);
  void describe_task_definition(const Grant& grant, const Path& path, Done done);
  void run_task(const Grant& grant, const Path& path, const Request& request, Done done);
  void list_tasks(const Grant& grant, const Path& path, const Request& request, Done done);
  void describe_task(const Grant& grant, const Path& path, Done done);
  void stop_task(const Grant& grant, const Path& path, const Request& request, Done done);
  void list_nodes(const Grant& grant, const Request& request, Done done);

  // The project's usage from the store and from what this node admitted that
  // the store does not show yet. Called with _admission held.
  Usage usage(const std::string& project) const DANNAN_REQUIRES(_admission);
  uint32_t highest_revision(const std::string& project, const std::string& family) const DANNAN_REQUIRES(_admission);
  void forget_tokens(uint64_t now) DANNAN_REQUIRES(_admission);

  store::Store& _store;
  const dannan::contract::WallClock& _wall;
  const model::NodeId _self;
  store::Executor _executor;
  dannan::status::ScopedLogger _logger;
  const store::Deadline _write_deadline;
  events::Events* const _events;

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
