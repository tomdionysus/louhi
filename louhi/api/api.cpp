//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/api/api.h"

#include <algorithm>
#include <boost/asio/post.hpp>
#include <boost/json.hpp>
#include <limits>
#include <memory>
#include <optional>
#include <set>

#include "louhi/api/json.h"
#include "louhi/events/events.h"
#include "louhi/model/codec.h"
#include "louhi/model/keys.h"
#include "louhi/model/names.h"

namespace louhi::api {

namespace json = boost::json;

namespace {

// The ten digits of a revision in a key.
constexpr size_t revision_digits = 10;

std::vector<std::string> segments_of(std::string_view path) {
  std::vector<std::string> out;
  if (path.empty() || path.front() != '/') return out;
  path.remove_prefix(1);
  while (!path.empty()) {
    const auto slash = path.find('/');
    out.emplace_back(path.substr(0, slash));
    if (slash == std::string_view::npos) break;
    path.remove_prefix(slash + 1);
  }
  return out;
}

std::optional<uint32_t> revision_of(std::string_view text) {
  if (text.empty() || text.size() > revision_digits) return std::nullopt;
  uint64_t n = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return std::nullopt;
    n = n * 10 + static_cast<uint64_t>(c - '0');
  }
  if (n == 0 || n > std::numeric_limits<uint32_t>::max()) return std::nullopt;
  return static_cast<uint32_t>(n);
}

Response forbidden(const std::string& scope) { return error_response(403, "forbidden", "this needs the scope " + scope); }

Response not_found(const std::string& what) { return error_response(404, "not_found", what + " does not exist"); }

std::string scope_of(std::string_view project, Action action) { return "louhi:" + std::string(project) + ":" + action_name(action); }

// A list's page: at most `limit` from after `next`.
struct Page {
  size_t limit = Api::max_list;
  std::string next;
};

Page page_of(const Request& request) {
  Page out;
  if (const auto it = request.query.find("limit"); it != request.query.end()) {
    const auto n = revision_of(it->second);
    if (!n || *n > Api::max_list) throw Invalid("limit is not 1 to " + std::to_string(Api::max_list));
    out.limit = *n;
  }
  if (const auto it = request.query.find("next"); it != request.query.end()) {
    if (it->second.size() > 64) throw Invalid("next is longer than 64 bytes");
    out.next = it->second;
  }
  return out;
}

// The last segment of a key.
std::string last_segment(const std::string& key) { return key.substr(key.rfind('/') + 1); }

}  // namespace

struct Api::Path {
  std::vector<std::string> segments;
  std::string project;
  std::string family;
  uint32_t revision = 0;
  model::TaskId task;
};

struct Api::Usage {
  uint64_t tasks = 0;
  uint64_t cpu_millis = 0;
  uint64_t memory_mib = 0;
};

Response error_response(unsigned status, std::string_view code, std::string_view message) {
  return {status, json::object{{"error", json::string(code)}, {"message", json::string(message)}}};
}

Api::Api(store::Store& store, const dannan::contract::WallClock& wall, model::NodeId self, store::Executor executor, dannan::status::ScopedLogger logger,
         store::Deadline write_deadline, events::Events* events)
    : _store(store), _wall(wall), _self(self), _executor(std::move(executor)), _logger(std::move(logger)), _write_deadline(write_deadline), _events(events) {}

void Api::answer(Done& done, Response response) {
  boost::asio::post(_executor, [done = std::move(done), response = std::move(response)]() mutable { done(std::move(response)); });
}

void Api::write(const std::string& key, store::Bytes value, Response success, Done done) {
  _store.put(key, std::move(value), _write_deadline, _executor,
             [this, key, success = std::move(success), done = std::move(done)](runtime::Result<store::Done> written) mutable {
               if (written.ok()) return done(std::move(success));
               _logger.warn("writing " + key + ": " + written.error().message);
               done(error_response(503, "not_written", "the store did not take the write: " + written.error().message));
             });
}

