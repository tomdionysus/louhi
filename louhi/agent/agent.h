//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/strand.hpp>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dannan/contract/time_source.h"
#include "dannan/status/log.h"
#include "louhi/agent/specs.h"
#include "louhi/model/records.h"
#include "louhi/runtime/driver.h"

// The node agent (design, "The node"): it runs the tasks placed on its node.
// Each pass reads what should run (Desired) and what the runtime has (the
// driver's list, by Louhi's labels), and acts on the difference: it launches
// what is missing, starts what was made but not started, restarts or stops
// what exited, and removes what is no longer wanted. Nothing it decides by is
// remembered that the runtime and the records cannot give again
// (disciplines 1 and 3), so an agent restarted mid-pass picks up where the
// runtime stands. Tasks keep running while the agent is down.
//
// Passes run one at a time on the agent's strand; a poke during a pass runs
// one more after it. Runtime events only poke: the list is the truth.
//
// What exits: a task run on its own (no service) is done when an essential
// container exits, and is reported stopped, its other containers stopped and
// kept for inspection. A service's task is relaunched, after a backoff, up to
// a budget of restarts within a window; past it, it is parked: stopped,
// reported, and left until an operator unparks it (discipline 2: backoff, a
// budget, a visible parked state and an operator action).
//
// A runtime that cannot enforce memory limits discards them silently, so on
// such a node no task is launched, and each says why, unless the operator
// allows it (Policy).
namespace louhi::agent {

// What should run on this node.
class Desired {
 public:
  virtual ~Desired() = default;
  // Whether it holds the whole desired state yet. Until it does, the agent
  // removes nothing it does not recognise. Waits on nothing; thread-safe.
  virtual bool ready() const = 0;
  // The tasks placed on this node. Waits on nothing; thread-safe.
  virtual std::vector<Assignment> assignments() const = 0;
};

// Where the agent's observations go.
class Reports {
 public:
  virtual ~Reports() = default;
  // A task's status changed; its sequence is higher than any this node
  // reported before. Waits on nothing; thread-safe.
  virtual void report(const model::TaskStatus& status) = 0;
};

// The agent's clocks and its one timer.
struct Clock {
  const dannan::contract::TimeSource* steady = nullptr;
  const dannan::contract::WallClock* wall = nullptr;
  // Calls `fn` after `delay`, on any thread. Waits on nothing.
  std::function<void(std::chrono::milliseconds, std::function<void()>)> after;
};

struct Policy {
  SpecPolicy spec;
  uint32_t restart_budget = 5;
  std::chrono::milliseconds restart_window = std::chrono::minutes(10);
  std::chrono::milliseconds first_backoff = std::chrono::seconds(1);
  std::chrono::milliseconds max_backoff = std::chrono::minutes(1);
  std::chrono::seconds stop_grace{10};
  runtime::Deadline call_deadline{30'000};
  runtime::Deadline pull_deadline{600'000};
  // Launch tasks on a node whose runtime cannot enforce memory limits.
  bool allow_unenforced_memory = false;
};

class Agent : public std::enable_shared_from_this<Agent> {
 public:
  Agent(runtime::Driver& driver, Desired& desired, Reports& reports, model::NodeId self, Clock clock, boost::asio::any_io_executor io,
        dannan::status::ScopedLogger logger, Policy policy);

  // Describes the runtime, watches its events, and runs the first pass.
  // Waits on nothing.
  void start();
  // Desired state may have moved: run a pass. Waits on nothing; thread-safe.
  void poke();
  // The operator's action on a parked task: try it again from a clean slate.
  // Waits on nothing; thread-safe.
  void unpark(const model::TaskId& task);
  // No pass starts after this; `done` is posted to the agent's executor once
  // the pass in flight, if any, has finished. Tasks are left running.
  void stop(std::function<void()> done);

  // Passes finished so far. Thread-safe.
  uint64_t passes() const noexcept { return _passes.load(); }

 private:
  using TimePoint = dannan::contract::TimePoint;
  using Step = std::function<void(std::function<void()>)>;

  // What the agent keeps about a task between passes: only what paces it.
  struct Pacing {
    uint32_t failures = 0;
    std::optional<TimePoint> retry_at;
    std::string last_error;
    std::deque<TimePoint> restarts;
    uint32_t total_restarts = 0;
    bool parked = false;
    std::string parked_reason;
    // A task run on its own that has finished; never launched again.
    bool finished = false;
    std::string finished_reason;
    std::optional<model::TaskStatus> reported;
  };

  void pass();
  void finish_pass();
  void with_runtime(std::function<void()> then);
  void watch();
  std::vector<Step> plan(const std::vector<Assignment>& assignments, const std::vector<runtime::ContainerInfo>& containers,
                         const std::vector<runtime::VolumeInfo>& volumes);
  Step handle(const Assignment& assignment, std::vector<runtime::ContainerInfo> observed);
  Step launch(const Assignment& assignment);
  Step teardown(std::vector<runtime::ContainerInfo> containers, std::vector<std::string> volumes, bool remove);
  Step start_created(std::vector<runtime::ContainerInfo> containers);
  Step report_step(const Assignment& assignment, model::TaskState state, std::string reason, std::vector<runtime::ContainerInfo> observed);
  void report(const Assignment& assignment, model::TaskState state, std::string reason, const std::vector<runtime::ContainerInfo>& observed);
  std::chrono::milliseconds backoff(uint32_t attempt) const;
  void wake_at(TimePoint when);

  runtime::Driver& _driver;
  Desired& _desired;
  Reports& _reports;
  const model::NodeId _self;
  const Clock _clock;
  boost::asio::strand<boost::asio::any_io_executor> _strand;
  const dannan::status::ScopedLogger _logger;
  const Policy _policy;

  // Touched only on _strand.
  std::optional<runtime::RuntimeInfo> _runtime;
  std::map<model::TaskId, Pacing> _pacing;
  uint64_t _sequence = 0;
  bool _running = false;
  bool _dirty = false;
  bool _stopping = false;
  std::optional<TimePoint> _wake;
  std::optional<uint64_t> _watch;
  std::vector<std::function<void()>> _on_stopped;

  std::atomic<uint64_t> _passes{0};
};

}  // namespace louhi::agent
