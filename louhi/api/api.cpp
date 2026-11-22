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
#include "louhi/api/openapi.h"
#include "louhi/events/events.h"
#include "louhi/model/codec.h"
#include "louhi/model/keys.h"
#include "louhi/model/names.h"
#include "louhi/version.h"

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

// Whether a path's segments fit a route's template: as many, and each
// literal the same.
bool fits(std::string_view route, const std::vector<std::string>& segments) {
  const auto parts = segments_of(route);
  if (parts.size() != segments.size()) return false;
  for (size_t i = 0; i < parts.size(); ++i)
    if (parts[i].front() != '{' && parts[i] != segments[i]) return false;
  return true;
}

bool permitted(const Grant& grant, Scope scope, std::string_view project) {
  switch (scope) {
    case Scope::any:
      return true;
    case Scope::admin:
      return grant.admin();
    case Scope::read:
      return grant.may(project, Action::read);
    case Scope::register_definitions:
      return grant.may(project, Action::register_definitions);
    case Scope::run:
      return grant.may(project, Action::run);
  }
  return false;
}

// The last segment of a key.
std::string last_segment(const std::string& key) { return key.substr(key.rfind('/') + 1); }

}  // namespace

struct Api::Path {
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

Api::Api(store::Store& store, const dannan::contract::WallClock& wall, model::NodeId self, runtime::Driver& images, store::Executor executor,
         dannan::status::ScopedLogger logger, store::Deadline write_deadline, events::Events* events)
    : _store(store),
      _wall(wall),
      _self(self),
      _images(images),
      _executor(std::move(executor)),
      _logger(std::move(logger)),
      _write_deadline(write_deadline),
      _events(events),
      _openapi(openapi_document()) {}

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
    const auto segments = segments_of(request.path);
    // The routes this path fits; routes of one path share their template.
    const Route* fitted = nullptr;
    const Route* route = nullptr;
    for (const Route& candidate : routes()) {
      if (!fits(candidate.path, segments)) continue;
      if (!fitted) fitted = &candidate;
      if (candidate.method == request.method) {
        route = &candidate;
        break;
      }
    }
    if (!fitted) return answer(done, not_found("the path"));

    Path path;
    const auto parameters = segments_of(fitted->path);
    for (size_t i = 0; i < parameters.size(); ++i) {
      const std::string& name = parameters[i];
      const std::string& value = segments[i];
      if (name == "{project}") {
        if (!model::is_name(value)) return answer(done, not_found("the project"));
        path.project = value;
      } else if (name == "{family}") {
        if (!model::is_name(value)) return answer(done, not_found("the family"));
        path.family = value;
      } else if (name == "{revision}") {
        const auto revision = revision_of(value);
        if (!revision) return answer(done, not_found("the revision"));
        path.revision = *revision;
      } else if (name == "{task}") {
        const auto task = model::TaskId::parse(value);
        if (!task) return answer(done, not_found("the task"));
        path.task = *task;
      }
    }
    if (!route) return answer(done, error_response(405, "method_not_allowed", request.method + " is not allowed on " + request.path));
    if (!permitted(grant, route->scope, path.project)) return answer(done, forbidden(scope_text(route->scope, path.project)));