void Api::handle(const Grant& grant, const Request& request, Done done) {
  // A handler takes `done` and may throw before it answers (a body that is
  // not valid); the refusal goes through this copy. Every handler throws
  // only before it hands `done` on, so a request is answered once.
  Done refuse = done;
  try {
    Path path;
    path.segments = segments_of(request.path);
    const auto& s = path.segments;
    const auto& m = request.method;
    const auto method_not_allowed = [&] { answer(done, error_response(405, "method_not_allowed", m + " is not allowed on " + request.path)); };
    if (s.size() < 2 || s[0] != "v1") return answer(done, not_found("the path"));

    if (s[1] == "nodes" && s.size() == 2) return m == "GET" ? list_nodes(grant, request, std::move(done)) : method_not_allowed();
    if (s[1] != "projects") return answer(done, not_found("the path"));
    if (s.size() == 2) return m == "GET" ? list_projects(grant, request, std::move(done)) : method_not_allowed();

    path.project = s[2];
    if (!model::is_name(path.project)) return answer(done, not_found("the project"));
    if (s.size() == 3) {
      if (m == "PUT") return put_project(grant, path, request, std::move(done));
      if (m == "GET") return get_project(grant, path, std::move(done));
      return method_not_allowed();
    }
    if (s[3] == "quota" && s.size() == 4) {
      if (m == "PUT") return put_quota(grant, path, request, std::move(done));
      if (m == "GET") return get_quota(grant, path, std::move(done));
      return method_not_allowed();
    }
    if (s[3] == "task-definitions") {
      if (s.size() == 4) {
        if (m == "POST") return register_task_definition(grant, path, request, std::move(done));
        if (m == "GET") return list_families(grant, path, request, std::move(done));
        return method_not_allowed();
      }
      path.family = s[4];
      if (!model::is_name(path.family)) return answer(done, not_found("the family"));
      if (s.size() == 5) return m == "GET" ? list_revisions(grant, path, request, std::move(done)) : method_not_allowed();
      const auto revision = revision_of(s[5]);
      if (!revision) return answer(done, not_found("the revision"));
      path.revision = *revision;
      if (s.size() == 6) return m == "GET" ? describe_task_definition(grant, path, std::move(done)) : method_not_allowed();
      return answer(done, not_found("the path"));
    }
    if (s[3] == "tasks") {
      if (s.size() == 4) {
        if (m == "POST") return run_task(grant, path, request, std::move(done));
        if (m == "GET") return list_tasks(grant, path, request, std::move(done));
        return method_not_allowed();
      }
      const auto task = model::TaskId::parse(s[4]);
      if (!task) return answer(done, not_found("the task"));
      path.task = *task;
      if (s.size() == 5) return m == "GET" ? describe_task(grant, path, std::move(done)) : method_not_allowed();
      if (s.size() == 6 && s[5] == "stop") return m == "POST" ? stop_task(grant, path, request, std::move(done)) : method_not_allowed();
    }
    return answer(done, not_found("the path"));
  } catch (const std::invalid_argument& error) {
    // Invalid from a body, or a record's own validate().
    answer(refuse, error_response(400, "invalid", error.what()));
  } catch (const std::exception& error) {
    _logger.error(std::string("answering ") + request.method + " " + request.path + ": " + error.what());
    answer(refuse, error_response(500, "internal", "the request failed inside Louhi"));
  }
}

void Api::put_project(const Grant& grant, const Path& path, const Request& request, Done done) {
  if (!grant.admin()) return answer(done, forbidden("louhi:admin"));
  const auto body = project_body(parse_body(request.body), path.project);
  model::Project project;
  project.name = path.project;
  project.description = body.description;
  project.created_unix_ms = _wall.unix_ms();
  // A project put again keeps when it was made.
  if (const auto existing = _store.get(model::key(project))) {
    try {
      project.created_unix_ms = model::decode_project(*existing).created_unix_ms;
    } catch (const model::DecodeError& bad) {
      _logger.warn("project " + project.name + " does not decode and is replaced: " + bad.what());
    }
  }
  project.validate();
  write(model::key(project), model::encode(project), {200, to_json(project)}, std::move(done));
}

