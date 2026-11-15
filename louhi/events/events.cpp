//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/events/events.h"

#include <boost/json.hpp>

#include "louhi/api/json.h"

namespace louhi::events {

namespace json = boost::json;

// Every segment is a name, or hex, so none needs escaping and none holds an
// MQTT wildcard or a '/'.
std::string task_status_topic(std::string_view project, const model::TaskId& task, const model::NodeId& node) {
  return "louhi/projects/" + std::string(project) + "/tasks/" + model::to_string(task) + "/nodes/" + dannan::identity::to_string(node);
}

std::string events_topic(std::string_view project) { return "louhi/projects/" + std::string(project) + "/events"; }

std::string node_topic(const model::NodeId& node) { return "louhi/nodes/" + dannan::identity::to_string(node); }

Events::Events(Publisher& publisher, model::NodeId self, const dannan::contract::WallClock& wall) : _publisher(publisher), _self(self), _wall(wall) {}

std::string Events::event_json(std::string_view kind, const model::TaskId& task, std::string_view reason) const {
  json::object out{{"event", json::string(kind)}, {"task", model::to_string(task)}, {"node", dannan::identity::to_string(_self)}, {"at", _wall.unix_ms()}};
  if (!reason.empty()) out["reason"] = json::string(reason);
  return json::serialize(out);
}

void Events::event(std::string_view project, std::string_view kind, const model::TaskId& task, std::string_view reason) {
  _publisher.publish(events_topic(project), event_json(kind, task, reason), false);
}

void Events::task_status(std::string_view project, const model::TaskStatus& status) {
  _publisher.publish(task_status_topic(project, status.task, status.node), json::serialize(api::to_json(status)), true);
  const char* kind = nullptr;
  {
    dannan::contract::Lock lock(_mutex);
    const auto before = _last.find(status.task);
    const bool moved = before == _last.end() || before->second != status.state;
    _last[status.task] = status.state;
    if (!moved) return;
  }
  switch (status.state) {
    case model::TaskState::running:
      kind = "task.started";
      break;
    case model::TaskState::stopped:
      kind = "task.stopped";
      break;
    case model::TaskState::parked:
      kind = "task.parked";
      break;
    default:
      return;
  }
  event(project, kind, status.task, status.reason);
}

void Events::task_run(const model::Task& task) { event(task.project, "task.run", task.id, ""); }

void Events::task_stop_requested(const model::Task& task) { event(task.project, "task.stop_requested", task.id, task.stop_reason); }

void Events::node_status(const model::NodeStatus& status) { _publisher.publish(node_topic(status.node), json::serialize(api::to_json(status)), true); }

void Events::node_gone() { _publisher.publish(node_topic(_self), "", true); }

void EventReports::report(const model::TaskStatus& status) {
  _inner.report(status);
  for (const auto& assignment : _desired.assignments()) {
    if (assignment.task.id == status.task) return _events.task_status(assignment.task.project, status);
  }
  // A task no longer assigned here (stopping one removed from the store) has
  // no project to file it under; the store has its status.
}

}  // namespace louhi::events
