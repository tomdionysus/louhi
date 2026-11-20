//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// The API's routes as data (design, the endpoint table): the one list the
// Api dispatches from, checks scopes from and writes its OpenAPI document
// from, so the three cannot disagree.
//
// A path is a template: "/v1/" and segments, each a literal or a parameter
// in braces. The parameters are {project} and {family} (names), {revision}
// (1 and up) and {task} (a task id); a value that is not one is not found.
namespace louhi::api {

// Who may call a route: any signed-in token, louhi:admin, or the action in
// the path's project.
enum class Scope : uint8_t { any = 1, admin = 2, read = 3, register_definitions = 4, run = 5 };

// What a route does; the Api's dispatch switches on it.
enum class Operation : uint8_t {
  put_project = 1,
  list_projects,
  get_project,
  put_quota,
  get_quota,
  register_task_definition,
  list_families,
  list_revisions,
  describe_task_definition,
  run_task,
  list_tasks,
  describe_task,
  stop_task,
  list_nodes,
  status,
  openapi,
};

struct Route {
  std::string_view method;
  std::string_view path;
  Scope scope;
  Operation operation;
  // The OpenAPI operationId: AWS's name where there is one.
  std::string_view operation_id;
  std::string_view summary;
};

// Every route, in the order dispatch tries them.
std::span<const Route> routes() noexcept;

// The scope a route needs, as a token carries it: "louhi:admin",
// "louhi:<project>:read" and so on; empty for any.
std::string scope_text(Scope scope, std::string_view project);

}  // namespace louhi::api
