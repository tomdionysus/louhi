//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "louhi/agent/agent.h"
#include "louhi/model/records.h"

// Louhi's events (design, "Events", the topic table): what happens published
// as notifications, never the truth, which stays in the store. A task's status
// as each node running it observes it, retained; a project's events, not
// retained; a node's own status, retained. All at QoS 1.
//
// Publishing never waits and never fails the caller: a notification that
// cannot be sent is the publisher's to count, and the agent and the API go on.
namespace louhi::events {

// Sends one message. Never waits; thread-safe. QoS 1.
class Publisher {
 public:
  virtual ~Publisher() = default;
  virtual void publish(std::string topic, std::string payload, bool retain) = 0;
};

std::string task_status_topic(std::string_view project, const model::TaskId& task, const model::NodeId& node);
std::string events_topic(std::string_view project);
std::string node_topic(const model::NodeId& node);

class Events {
 public:
  // The publisher and the clock outlive this.
  Events(Publisher& publisher, model::NodeId self, const dannan::contract::WallClock& wall);

  // A status this node observed, retained under its task; and, when its
  // state moved to running, stopped or parked, the event that says so.
  void task_status(std::string_view project, const model::TaskStatus& status);
  // The API took a request.
  void task_run(const model::Task& task);
  void task_stop_requested(const model::Task& task);
  // This node's own status, retained.
  void node_status(const model::NodeStatus& status);
  // This node is stopping: its retained status cleared (an empty message).
  void node_gone();

  // An event's payload: {"event","task","node","at","reason"}.
  std::string event_json(std::string_view kind, const model::TaskId& task, std::string_view reason) const;

 private:
  void event(std::string_view project, std::string_view kind, const model::TaskId& task, std::string_view reason);

  Publisher& _publisher;
  const model::NodeId _self;
  const dannan::contract::WallClock& _wall;
  dannan::contract::Mutex _mutex;
  // The last state each task was reported in here, so an event is published
  // on a change, not on every report.
  std::map<model::TaskId, model::TaskState> _last DANNAN_GUARDED_BY(_mutex);
};

// The agent's reports, passed on to `inner` (the store) and then published.
// A status names no project, so the project is the assignment's.
class EventReports final : public agent::Reports {
 public:
  // All three outlive this.
  EventReports(agent::Reports& inner, const agent::Desired& desired, Events& events) : _inner(inner), _desired(desired), _events(events) {}
  void report(const model::TaskStatus& status) override;

 private:
  agent::Reports& _inner;
  const agent::Desired& _desired;
  Events& _events;
};

}  // namespace louhi::events