void Api::list_projects(const Grant& grant, const Request& request, Done done) {
  const Page page = page_of(request);
  json::array projects;
  for (const auto& [key, bytes] : _store.list("project/")) {
    if (projects.size() == page.limit) break;
    const std::string name = last_segment(key);
    if (name <= page.next || !grant.holds(name)) continue;
    try {
      projects.push_back(to_json(model::decode_project(bytes)));
    } catch (const model::DecodeError& bad) {
      _logger.warn("skipping record " + key + ": " + bad.what());
    }
  }
  json::object out{{"projects", std::move(projects)}};
  if (out["projects"].as_array().size() == page.limit) out["next"] = out["projects"].as_array().back().as_object().at("name");
  answer(done, {200, std::move(out)});
}

void Api::get_project(const Grant& grant, const Path& path, Done done) {
  if (!grant.may(path.project, Action::read)) return answer(done, forbidden(scope_of(path.project, Action::read)));
  model::Project probe;
  probe.name = path.project;
  const auto bytes = _store.get(model::key(probe));
  if (!bytes) return answer(done, not_found("the project"));
  answer(done, {200, to_json(model::decode_project(*bytes))});
}

void Api::put_quota(const Grant& grant, const Path& path, const Request& request, Done done) {
  if (!grant.admin()) return answer(done, forbidden("louhi:admin"));
  model::Project probe;
  probe.name = path.project;
  if (!_store.get(model::key(probe))) return answer(done, not_found("the project"));
  auto quota = quota_body(parse_body(request.body), path.project);
  quota.updated_unix_ms = _wall.unix_ms();
  write(model::key(quota), model::encode(quota), {200, to_json(quota)}, std::move(done));
}

void Api::get_quota(const Grant& grant, const Path& path, Done done) {
  if (!grant.may(path.project, Action::read)) return answer(done, forbidden(scope_of(path.project, Action::read)));
  model::Quota probe;
  probe.project = path.project;
  const auto bytes = _store.get(model::key(probe));
  if (!bytes) return answer(done, not_found("the project's quota"));
  answer(done, {200, to_json(model::decode_quota(*bytes))});
}

uint32_t Api::highest_revision(const std::string& project, const std::string& family) const {
  uint32_t highest = 0;
  const auto stored = _store.list(model::task_definitions_prefix(project, family));
  if (!stored.empty()) highest = revision_of(last_segment(stored.back().first)).value_or(0);
  if (const auto it = _pending_revisions.find(model::task_definitions_prefix(project, family)); it != _pending_revisions.end())
    highest = std::max(highest, it->second);
  return highest;
}

void Api::register_task_definition(const Grant& grant, const Path& path, const Request& request, Done done) {
  if (!grant.may(path.project, Action::register_definitions)) return answer(done, forbidden(scope_of(path.project, Action::register_definitions)));
  auto definition = task_definition_body(parse_body(request.body), path.project);
  model::Project probe;
  probe.name = path.project;
  if (!_store.get(model::key(probe))) return answer(done, not_found("the project"));
  const std::string family_key = model::task_definitions_prefix(definition.project, definition.family);
  {
    dannan::contract::Lock lock(_admission);
    const uint32_t highest = highest_revision(definition.project, definition.family);
    if (highest == std::numeric_limits<uint32_t>::max()) return answer(done, error_response(409, "full", "the family has no revision left"));
    definition.revision = highest + 1;
    definition.registered_unix_ms = _wall.unix_ms();
    _pending_revisions[family_key] = definition.revision;
  }
  definition.validate();
  const uint32_t revision = definition.revision;
  write(model::key(definition), model::encode(definition), {200, to_json(definition)},
        [this, family_key, revision, done = std::move(done)](Response response) mutable {
          {
            dannan::contract::Lock lock(_admission);
            if (const auto it = _pending_revisions.find(family_key); it != _pending_revisions.end() && it->second == revision) _pending_revisions.erase(it);
          }
          done(std::move(response));
        });
}

