//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/api/routes.h"

#include "louhi/api/auth.h"

namespace louhi::api {

namespace {

constexpr Route table[] = {
    {"GET", "/v1/status", Scope::any, Operation::status, "GetStatus", "This node as it sees itself"},
    {"GET", "/v1/openapi.json", Scope::any, Operation::openapi, "GetOpenApi", "This document"},
    {"GET", "/v1/nodes", Scope::admin, Operation::list_nodes, "ListNodes", "Every node's last status"},
    {"GET", "/v1/projects", Scope::any, Operation::list_projects, "ListProjects", "The projects the token holds a scope in, all for admin"},
    {"PUT", "/v1/projects/{project}", Scope::admin, Operation::put_project, "PutProject", "Make a project, or change its description"},
    {"GET", "/v1/projects/{project}", Scope::read, Operation::get_project, "GetProject", "One project"},
    {"PUT", "/v1/projects/{project}/quota", Scope::admin, Operation::put_quota, "PutQuota", "Set a project's limits"},
    {"GET", "/v1/projects/{project}/quota", Scope::read, Operation::get_quota, "GetQuota", "A project's limits"},
    {"POST", "/v1/projects/{project}/task-definitions", Scope::register_definitions, Operation::register_task_definition, "RegisterTaskDefinition",
     "Register a family's next revision"},
    {"GET", "/v1/projects/{project}/task-definitions", Scope::read, Operation::list_families, "ListTaskDefinitionFamilies", "A project's families"},
    {"GET", "/v1/projects/{project}/task-definitions/{family}", Scope::read, Operation::list_revisions, "ListTaskDefinitions", "A family's revisions"},
    {"GET", "/v1/projects/{project}/task-definitions/{family}/{revision}", Scope::read, Operation::describe_task_definition, "DescribeTaskDefinition",
     "One revision"},
    {"POST", "/v1/projects/{project}/tasks", Scope::run, Operation::run_task, "RunTask", "Run tasks of a revision"},
    {"GET", "/v1/projects/{project}/tasks", Scope::read, Operation::list_tasks, "ListTasks", "A project's tasks"},
    {"GET", "/v1/projects/{project}/tasks/{task}", Scope::read, Operation::describe_task, "DescribeTasks", "A task and every node's status of it"},
    {"POST", "/v1/projects/{project}/tasks/{task}/stop", Scope::run, Operation::stop_task, "StopTask", "Ask a task to stop"},
    {"GET", "/v1/projects/{project}/tasks/{task}/logs", Scope::read, Operation::get_task_logs, "GetTaskLogs", "What one of a task's containers wrote"},
};

}  // namespace

std::span<const Route> routes() noexcept { return table; }

std::string scope_text(Scope scope, std::string_view project) {
  const auto in_project = [&](Action action) { return "louhi:" + std::string(project) + ":" + action_name(action); };
  switch (scope) {
    case Scope::any:
      return "";
    case Scope::admin:
      return "louhi:admin";
    case Scope::read:
      return in_project(Action::read);
    case Scope::register_definitions:
      return in_project(Action::register_definitions);
    case Scope::run:
      return in_project(Action::run);
  }
  return "louhi:admin";
}

}  // namespace louhi::api