    switch (route->operation) {
      case Operation::put_project:
        return put_project(path, request, std::move(done));
      case Operation::list_projects:
        return list_projects(grant, request, std::move(done));
      case Operation::get_project:
        return get_project(path, std::move(done));
      case Operation::put_quota:
        return put_quota(path, request, std::move(done));
      case Operation::get_quota:
        return get_quota(path, std::move(done));
      case Operation::register_task_definition:
        return register_task_definition(path, request, std::move(done));
      case Operation::list_families:
        return list_families(path, request, std::move(done));
      case Operation::list_revisions:
        return list_revisions(path, request, std::move(done));
      case Operation::describe_task_definition:
        return describe_task_definition(path, std::move(done));
      case Operation::run_task:
        return run_task(grant, path, request, std::move(done));
      case Operation::list_tasks:
        return list_tasks(path, request, std::move(done));
      case Operation::describe_task:
        return describe_task(path, std::move(done));
      case Operation::stop_task:
        return stop_task(grant, path, request, std::move(done));
      case Operation::list_nodes:
        return list_nodes(request, std::move(done));
      case Operation::status:
        return get_status(grant, std::move(done));
      case Operation::openapi:
        return answer(done, {200, _openapi});
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

void Api::put_project(const Path& path, const Request& request, Done done) {
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

void Api::get_project(const Path& path, Done done) {
  model::Project probe;
  probe.name = path.project;
  const auto bytes = _store.get(model::key(probe));
  if (!bytes) return answer(done, not_found("the project"));
  answer(done, {200, to_json(model::decode_project(*bytes))});
}

void Api::put_quota(const Path& path, const Request& request, Done done) {
  model::Project probe;
  probe.name = path.project;
  if (!_store.get(model::key(probe))) return answer(done, not_found("the project"));
  auto quota = quota_body(parse_body(request.body), path.project);
  quota.updated_unix_ms = _wall.unix_ms();
  write(model::key(quota), model::encode(quota), {200, to_json(quota)}, std::move(done));
}

void Api::get_quota(const Path& path, Done done) {
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

void Api::register_task_definition(const Path& path, const Request& request, Done done) {
  auto definition = std::make_shared<model::TaskDefinition>(task_definition_body(parse_body(request.body), path.project));
  model::Project probe;
  probe.name = path.project;
  if (!_store.get(model::key(probe))) return answer(done, not_found("the project"));
  // A definition has at least one container, so the answer always comes from
  // a posted completion.
  resolve_images(std::move(definition), 0, std::move(done));
}

void Api::resolve_images(std::shared_ptr<model::TaskDefinition> definition, size_t next, Done done) {
  if (next == definition->containers.size()) return admit_task_definition(std::move(*definition), std::move(done));
  const std::string image = definition->containers[next].image;
  _images.resolve(image, resolve_deadline, _executor, [this, definition, next, image, done = std::move(done)](runtime::Result<std::string> got) mutable {
    if (!got.ok()) {
      const runtime::Error& error = got.error();
      _logger.info("resolving " + image + ": " + error.message);
      if (error.kind == runtime::Error::Kind::refused) return done(error_response(400, "image_refused", error.message));
      if (error.kind == runtime::Error::Kind::not_found) return done(error_response(400, "image_not_found", error.message));
      return done(error_response(503, "image_unresolved", error.message));
    }
    definition->containers[next].image_digest = got.value();
    resolve_images(std::move(definition), next + 1, std::move(done));
  });
}

void Api::admit_task_definition(model::TaskDefinition definition, Done done) {
  const std::string family_key = model::task_definitions_prefix(definition.project, definition.family);
  {
    dannan::contract::Lock lock(_admission);
    const uint32_t highest = highest_revision(definition.project, definition.family);
    if (highest == std::numeric_limits<uint32_t>::max()) return answer(done, error_response(409, "full", "the family has no revision left"));
    definition.revision = highest + 1;
    definition.registered_unix_ms = _wall.unix_ms();
    _pending_revisions[family_key] = definition.revision;
  }
  // Runs in a completion, where nothing may throw: a definition that does not
  // validate with its digests is answered here.
  store::Bytes bytes;
  try {
    bytes = model::encode(definition);
  } catch (const std::exception& error) {
    {
      dannan::contract::Lock lock(_admission);
      if (const auto it = _pending_revisions.find(family_key); it != _pending_revisions.end() && it->second == definition.revision)
        _pending_revisions.erase(it);
    }
    _logger.error(std::string("registering a definition: ") + error.what());
    return answer(done, error_response(500, "internal", error.what()));
  }
  const uint32_t revision = definition.revision;
  write(model::key(definition), std::move(bytes), {200, to_json(definition)}, [this, family_key, revision, done = std::move(done)](Response response) mutable {
    {
      dannan::contract::Lock lock(_admission);
      if (const auto it = _pending_revisions.find(family_key); it != _pending_revisions.end() && it->second == revision) _pending_revisions.erase(it);
    }
    done(std::move(response));
  });
}

void Api::list_families(const Path& path, const Request& request, Done done) {
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

void Api::list_revisions(const Path& path, const Request& request, Done done) {
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

void Api::describe_task_definition(const Path& path, Done done) {
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
    if (!model::holds_resources(task, status_on_its_node(task))) continue;
    ++out.tasks;
    const auto bytes = _store.get(model::task_definition_key(task.project, task.family, task.revision));
    if (!bytes) continue;
    try {
      const auto definition = model::decode_task_definition(*bytes);
      out.cpu_millis += definition.cpu_millis;
      out.memory_mib += definition.memory_mib;
    } catch (const model::DecodeError& bad) {
      // Counted as a task, its resources unknown: said, never silent.
      _logger.warn("task " + key + " runs a revision that does not decode, so its CPU and memory are not counted: " + bad.what());
    }
  }
  return out;
}

Api::Usage Api::node_usage() const {
  std::map<std::string, model::Task> tasks;
  for (const auto& [key, bytes] : _store.list(model::tasks_prefix(""))) {
    try {
      auto task = model::decode_task(bytes);
      if (task.node == _self) tasks.emplace(key, std::move(task));
    } catch (const model::DecodeError& bad) {
      _logger.warn("skipping record " + key + ": " + bad.what());
    }
  }
  for (const auto& [key, task] : _pending_tasks)
    if (task.node == _self) tasks.emplace(key, task);
  Usage out;
  for (const auto& [key, task] : tasks) {
    if (!model::holds_resources(task, status_on_its_node(task))) continue;
    ++out.tasks;
    const auto bytes = _store.get(model::task_definition_key(task.project, task.family, task.revision));
    if (!bytes) continue;
    try {
      const auto definition = model::decode_task_definition(*bytes);
      out.cpu_millis += definition.cpu_millis;
      out.memory_mib += definition.memory_mib;
    } catch (const model::DecodeError& bad) {
      _logger.warn("task " + key + " runs a revision that does not decode, so its CPU and memory are not counted here: " + bad.what());
    }
  }
  return out;
}

std::optional<model::TaskStatus> Api::status_on_its_node(const model::Task& task) const {
  if (!task.node) return std::nullopt;
  model::TaskStatus probe;
  probe.task = task.id;
  probe.node = *task.node;
  const auto bytes = _store.get(model::key(probe));
  if (!bytes) return std::nullopt;
  try {
    return model::decode_task_status(*bytes);
  } catch (const model::DecodeError& bad) {
    // Counted as unfinished: holding too much is safe, too little is not.
    _logger.warn("task " + model::to_string(task.id) + ": its node's status does not decode: " + bad.what());
    return std::nullopt;
  }
}

void Api::forget_tokens(uint64_t now) {
  const uint64_t life = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(client_token_life).count());
  for (auto it = _client_tokens.begin(); it != _client_tokens.end();) it = it->second.at_unix_ms + life <= now ? _client_tokens.erase(it) : std::next(it);
}

void Api::run_task(const Grant& grant, const Path& path, const Request& request, Done done) {
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

    // This node's room, from its own last report of what it has.
    model::NodeStatus self_probe;
    self_probe.node = _self;
    const auto report_bytes = _store.get(model::key(self_probe));
    if (!report_bytes) return answer(done, error_response(503, "capacity_unknown", "this node has not yet reported what it can hold"));
    const auto report = model::decode_node_status(*report_bytes);
    const Usage here = node_usage();
    if (here.cpu_millis + uint64_t{definition.cpu_millis} * body.count > report.total.cpu_millis)
      return answer(done, error_response(409, "capacity", "this node lacks the CPU for this beside what it already holds"));
    if (here.memory_mib + uint64_t{definition.memory_mib} * body.count > report.total.memory_mib)
      return answer(done, error_response(409, "capacity", "this node lacks the memory for this beside what it already holds"));

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

void Api::list_tasks(const Path& path, const Request& request, Done done) {
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

void Api::describe_task(const Path& path, Done done) {
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

void Api::list_nodes(const Request& request, Done done) {
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

void Api::get_status(const Grant& grant, Done done) {
  model::NodeStatus probe;
  probe.node = _self;
  std::optional<model::NodeStatus> report;
  if (const auto bytes = _store.get(model::key(probe))) {
    try {
      report = model::decode_node_status(*bytes);
    } catch (const model::DecodeError& bad) {
      _logger.warn("this node's status does not decode: " + std::string(bad.what()));
    }
  }
  json::object out{{"node", dannan::identity::to_string(_self)},
                   {"version", kVersion},
                   {"ready", report && report->ready},
                   {"store", json::object{{"ready", _store.ready()}}}};
  if (grant.admin()) {
    out["tasks"] = report ? report->tasks : 0u;
    out["report"] = report ? json::value(to_json(*report)) : json::value(nullptr);
  }
  answer(done, {200, std::move(out)});
}

}  // namespace louhi::api