void Api::list_families(const Grant& grant, const Path& path, const Request& request, Done done) {
  if (!grant.may(path.project, Action::read)) return answer(done, forbidden(scope_of(path.project, Action::read)));
  const Page page = page_of(request);
  const std::string prefix = "taskdef/" + path.project + "/";
  json::array families;
  std::string last;
  for (const auto& [key, bytes] : _store.list(prefix)) {
    const std::string family = key.substr(prefix.size(), key.find('/', prefix.size()) - prefix.size());
    if (family == last || family <= page.next) continue;
    if (families.size() == page.limit) break;
    families.emplace_back(family);
    last = family;
  }
  json::object out{{"families", families}};
  if (families.size() == page.limit) out["next"] = last;
  answer(done, {200, std::move(out)});
}

void Api::list_revisions(const Grant& grant, const Path& path, const Request& request, Done done) {
  if (!grant.may(path.project, Action::read)) return answer(done, forbidden(scope_of(path.project, Action::read)));
  const Page page = page_of(request);
  const uint32_t after = page.next.empty() ? 0 : revision_of(page.next).value_or(std::numeric_limits<uint32_t>::max());
  json::array revisions;
  for (const auto& [key, bytes] : _store.list(model::task_definitions_prefix(path.project, path.family))) {
    const uint32_t revision = revision_of(last_segment(key)).value_or(0);
    if (revision <= after) continue;
    if (revisions.size() == page.limit) break;
    try {
      revisions.push_back(to_json(model::decode_task_definition(bytes)));
    } catch (const model::DecodeError& bad) {
      _logger.warn("skipping record " + key + ": " + bad.what());
    }
  }
  json::object out{{"task_definitions", revisions}};
  if (revisions.size() == page.limit) out["next"] = std::to_string(revisions.back().as_object().at("revision").to_number<uint32_t>());
  answer(done, {200, std::move(out)});
}

void Api::describe_task_definition(const Grant& grant, const Path& path, Done done) {
  if (!grant.may(path.project, Action::read)) return answer(done, forbidden(scope_of(path.project, Action::read)));
  const auto bytes = _store.get(model::task_definition_key(path.project, path.family, path.revision));
  if (!bytes) return answer(done, not_found("the revision"));
  answer(done, {200, to_json(model::decode_task_definition(*bytes))});
}

Api::Usage Api::usage(const std::string& project) const {
  std::map<std::string, model::Task> tasks;
  for (const auto& [key, bytes] : _store.list(model::tasks_prefix(project))) {
    try {
      tasks.emplace(key, model::decode_task(bytes));
    } catch (const model::DecodeError& bad) {
      _logger.warn("skipping record " + key + ": " + bad.what());
    }
  }
  for (const auto& [key, task] : _pending_tasks)
    if (task.project == project) tasks.emplace(key, task);
  Usage out;
  for (const auto& [key, task] : tasks) {
    if (task.desired != model::DesiredState::running) continue;
    ++out.tasks;
    const auto bytes = _store.get(model::task_definition_key(task.project, task.family, task.revision));
    if (!bytes) continue;
    try {
      const auto definition = model::decode_task_definition(*bytes);
      out.cpu_millis += definition.cpu_millis;
      out.memory_mib += definition.memory_mib;
    } catch (const model::DecodeError&) {
    }
  }
  return out;
}

void Api::forget_tokens(uint64_t now) {
  const uint64_t life = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(client_token_life).count());
  for (auto it = _client_tokens.begin(); it != _client_tokens.end();) it = it->second.at_unix_ms + life <= now ? _client_tokens.erase(it) : std::next(it);
}

