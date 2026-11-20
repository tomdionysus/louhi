//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/agent/agent.h"

#include <algorithm>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <iterator>
#include <limits>
#include <set>
#include <utility>

#include "louhi/model/names.h"
#include "louhi/runtime/labels.h"

namespace louhi::agent {

namespace asio = boost::asio;
using runtime::ContainerInfo;
using runtime::Result;

namespace {

using Step = std::function<void(std::function<void()>)>;

std::string container_of(const ContainerInfo& c) {
  const auto found = c.labels.find(std::string(runtime::label_container));
  return found == c.labels.end() ? std::string() : found->second;
}

bool has_exited(const ContainerInfo& c) {
  return c.state == runtime::ContainerState::exited || c.state == runtime::ContainerState::dead || c.state == runtime::ContainerState::removing;
}

bool is_running(const ContainerInfo& c) {
  return c.state == runtime::ContainerState::running || c.state == runtime::ContainerState::restarting || c.state == runtime::ContainerState::paused;
}

model::ContainerState state_of(const ContainerInfo& c) {
  if (has_exited(c)) return model::ContainerState::exited;
  if (is_running(c)) return model::ContainerState::running;
  return model::ContainerState::created;
}

model::Health health_of(const ContainerInfo& c) {
  switch (c.health) {
    case runtime::Health::healthy:
      return model::Health::healthy;
    case runtime::Health::unhealthy:
      return model::Health::unhealthy;
    default:
      return model::Health::unknown;
  }
}

std::string truncated(std::string text, size_t maximum) {
  if (text.size() > maximum) text.resize(maximum);
  return text;
}

void run_steps(const std::shared_ptr<std::vector<Step>>& steps, size_t i, std::function<void()> done) {
  if (i == steps->size()) return done();
  (*steps)[i]([steps, i, done = std::move(done)]() mutable { run_steps(steps, i + 1, std::move(done)); });
}

Step chain(Step first, Step second) {
  return [first = std::move(first), second = std::move(second)](std::function<void()> next) {
    first([second, next = std::move(next)]() mutable { second(std::move(next)); });
  };
}

}  // namespace

Agent::Agent(runtime::Driver& driver, Desired& desired, Reports& reports, model::NodeId self, Clock clock, asio::any_io_executor io,
             dannan::status::ScopedLogger logger, Policy policy)
    : _driver(driver),
      _desired(desired),
      _reports(reports),
      _self(self),
      _clock(std::move(clock)),
      _strand(asio::make_strand(io)),
      _logger(std::move(logger)),
      _policy(std::move(policy)) {}

void Agent::start() {
  auto self = shared_from_this();
  asio::dispatch(_strand, [self] {
    self->watch();
    self->pass();
  });
}

void Agent::poke() {
  auto self = shared_from_this();
  asio::dispatch(_strand, [self] { self->pass(); });
}

void Agent::unpark(const model::TaskId& task) {
  auto self = shared_from_this();
  asio::dispatch(_strand, [self, task] {
    const auto found = self->_pacing.find(task);
    if (found == self->_pacing.end() || !found->second.parked) return;
    self->_logger.info("unparking task " + model::to_string(task) + " at the operator's word");
    auto reported = found->second.reported;
    found->second = Pacing{};
    found->second.reported = std::move(reported);
    self->pass();
  });
}

void Agent::stop(std::function<void()> done) {
  auto self = shared_from_this();
  asio::dispatch(_strand, [self, done = std::move(done)]() mutable {
    self->_stopping = true;
    if (self->_watch) {
      self->_driver.unwatch(*self->_watch);
      self->_watch.reset();
    }
    if (self->_running) {
      self->_on_stopped.push_back(std::move(done));
    } else {
      asio::post(self->_strand, std::move(done));
    }
  });
}

void Agent::watch() {
  if (_stopping) return;
  std::weak_ptr<Agent> weak = weak_from_this();
  _watch = _driver.watch(
      _strand,
      [weak](runtime::ContainerEvent) {
        if (auto self = weak.lock()) self->pass();
      },
      [weak](runtime::Error lost) {
        auto self = weak.lock();
        if (!self) return;
        self->_watch.reset();
        if (self->_stopping) return;
        self->_logger.warn("lost the runtime's event stream (" + lost.message + "); watching again and re-listing");
        self->_clock.after(self->_policy.restarts.initial_backoff, [weak] {
          if (auto again = weak.lock())
            asio::dispatch(again->_strand, [again] {
              again->watch();
              again->pass();
            });
        });
      });
}

void Agent::with_runtime(std::function<void()> then) {
  if (_runtime) return then();
  auto self = shared_from_this();
  _driver.describe(_policy.call_deadline, _strand, [self, then = std::move(then)](Result<runtime::RuntimeInfo> got) {
    if (got.ok()) {
      self->_runtime = got.value();
      const auto& r = *self->_runtime;
      self->_logger.info("runtime " + r.name + " " + r.version + ", cgroup v" + std::to_string(r.cgroup_version) + ", limits: memory " +
                         (r.limits_memory ? "yes" : "no") + ", cpu " + (r.limits_cpu ? "yes" : "no") + ", pids " + (r.limits_pids ? "yes" : "no") +
                         ", storage " + (r.limits_storage ? "yes" : "no") + ", user namespaces " + (r.user_namespaces ? "yes" : "no"));
      if (!r.limits_storage) {
        self->_logger.warn(self->_policy.allow_unbounded_storage ? "this node cannot bound storage; launching tasks anyway, as the operator allows"
                                                                 : "this node cannot bound storage; no task will be launched here");
      }
      if (!r.limits_memory) {
        self->_logger.warn(self->_policy.allow_unenforced_memory ? "this node cannot enforce memory limits; launching tasks anyway, as the operator allows"
                                                                 : "this node cannot enforce memory limits; no task will be launched here");
      }
      if (!r.user_namespaces) {
        self->_logger.warn(self->_policy.allow_without_user_namespaces
                               ? "this node's runtime has no user namespaces; launching tasks anyway, as the operator allows"
                               : "this node's runtime has no user namespaces; no task will be launched here");
      }
    } else {
      self->_logger.warn("cannot describe the runtime: " + got.error().message);
    }
    then();
  });
}

void Agent::pass() {
  if (_stopping) return;
  if (_running) {
    _dirty = true;
    return;
  }
  _running = true;
  _dirty = false;
  auto self = shared_from_this();
  with_runtime([self] {
    self->_driver.list(self->_policy.call_deadline, self->_strand, [self](Result<std::vector<ContainerInfo>> containers) {
      if (!containers.ok()) {
        self->_logger.warn("cannot list containers: " + containers.error().message);
        self->wake_at(self->_clock.steady->now() + self->_policy.restarts.initial_backoff);
        return self->finish_pass();
      }
      self->_driver.list_volumes(self->_policy.call_deadline, self->_strand,
                                 [self, containers = std::move(containers.value())](Result<std::vector<runtime::VolumeInfo>> volumes) {
                                   if (!volumes.ok()) {
                                     self->_logger.warn("cannot list volumes: " + volumes.error().message);
                                     self->wake_at(self->_clock.steady->now() + self->_policy.restarts.initial_backoff);
                                     return self->finish_pass();
                                   }
                                   auto steps = std::make_shared<std::vector<Step>>(self->plan(self->_desired.assignments(), containers, volumes.value()));
                                   run_steps(steps, 0, [self] { self->finish_pass(); });
                                 });
    });
  });
}

void Agent::finish_pass() {
  _running = false;
  _passes.fetch_add(1);
  if (_stopping) {
    for (auto& done : _on_stopped) asio::post(_strand, std::move(done));
    _on_stopped.clear();
    return;
  }
  if (_wake && *_wake <= _clock.steady->now()) _wake.reset();
  if (_dirty) pass();
}

std::vector<Agent::Step> Agent::plan(const std::vector<Assignment>& assignments, const std::vector<ContainerInfo>& containers,
                                     const std::vector<runtime::VolumeInfo>& volumes) {
  std::map<model::TaskId, std::vector<ContainerInfo>> observed;
  for (const auto& c : containers) {
    if (const auto task = runtime::task_of(c.labels)) observed[*task].push_back(c);
  }
  std::map<model::TaskId, std::vector<std::string>> volumes_of;
  for (const auto& v : volumes) {
    if (const auto task = runtime::task_of(v.labels)) volumes_of[*task].push_back(v.name);
  }

  std::vector<Step> steps;
  std::set<model::TaskId> wanted;
  for (const auto& assignment : assignments) {
    if (assignment.task.node != _self) continue;
    const model::TaskId id = assignment.task.id;
    wanted.insert(id);
    if (assignment.task.desired == model::DesiredState::stopped) {
      auto found = observed.find(id);
      auto task_volumes = volumes_of[id];
      if ((found != observed.end() && !found->second.empty()) || !task_volumes.empty()) {
        steps.push_back(teardown(found == observed.end() ? std::vector<ContainerInfo>{} : found->second, std::move(task_volumes), true));
      }
      steps.push_back(report_step(assignment, model::TaskState::stopped, assignment.task.stop_reason, {}));
      continue;
    }
    steps.push_back(handle(assignment, observed[id]));
  }

  // What the runtime has that nothing wants here: only once the desired state
  // is whole, or an agent starting before its store has synced would remove
  // every task on the node.
  if (_desired.ready()) {
    std::set<model::TaskId> orphans;
    for (const auto& [id, list] : observed) {
      if (wanted.count(id) == 0) orphans.insert(id);
    }
    for (const auto& [id, list] : volumes_of) {
      if (wanted.count(id) == 0) orphans.insert(id);
    }
    for (const auto& id : orphans) {
      _logger.info("removing task " + model::to_string(id) + ", which is no longer placed here");
      steps.push_back(teardown(observed[id], volumes_of[id], true));
      _pacing.erase(id);
    }
  }
  for (auto it = _pacing.begin(); it != _pacing.end();) {
    it = wanted.count(it->first) == 0 && observed.count(it->first) == 0 ? _pacing.erase(it) : std::next(it);
  }
  return steps;
}

Agent::Step Agent::handle(const Assignment& a, std::vector<ContainerInfo> observed) {
  Pacing& p = _pacing[a.task.id];
  const TimePoint now = _clock.steady->now();

  std::vector<ContainerInfo> running;
  for (const auto& c : observed) {
    if (is_running(c)) running.push_back(c);
  }
  if (p.parked) {
    return chain(teardown(running, {}, false), report_step(a, model::TaskState::parked, p.parked_reason, observed));
  }
  if (p.finished) {
    return chain(teardown(running, {}, false), report_step(a, model::TaskState::stopped, p.finished_reason, observed));
  }

  // The containers the definition names, as the runtime has them.
  std::map<std::string, ContainerInfo> by_name;
  for (const auto& c : observed) by_name.emplace(container_of(c), c);
  size_t present = 0;
  std::vector<ContainerInfo> created;
  const ContainerInfo* exited_essential = nullptr;
  for (const auto& definition : a.definition.containers) {
    const auto found = by_name.find(definition.name);
    if (found == by_name.end()) continue;
    ++present;
    if (found->second.state == runtime::ContainerState::created) created.push_back(found->second);
    if (definition.essential && has_exited(found->second) && exited_essential == nullptr) exited_essential = &found->second;
  }

  if (exited_essential != nullptr) {
    std::string reason = "essential container " + container_of(*exited_essential) + " exited";
    if (exited_essential->exit_code) reason += " with " + std::to_string(*exited_essential->exit_code);
    if (exited_essential->oom_killed) reason += ", out of memory";
    if (a.task.service.empty()) {
      p.finished = true;
      p.finished_reason = reason;
      return chain(teardown(running, {}, false), report_step(a, model::TaskState::stopped, reason, observed));
    }
    // A window gone quiet since the last exit starts the backoff again.
    if (p.restarts.consecutive_failures() > 0 && now - p.restarts.last_failure() > _policy.restarts.failure_window) p.restarts.succeeded();
    const auto delay = p.restarts.failed(_policy.restarts, now);
    if (!delay) {
      p.parked = true;
      p.parked_reason = "restarted " + std::to_string(p.restarts.failures_in_window() - 1) + " times within " +
                        std::to_string(std::chrono::duration_cast<std::chrono::seconds>(_policy.restarts.failure_window).count()) + " seconds; last, " + reason;
      _logger.warn("parking task " + model::to_string(a.task.id) + ": " + p.parked_reason);
      return chain(teardown(running, {}, false), report_step(a, model::TaskState::parked, p.parked_reason, observed));
    }
    p.retry_at = now + *delay;
    wake_at(*p.retry_at);
    return chain(teardown(observed, {}, true), report_step(a, model::TaskState::pending, "restarting: " + reason, {}));
  }

  if (present == 0) {
    if (!_runtime) {
      wake_at(now + _policy.restarts.initial_backoff);
      return report_step(a, model::TaskState::pending, "waiting to describe this node's runtime", {});
    }
    if (!_runtime->limits_memory && !_policy.allow_unenforced_memory) {
      return report_step(a, model::TaskState::pending, "not launched: this node's runtime cannot enforce memory limits", {});
    }
    if (!_runtime->user_namespaces && !_policy.allow_without_user_namespaces) {
      return report_step(a, model::TaskState::pending, "not launched: this node's runtime has no user namespaces", {});
    }
    if (!_runtime->limits_storage && !_policy.allow_unbounded_storage) {
      return report_step(a, model::TaskState::pending, "not launched: this node's runtime cannot bound storage", {});
    }
    if (p.retry_at && now < *p.retry_at) {
      wake_at(*p.retry_at);
      return report_step(a, model::TaskState::pending, p.last_error.empty() ? "waiting to restart" : "waiting to retry: " + p.last_error, {});
    }
    return launch(a);
  }

  if (present < a.definition.containers.size()) {
    // A launch that broke half way: remove what it made and try again whole.
    _logger.info("task " + model::to_string(a.task.id) + " is incomplete; removing it to relaunch");
    auto self = shared_from_this();
    return chain(teardown(observed, {}, true), [self](std::function<void()> next) {
      self->_dirty = true;
      next();
    });
  }

  if (!created.empty()) return chain(start_created(created), report_step(a, model::TaskState::starting, "", observed));

  p.launches.succeeded();
  p.last_error.clear();
  p.retry_at.reset();
  return report_step(a, model::TaskState::running, "", observed);
}

Agent::Step Agent::launch(const Assignment& a) {
  auto self = shared_from_this();
  return [self, a](std::function<void()> next) {
    struct Launch {
      std::vector<runtime::ContainerSpec> containers;
      std::vector<runtime::VolumeSpec> volumes;
      std::vector<std::string> images;
      std::vector<std::string> ids;
    };
    auto plan = std::make_shared<Launch>();
    // Storage is bounded wherever the runtime can; launching here at all where
    // it cannot is the operator's allowance.
    SpecPolicy here = self->_policy.spec;
    here.bound_storage = self->_runtime && self->_runtime->limits_storage;
    plan->containers = container_specs(a, here);
    plan->volumes = volume_specs(a, here);
    for (const auto& c : plan->containers) {
      if (std::find(plan->images.begin(), plan->images.end(), c.image) == plan->images.end()) plan->images.push_back(c.image);
    }
    self->report(a, model::TaskState::pulling, "", {});

    // Any failure ends the launch; the next pass removes what it made.
    auto fail = [self, a, next](const runtime::Error& error) {
      Pacing& p = self->_pacing[a.task.id];
      p.last_error = truncated(error.message, 512);
      const auto now = self->_clock.steady->now();
      p.retry_at = now + p.launches.failed(self->launch_policy(), now).value_or(self->_policy.restarts.max_backoff);
      self->wake_at(*p.retry_at);
      self->_logger.warn("launching task " + model::to_string(a.task.id) + " failed (" + runtime::kind_name(error.kind) + "): " + error.message);
      self->report(a, model::TaskState::pending, "launch failed: " + p.last_error, {});
      next();
    };

    std::vector<Step> steps;
    for (const auto& image : plan->images) {
      steps.push_back([self, image, fail](std::function<void()> go) {
        self->_driver.pull(image, self->_policy.pull_deadline, self->_strand, [fail, go](Result<runtime::ImageInfo> got) {
          if (!got.ok()) return fail(got.error());
          go();
        });
      });
    }
    for (const auto& volume : plan->volumes) {
      steps.push_back([self, volume, fail](std::function<void()> go) {
        self->_driver.create_volume(volume, self->_policy.call_deadline, self->_strand, [fail, go](Result<runtime::Done> got) {
          if (!got.ok() && got.error().kind != runtime::Error::Kind::refused) return fail(got.error());
          go();
        });
      });
    }
    for (const auto& spec : plan->containers) {
      steps.push_back([self, spec, plan, fail](std::function<void()> go) {
        self->_driver.create(spec, self->_policy.call_deadline, self->_strand, [plan, fail, go](Result<std::string> got) {
          if (!got.ok()) return fail(got.error());
          plan->ids.push_back(got.value());
          go();
        });
      });
    }
    for (size_t i = 0; i < plan->containers.size(); ++i) {
      steps.push_back([self, plan, i, fail](std::function<void()> go) {
        self->_driver.start(plan->ids[i], self->_policy.call_deadline, self->_strand, [fail, go](Result<runtime::Done> got) {
          if (!got.ok()) return fail(got.error());
          go();
        });
      });
    }
    steps.push_back([self, a, next](std::function<void()>) {
      self->_logger.info("launched task " + model::to_string(a.task.id) + " (" + a.task.family + ":" + std::to_string(a.task.revision) + ")");
      self->report(a, model::TaskState::starting, "", {});
      next();
    });
    run_steps(std::make_shared<std::vector<Step>>(std::move(steps)), 0, [] {});
  };
}

Agent::Step Agent::teardown(std::vector<ContainerInfo> containers, std::vector<std::string> volumes, bool remove) {
  auto self = shared_from_this();
  return [self, containers = std::move(containers), volumes = std::move(volumes), remove](std::function<void()> next) {
    std::vector<Step> steps;
    for (const auto& c : containers) {
      if (is_running(c)) {
        steps.push_back([self, id = c.id](std::function<void()> go) {
          self->_driver.stop(id, self->_policy.stop_grace, self->_policy.call_deadline + self->_policy.stop_grace, self->_strand,
                             [self, id, go](Result<runtime::Done> got) {
                               if (!got.ok()) self->_logger.warn("stopping container " + id + ": " + got.error().message);
                               go();
                             });
        });
      }
      if (remove) {
        steps.push_back([self, id = c.id](std::function<void()> go) {
          self->_driver.remove(id, self->_policy.call_deadline, self->_strand, [self, id, go](Result<runtime::Done> got) {
            if (!got.ok()) self->_logger.warn("removing container " + id + ": " + got.error().message);
            go();
          });
        });
      }
    }
    for (const auto& name : volumes) {
      steps.push_back([self, name](std::function<void()> go) {
        self->_driver.remove_volume(name, self->_policy.call_deadline, self->_strand, [self, name, go](Result<runtime::Done> got) {
          if (!got.ok()) self->_logger.warn("removing volume " + name + ": " + got.error().message);
          go();
        });
      });
    }
    run_steps(std::make_shared<std::vector<Step>>(std::move(steps)), 0, std::move(next));
  };
}

Agent::Step Agent::start_created(std::vector<ContainerInfo> containers) {
  auto self = shared_from_this();
  return [self, containers = std::move(containers)](std::function<void()> next) {
    std::vector<Step> steps;
    for (const auto& c : containers) {
      steps.push_back([self, id = c.id](std::function<void()> go) {
        self->_driver.start(id, self->_policy.call_deadline, self->_strand, [self, id, go](Result<runtime::Done> got) {
          if (!got.ok()) self->_logger.warn("starting container " + id + ": " + got.error().message);
          go();
        });
      });
    }
    run_steps(std::make_shared<std::vector<Step>>(std::move(steps)), 0, std::move(next));
  };
}

Agent::Step Agent::report_step(const Assignment& a, model::TaskState state, std::string reason, std::vector<ContainerInfo> observed) {
  auto self = shared_from_this();
  return [self, a, state, reason = std::move(reason), observed = std::move(observed)](std::function<void()> next) {
    self->report(a, state, reason, observed);
    next();
  };
}

void Agent::report(const Assignment& a, model::TaskState state, std::string reason, const std::vector<ContainerInfo>& observed) {
  Pacing& p = _pacing[a.task.id];
  model::TaskStatus status;
  status.task = a.task.id;
  status.node = _self;
  status.state = state;
  status.reason = truncated(std::move(reason), model::max_reason);
  std::set<std::string> seen;
  for (const auto& definition : a.definition.containers) {
    for (const auto& c : observed) {
      const std::string name = container_of(c);
      if (name != definition.name || !seen.insert(name).second) continue;
      model::ContainerStatus container;
      container.name = name;
      container.state = state_of(c);
      if (container.state == model::ContainerState::exited) container.exit_code = c.exit_code;
      // Relaunches made: the exit that parked a task made none.
      container.restarts = static_cast<uint32_t>(p.restarts.total_failures() - (p.parked ? 1 : 0));
      container.health = health_of(c);
      status.containers.push_back(std::move(container));
      if (c.started_unix_ms != 0 && (status.started_unix_ms == 0 || c.started_unix_ms < status.started_unix_ms)) status.started_unix_ms = c.started_unix_ms;
      if (has_exited(c)) status.stopped_unix_ms = std::max(status.stopped_unix_ms, c.finished_unix_ms);
    }
  }

  // Reported only when something but its sequence and time has changed.
  if (p.reported) {
    model::TaskStatus same = status;
    same.sequence = p.reported->sequence;
    same.observed_unix_ms = p.reported->observed_unix_ms;
    if (same == *p.reported) return;
  }
  _sequence = std::max(_sequence + 1, _clock.wall->unix_ms());
  status.sequence = _sequence;
  status.observed_unix_ms = _clock.wall->unix_ms();
  try {
    status.validate();
  } catch (const std::invalid_argument& invalid) {
    _logger.error("not reporting task " + model::to_string(a.task.id) + ": " + invalid.what());
    return;
  }
  p.reported = status;
  _reports.report(status);
}

dannan::sched::RetryPolicy Agent::launch_policy() const {
  dannan::sched::RetryPolicy launches = _policy.restarts;
  // The stack's never-parking form until Dannan names one: no budget, a window
  // that remembers nothing, no run bound.
  launches.max_failures_in_window = std::numeric_limits<size_t>::max();
  launches.failure_window = std::chrono::milliseconds{1};
  launches.max_failing_duration = std::chrono::milliseconds{0};
  return launches;
}

void Agent::wake_at(TimePoint when) {
  if (_wake && *_wake <= when) return;
  _wake = when;
  const auto delay = std::max(std::chrono::duration_cast<std::chrono::milliseconds>(when - _clock.steady->now()), std::chrono::milliseconds(1));
  std::weak_ptr<Agent> weak = weak_from_this();
  _clock.after(delay, [weak] {
    if (auto self = weak.lock()) {
      asio::dispatch(self->_strand, [self] {
        self->_wake.reset();
        self->pass();
      });
    }
  });
}

}  // namespace louhi::agent
