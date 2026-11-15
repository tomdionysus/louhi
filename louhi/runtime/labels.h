//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "louhi/model/ids.h"
#include "louhi/runtime/driver.h"

// How Louhi marks what it makes in a runtime, so it can find it again with
// nothing but the runtime to ask. Every container and volume carries
// louhi.task; a container also louhi.container, and both louhi.project and
// louhi.revision. Names are derived from the same facts, so one task's
// containers can never collide with another's on a node.
namespace louhi::runtime {

inline constexpr std::string_view label_task = "louhi.task";
inline constexpr std::string_view label_container = "louhi.container";
inline constexpr std::string_view label_project = "louhi.project";
inline constexpr std::string_view label_family = "louhi.family";
inline constexpr std::string_view label_revision = "louhi.revision";

// "louhi-<task id>-<container>".
std::string container_name(const model::TaskId& task, std::string_view container);
// "louhi-<task id>-<volume>".
std::string volume_name(const model::TaskId& task, std::string_view volume);

// The task a container or volume belongs to, from its labels; empty when it is
// not Louhi's or the label is not a task id.
std::optional<model::TaskId> task_of(const Labels& labels);

}  // namespace louhi::runtime