void Api::run_task(const Grant& grant, const Path& path, const Request& request, Done done) {
  if (!grant.may(path.project, Action::run)) return answer(done, forbidden(scope_of(path.project, Action::run)));
  const auto body = run_task_body(parse_body(request.body));
  model::Project probe;
  probe.name = path.project;
  if (!_store.get(model::key(probe))) return answer(done, not_found("the project"));

  std::vector<model::Task> made;
  {
    dannan::contract::Lock lock(_admission);
    const uint64_t now = _wall.unix_ms();
    forget_tokens(now);
    const auto token_key = std::make_pair(path.project, body.client_token);
    if (!body.client_token.empty()) {
      if (const auto it = _client_tokens.find(token_key); it != _client_tokens.end()) {
        json::array tasks;
        for (const auto& id : it->second.tasks) {
          model::Task t;
          t.id = id;
          t.project = path.project;
          const std::string key = model::key(t);
          if (const auto pending = _pending_tasks.find(key); pending != _pending_tasks.end())
            tasks.push_back(to_json(pending->second));
          else if (const auto bytes = _store.get(key))
            tasks.push_back(to_json(model::decode_task(*bytes)));
        }
        return answer(done, {200, json::object{{"tasks", std::move(tasks)}}});
      }
    }

    const uint32_t revision = body.revision.value_or(highest_revision(path.project, body.family));
    const auto definition_bytes = revision == 0 ? std::nullopt : _store.get(model::task_definition_key(path.project, body.family, revision));
    if (!definition_bytes) return answer(done, not_found("the revision"));
    const auto definition = model::decode_task_definition(*definition_bytes);

    model::Quota quota_probe;
    quota_probe.project = path.project;
    const auto quota_bytes = _store.get(model::key(quota_probe));
    if (!quota_bytes) return answer(done, error_response(409, "quota", "the project has no quota, so it may run nothing"));
    const auto quota = model::decode_quota(*quota_bytes);
    const Usage used = usage(path.project);
    const auto over = [&](uint64_t now_used, uint64_t each, uint32_t limit) { return now_used + each * body.count > limit; };
    if (over(used.tasks, 1, quota.max_tasks)) return answer(done, error_response(409, "quota", "this would pass the project's max_tasks"));
    if (over(used.cpu_millis, definition.cpu_millis, quota.max_cpu_millis))
      return answer(done, error_response(409, "quota", "this would pass the project's max_cpu_millis"));
    if (over(used.memory_mib, definition.memory_mib, quota.max_memory_mib))
      return answer(done, error_response(409, "quota", "this would pass the project's max_memory_mib"));

    std::vector<model::TaskId> ids;
    for (uint32_t i = 0; i < body.count; ++i) {
      model::Task task;
      task.id = model::TaskId::random();
      task.project = path.project;
      task.family = definition.family;
      task.revision = definition.revision;
      task.desired = model::DesiredState::running;
      // On this node until placement across nodes (step 6).
      task.node = _self;
      task.placement_epoch = 1;
      task.started_by = grant.subject().substr(0, model::max_principal);
      task.created_unix_ms = now;
      task.validate();
      _pending_tasks[model::key(task)] = task;
      ids.push_back(task.id);
      made.push_back(std::move(task));
    }
    if (!body.client_token.empty()) _client_tokens[token_key] = {std::move(ids), now};
  }

  // One answer when every write has completed; any that failed fails it.
  struct Gather {
    size_t left = 0;
    std::optional<Response> failed;
    json::array tasks;
    Done done;
  };
  auto gather = std::make_shared<Gather>();
  gather->left = made.size();
  gather->done = std::move(done);
  for (const auto& task : made) {
    const std::string key = model::key(task);
    gather->tasks.push_back(to_json(task));
    write(key, model::encode(task), {200, json::value()}, [this, key, task, gather](Response response) {
      {
        dannan::contract::Lock lock(_admission);
        _pending_tasks.erase(key);
      }
      if (response.status == 200 && _events) _events->task_run(task);
      if (response.status != 200 && !gather->failed) gather->failed = std::move(response);
      if (--gather->left > 0) return;
      if (gather->failed) return gather->done(std::move(*gather->failed));
      gather->done({200, json::object{{"tasks", std::move(gather->tasks)}}});
    });
  }
}

