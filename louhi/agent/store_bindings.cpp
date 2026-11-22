//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/agent/store_bindings.h"

#include "louhi/model/keys.h"

namespace louhi::agent {

StoreDesired::StoreDesired(const store::Store& store, model::NodeId self, dannan::status::ScopedLogger logger)
    : _store(store), _self(self), _logger(std::move(logger)) {}

bool StoreDesired::ready() const { return _store.ready(); }

std::vector<Assignment> StoreDesired::assignments() const {
  std::vector<Assignment> out;
  for (const auto& [key, bytes] : _store.list(model::tasks_prefix(""))) {
    model::Task task;
    try {
      task = model::decode_task(bytes);
    } catch (const model::DecodeError& bad) {
      _logger.warn("skipping record " + key + ": " + bad.what());
      continue;
    }
    if (task.node != _self) continue;
    const auto definition_bytes = _store.get(model::task_definition_key(task.project, task.family, task.revision));
    if (!definition_bytes) {
      _logger.warn("skipping task " + model::to_string(task.id) + ": its revision " + task.family + ":" + std::to_string(task.revision) +
                   " is not in the store");
      continue;
    }
    Assignment assignment;
    try {
      assignment.definition = model::decode_task_definition(*definition_bytes);
    } catch (const model::DecodeError& bad) {
      _logger.warn("skipping task " + model::to_string(task.id) + ": its revision does not decode: " + bad.what());
      continue;
    }
    model::TaskStatus probe;
    probe.task = task.id;
    probe.node = _self;
    if (const auto status_bytes = _store.get(model::key(probe))) {
      // A report that does not decode is as none: the agent reports afresh.
      try {
        assignment.status = model::decode_task_status(*status_bytes);
      } catch (const model::DecodeError& bad) {
        _logger.warn("task " + model::to_string(task.id) + ": this node's status of it does not decode: " + bad.what());
      }
    }
    assignment.task = std::move(task);
    out.push_back(std::move(assignment));
  }
  return out;
}

StoreReports::StoreReports(store::Store& store, store::Executor executor, dannan::status::ScopedLogger logger, store::Deadline deadline)
    : _store(store), _executor(std::move(executor)), _logger(std::move(logger)), _deadline(deadline) {}

void StoreReports::report(const model::TaskStatus& status) {
  const std::string key = model::key(status);
  _store.put(key, model::encode(status), _deadline, _executor, [logger = _logger, key](runtime::Result<runtime::Done> written) {
    if (!written.ok()) logger.warn("writing " + key + ": " + written.error().message);
  });
}

}  // namespace louhi::agent
