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
#include "dannan/sched/retry.h"
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
// kept for inspection. Once the store holds that report (model::finished) the
// task is never launched again, by this agent or one restarted after its
// containers are gone, and when Policy::retain_finished has passed since the
// report its containers and volumes are removed. A service's task is relaunched, after a backoff, up to
// a budget of restarts within a window; past it, it is parked: stopped,
// reported, and left until an operator unparks it (discipline 2: backoff, a
// budget, a visible parked state and an operator action).
//
// A runtime that cannot enforce memory limits discards them silently, one
// without user namespaces makes root in a container root on the node, and one
// that cannot bound storage lets a task fill the node's disk, so on such a
// node no task is launched, and each says why, unless the operator allows it
// (Policy).
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
  // A service's task relaunched: Dannan's retry discipline (backoff, a budget
  // in a window, then parked). Its unbroken-run backstop is off: a task that
  // runs between its exits is not failing unbroken, and a quiet window starts
  // its backoff again.
  dannan::sched::RetryPolicy restarts{5, std::chrono::minutes(10), std::chrono::seconds(1), std::chrono::minutes(1), std::chrono::milliseconds{0}};
  std::chrono::seconds stop_grace{10};
  // How long a finished task's containers and volumes are kept, for its logs
  // and inspection, after this node reported it stopped.
  std::chrono::milliseconds retain_finished{std::chrono::hours(24)};
  runtime::Deadline call_deadline{30'000};
  runtime::Deadline pull_deadline{600'000};
  // Launch tasks on a node whose runtime cannot enforce memory limits.
  bool allow_unenforced_memory = false;
  // Launch tasks on a node whose runtime has no user namespaces, where root
  // in a container is root on the node.
  bool allow_without_user_namespaces = false;
  // Launch tasks on a node whose runtime cannot bound what a container writes
  // or a disk volume's size; they are then not bounded.
  bool allow_unbounded_storage = false;
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
    // Launches that failed: backed off, never parked, since what fails a
    // launch (a registry away, a pull refused) is not the task's doing.
    dannan::sched::RetryState launches;
    std::optional<TimePoint> retry_at;
    std::string last_error;
    // A service task's exits, each a relaunch, against Policy::restarts.
    dannan::sched::RetryState restarts;
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
  Step handle(const Assignment& assignment, std::vector<runtime::ContainerInfo> observed, std::vector<std::string> volumes);
  // A finished task: kept until its retention has passed, then removed.
  Step finished(const Assignment& assignment, const std::vector<runtime::ContainerInfo>& observed, std::vector<std::string> volumes);
  Step launch(const Assignment& assignment);
  Step teardown(std::vector<runtime::ContainerInfo> containers, std::vector<std::string> volumes, bool remove);
  Step start_created(std::vector<runtime::ContainerInfo> containers);
  Step report_step(const Assignment& assignment, model::TaskState state, std::string reason, std::vector<runtime::ContainerInfo> observed);
  void report(const Assignment& assignment, model::TaskState state, std::string reason, const std::vector<runtime::ContainerInfo>& observed);
  // Policy::restarts' backoff with no budget: a launch is never parked.
  dannan::sched::RetryPolicy launch_policy() const;
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
