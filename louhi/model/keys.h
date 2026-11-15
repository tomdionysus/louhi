//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <string>
#include <string_view>

#include "louhi/model/records.h"

// Where each record lives in the store: a key in a key-ordered tree (Dagda's
// heads::Tree), built so a project's records of one kind share a prefix and a
// listing is a prefix scan. Every segment is a name, a fixed-width number or
// fixed-width hex, so no segment needs escaping and none can contain '/'.
//
//   project/<project>
//   quota/<project>
//   taskdef/<project>/<family>/<revision, ten digits>
//   service/<project>/<service>
//   task/<project>/<task id>
//   taskstatus/<task id>/<node id>     one per node that ran it
//   node/<node id>
namespace louhi::model {

std::string key(const Project& record);
std::string key(const Quota& record);
std::string key(const TaskDefinition& record);
std::string key(const Service& record);
std::string key(const Task& record);
std::string key(const TaskStatus& record);
std::string key(const NodeStatus& record);

// Prefixes for listing. Each throws std::invalid_argument for a project or
// family that is not a name.
std::string task_definitions_prefix(std::string_view project, std::string_view family);
std::string services_prefix(std::string_view project);
std::string tasks_prefix(std::string_view project);
std::string task_statuses_prefix(const TaskId& task);

}  // namespace louhi::model
