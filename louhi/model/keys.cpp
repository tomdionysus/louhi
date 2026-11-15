//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/model/keys.h"

#include <cstdio>

#include "louhi/model/names.h"

namespace louhi::model {

namespace {

std::string revision_text(uint32_t revision) {
  char text[11];
  std::snprintf(text, sizeof text, "%010u", static_cast<unsigned>(revision));
  return text;
}

std::string named(std::string_view what, std::string_view name) {
  require_name(what, name);
  return std::string(name);
}

}  // namespace

std::string key(const Project& record) { return "project/" + named("project", record.name); }

std::string key(const Quota& record) { return "quota/" + named("project", record.project); }

std::string key(const TaskDefinition& record) { return task_definitions_prefix(record.project, record.family) + revision_text(record.revision); }

std::string key(const Service& record) { return services_prefix(record.project) + named("service", record.name); }

std::string key(const Task& record) { return tasks_prefix(record.project) + to_string(record.id); }

std::string key(const TaskStatus& record) { return task_statuses_prefix(record.task) + dannan::identity::to_string(record.node); }

std::string key(const NodeStatus& record) { return "node/" + dannan::identity::to_string(record.node); }

std::string task_definition_key(std::string_view project, std::string_view family, uint32_t revision) {
  return task_definitions_prefix(project, family) + revision_text(revision);
}

std::string task_definitions_prefix(std::string_view project, std::string_view family) {
  return "taskdef/" + named("project", project) + "/" + named("family", family) + "/";
}

std::string services_prefix(std::string_view project) { return "service/" + named("project", project) + "/"; }

std::string tasks_prefix(std::string_view project) { return project.empty() ? std::string("task/") : "task/" + named("project", project) + "/"; }

std::string task_statuses_prefix(const TaskId& task) { return "taskstatus/" + to_string(task) + "/"; }

}  // namespace louhi::model