void Api::list_tasks(const Grant& grant, const Path& path, const Request& request, Done done) {
  if (!grant.may(path.project, Action::read)) return answer(done, forbidden(scope_of(path.project, Action::read)));
  const Page page = page_of(request);
  json::array tasks;
  std::string last;
  for (const auto& [key, bytes] : _store.list(model::tasks_prefix(path.project))) {
    const std::string id = last_segment(key);
    if (id <= page.next) continue;
    if (tasks.size() == page.limit) break;
    try {
      tasks.push_back(to_json(model::decode_task(bytes)));
      last = id;
    } catch (const model::DecodeError& bad) {
      _logger.warn("skipping record " + key + ": " + bad.what());
    }
  }
  json::object out{{"tasks", tasks}};
  if (tasks.size() == page.limit) out["next"] = last;
  answer(done, {200, std::move(out)});
}

void Api::describe_task(const Grant& grant, const Path& path, Done done) {
  if (!grant.may(path.project, Action::read)) return answer(done, forbidden(scope_of(path.project, Action::read)));
  model::Task probe;
  probe.id = path.task;
  probe.project = path.project;
  const auto bytes = _store.get(model::key(probe));
  if (!bytes) return answer(done, not_found("the task"));
  json::object out = to_json(model::decode_task(*bytes));
  json::array statuses;
  for (const auto& [key, status] : _store.list(model::task_statuses_prefix(path.task))) {
    try {
      statuses.push_back(to_json(model::decode_task_status(status)));
    } catch (const model::DecodeError& bad) {
      _logger.warn("skipping record " + key + ": " + bad.what());
    }
  }
  out["statuses"] = std::move(statuses);
  answer(done, {200, std::move(out)});
}

void Api::stop_task(const Grant& grant, const Path& path, const Request& request, Done done) {
  if (!grant.may(path.project, Action::run)) return answer(done, forbidden(scope_of(path.project, Action::run)));
  const auto body = stop_task_body(parse_body(request.body));
  model::Task probe;
  probe.id = path.task;
  probe.project = path.project;
  const auto bytes = _store.get(model::key(probe));
  if (!bytes) return answer(done, not_found("the task"));
  auto task = model::decode_task(*bytes);
  // Stopping a stopped task changes nothing and succeeds.
  if (task.desired == model::DesiredState::stopped) return answer(done, {200, to_json(task)});
  task.desired = model::DesiredState::stopped;
  task.stop_reason = body.reason.empty() ? "stopped by " + grant.subject().substr(0, 256) : body.reason;
  task.validate();
  write(model::key(task), model::encode(task), {200, to_json(task)}, [this, task, done = std::move(done)](Response response) {
    if (response.status == 200 && _events) _events->task_stop_requested(task);
    done(std::move(response));
  });
}

void Api::list_nodes(const Grant& grant, const Request& request, Done done) {
  if (!grant.admin()) return answer(done, forbidden("louhi:admin"));
  const Page page = page_of(request);
  json::array nodes;
  std::string last;
  for (const auto& [key, bytes] : _store.list("node/")) {
    const std::string id = last_segment(key);
    if (id <= page.next) continue;
    if (nodes.size() == page.limit) break;
    try {
      nodes.push_back(to_json(model::decode_node_status(bytes)));
      last = id;
    } catch (const model::DecodeError& bad) {
      _logger.warn("skipping record " + key + ": " + bad.what());
    }
  }
  json::object out{{"nodes", nodes}};
  if (nodes.size() == page.limit) out["next"] = last;
  answer(done, {200, std::move(out)});
}

}  // namespace louhi::api
