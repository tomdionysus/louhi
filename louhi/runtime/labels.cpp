//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/runtime/labels.h"

namespace louhi::runtime {

std::string container_name(const model::TaskId& task, std::string_view container) { return "louhi-" + model::to_string(task) + "-" + std::string(container); }

std::string volume_name(const model::TaskId& task, std::string_view volume) { return "louhi-" + model::to_string(task) + "-" + std::string(volume); }

std::optional<model::TaskId> task_of(const Labels& labels) {
  const auto found = labels.find(std::string(label_task));
  if (found == labels.end()) return std::nullopt;
  return model::TaskId::parse(found->second);
}

}  // namespace louhi::runtime
