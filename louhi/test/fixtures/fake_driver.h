//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/post.hpp>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "louhi/runtime/driver.h"

// A runtime in memory, for the agent's tests: it keeps containers and volumes
// as the driver contract describes them, posts every completion to the
// caller's executor, and lets a test make a container exit or change its
// health, fail the next call
// of an operation, or lose the event stream. Every call is recorded in order.
namespace louhi::test {

class FakeDriver final : public runtime::Driver {
 public:
  struct Container {
    runtime::ContainerSpec spec;
    runtime::ContainerInfo info;
  };

  // What describe() answers.
  runtime::RuntimeInfo info{"docker", "26.1.5", "1.45", true, 2, true, true, true, true, true, 4, 8ull << 30};

  // What resolve() answers for a reference without a digest; one not here is
  // not found.
  std::map<std::string, std::string> digests;

  // --- What a test does to it.

  // The next call of `operation` ("pull", "create", "start", ...) fails so.
  void fail_next(const std::string& operation, runtime::Error error) {
    std::lock_guard<std::mutex> lock(_mutex);
    _failures[operation] = std::move(error);
  }
  // An image that cannot be pulled.
  void refuse_image(const std::string& image) {
    std::lock_guard<std::mutex> lock(_mutex);
    _refused_images.insert(image);
  }
  // A running container exits; watchers are told.
  void exit(const std::string& id, int32_t code, bool oom = false) {
    std::vector<Watcher> watchers;
    runtime::ContainerEvent event;
    {
      std::lock_guard<std::mutex> lock(_mutex);
      auto& c = _containers.at(id);
      c.info.state = runtime::ContainerState::exited;
      c.info.exit_code = code;
      c.info.oom_killed = oom;
      c.info.finished_unix_ms = ++_clock;
      event = {id, "die", c.info.labels, code};
      for (const auto& [n, w] : _watchers) watchers.push_back(w);
    }
    for (const auto& w : watchers) boost::asio::post(w.executor, [w, event] { w.event(event); });
  }
  // The event stream ends, as when the runtime restarts.
  void lose_watchers() {
    std::map<uint64_t, Watcher> watchers;
    {
      std::lock_guard<std::mutex> lock(_mutex);
      watchers.swap(_watchers);
    }
    for (const auto& [n, w] : watchers) boost::asio::post(w.executor, [w] { w.lost({runtime::Error::Kind::unavailable, "the runtime restarted"}); });
  }
  // A container's health check says so.
  void set_health(const std::string& id, runtime::Health health) {
    std::lock_guard<std::mutex> lock(_mutex);
    _containers.at(id).info.health = health;
  }
  // A container made outside this test's agent, as by an earlier one.
  std::string add(runtime::ContainerSpec spec, runtime::ContainerState state) {
    std::lock_guard<std::mutex> lock(_mutex);
    return insert(std::move(spec), state);
  }
  void add_volume(runtime::VolumeSpec spec) {
    std::lock_guard<std::mutex> lock(_mutex);
    _volumes[spec.name] = spec;
  }

  // --- What a test reads.

  std::vector<Container> containers() {
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<Container> out;
    for (const auto& [id, c] : _containers) out.push_back(c);
    return out;
  }
  std::optional<Container> named(const std::string& name) {
    std::lock_guard<std::mutex> lock(_mutex);
    for (const auto& [id, c] : _containers) {
      if (c.info.name == name) return c;
    }
    return std::nullopt;
  }
  std::set<std::string> volumes() {
    std::lock_guard<std::mutex> lock(_mutex);
    std::set<std::string> out;
    for (const auto& [name, v] : _volumes) out.insert(name);
    return out;
  }
  std::optional<runtime::VolumeSpec> volume(const std::string& name) {
    std::lock_guard<std::mutex> lock(_mutex);
    const auto found = _volumes.find(name);
    if (found == _volumes.end()) return std::nullopt;
    return found->second;
  }
  std::vector<std::string> calls() {
    std::lock_guard<std::mutex> lock(_mutex);
    return _calls;
  }
  void clear_calls() {
    std::lock_guard<std::mutex> lock(_mutex);
    _calls.clear();
  }
  size_t watchers() {
    std::lock_guard<std::mutex> lock(_mutex);
    return _watchers.size();
  }

  // --- The contract.

  void describe(runtime::Deadline, runtime::Executor executor, runtime::Completion<runtime::RuntimeInfo> done) override {
    auto failed = take("describe", "");
    complete(executor, std::move(done), failed ? runtime::Result<runtime::RuntimeInfo>(*failed) : runtime::Result<runtime::RuntimeInfo>(info));
  }
  void resolve(const std::string& image, runtime::Deadline, runtime::Executor executor, runtime::Completion<std::string> done) override {
    auto failed = take("resolve", image);
    std::string digest;
    if (!failed) {
      const auto at = image.find('@');
      if (at != std::string::npos) {
        digest = image.substr(at + 1);
      } else {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto found = digests.find(image);
        if (found == digests.end()) {
          failed = runtime::Error{runtime::Error::Kind::not_found, "resolving " + image + ": manifest unknown"};
        } else {
          digest = found->second;
        }
      }
    }
    complete(executor, std::move(done), failed ? runtime::Result<std::string>(*failed) : runtime::Result<std::string>(digest));
  }
  void pull(const std::string& image, runtime::Deadline, runtime::Executor executor, runtime::Completion<runtime::ImageInfo> done) override {
    auto failed = take("pull", image);
    bool refused = false;
    {
      std::lock_guard<std::mutex> lock(_mutex);
      refused = _refused_images.count(image) == 1;
    }
    if (!failed && refused) failed = runtime::Error{runtime::Error::Kind::refused, "pull access denied for " + image};
    complete(executor, std::move(done),
             failed ? runtime::Result<runtime::ImageInfo>(*failed) : runtime::Result<runtime::ImageInfo>(runtime::ImageInfo{image, ""}));
  }
  void create_volume(const runtime::VolumeSpec& spec, runtime::Deadline, runtime::Executor executor, runtime::Completion<runtime::Done> done) override {
    auto failed = take("create_volume", spec.name);
    if (!failed) {
      std::lock_guard<std::mutex> lock(_mutex);
      _volumes[spec.name] = spec;
    }
    complete(executor, std::move(done), failed ? runtime::Result<runtime::Done>(*failed) : runtime::Result<runtime::Done>(runtime::Done{}));
  }
  void remove_volume(const std::string& name, runtime::Deadline, runtime::Executor executor, runtime::Completion<runtime::Done> done) override {
    auto failed = take("remove_volume", name);
    if (!failed) {
      std::lock_guard<std::mutex> lock(_mutex);
      _volumes.erase(name);
    }
    complete(executor, std::move(done), failed ? runtime::Result<runtime::Done>(*failed) : runtime::Result<runtime::Done>(runtime::Done{}));
  }
  void list_volumes(runtime::Deadline, runtime::Executor executor, runtime::Completion<std::vector<runtime::VolumeInfo>> done) override {
    auto failed = take("list_volumes", "");
    std::vector<runtime::VolumeInfo> out;
    {
      std::lock_guard<std::mutex> lock(_mutex);
      for (const auto& [name, v] : _volumes) out.push_back({name, v.labels});
    }
    complete(executor, std::move(done),
             failed ? runtime::Result<std::vector<runtime::VolumeInfo>>(*failed) : runtime::Result<std::vector<runtime::VolumeInfo>>(out));
  }
  void create(const runtime::ContainerSpec& spec, runtime::Deadline, runtime::Executor executor, runtime::Completion<std::string> done) override {
    auto failed = take("create", spec.name);
    std::string id;
    if (!failed) {
      std::lock_guard<std::mutex> lock(_mutex);
      for (const auto& [existing, c] : _containers) {
        if (c.info.name == spec.name) failed = runtime::Error{runtime::Error::Kind::refused, "name in use"};
      }
      if (!failed) id = insert(spec, runtime::ContainerState::created);
    }
    complete(executor, std::move(done), failed ? runtime::Result<std::string>(*failed) : runtime::Result<std::string>(id));
  }
  void start(const std::string& id, runtime::Deadline, runtime::Executor executor, runtime::Completion<runtime::Done> done) override {
    auto failed = take("start", id);
    std::optional<runtime::ContainerEvent> event;
    std::vector<Watcher> watchers;
    if (!failed) {
      std::lock_guard<std::mutex> lock(_mutex);
      const auto found = _containers.find(id);
      if (found == _containers.end()) {
        failed = runtime::Error{runtime::Error::Kind::not_found, "no such container"};
      } else if (found->second.info.state != runtime::ContainerState::running) {
        found->second.info.state = runtime::ContainerState::running;
        found->second.info.started_unix_ms = ++_clock;
        found->second.info.exit_code.reset();
        event = runtime::ContainerEvent{id, "start", found->second.info.labels, std::nullopt};
        for (const auto& [n, w] : _watchers) watchers.push_back(w);
      }
    }
    complete(executor, std::move(done), failed ? runtime::Result<runtime::Done>(*failed) : runtime::Result<runtime::Done>(runtime::Done{}));
    if (event) {
      for (const auto& w : watchers) boost::asio::post(w.executor, [w, e = *event] { w.event(e); });
    }
  }
  void stop(const std::string& id, std::chrono::seconds, runtime::Deadline, runtime::Executor executor, runtime::Completion<runtime::Done> done) override {
    auto failed = take("stop", id);
    if (!failed) {
      std::lock_guard<std::mutex> lock(_mutex);
      const auto found = _containers.find(id);
      if (found != _containers.end() && found->second.info.state == runtime::ContainerState::running) {
        found->second.info.state = runtime::ContainerState::exited;
        found->second.info.exit_code = 143;
        found->second.info.finished_unix_ms = ++_clock;
      }
    }
    complete(executor, std::move(done), failed ? runtime::Result<runtime::Done>(*failed) : runtime::Result<runtime::Done>(runtime::Done{}));
  }
  void remove(const std::string& id, runtime::Deadline, runtime::Executor executor, runtime::Completion<runtime::Done> done) override {
    auto failed = take("remove", id);
    if (!failed) {
      std::lock_guard<std::mutex> lock(_mutex);
      _containers.erase(id);
    }
    complete(executor, std::move(done), failed ? runtime::Result<runtime::Done>(*failed) : runtime::Result<runtime::Done>(runtime::Done{}));
  }
  void list(runtime::Deadline, runtime::Executor executor, runtime::Completion<std::vector<runtime::ContainerInfo>> done) override {
    auto failed = take("list", "");
    std::vector<runtime::ContainerInfo> out;
    {
      std::lock_guard<std::mutex> lock(_mutex);
      for (const auto& [id, c] : _containers) out.push_back(c.info);
    }
    complete(executor, std::move(done),
             failed ? runtime::Result<std::vector<runtime::ContainerInfo>>(*failed) : runtime::Result<std::vector<runtime::ContainerInfo>>(out));
  }
  uint64_t watch(runtime::Executor executor, std::function<void(runtime::ContainerEvent)> event, std::function<void(runtime::Error)> lost) override {
    std::lock_guard<std::mutex> lock(_mutex);
    _calls.push_back("watch");
    const uint64_t id = ++_next_watch;
    _watchers[id] = Watcher{std::move(executor), std::move(event), std::move(lost)};
    return id;
  }
  void unwatch(uint64_t id) override {
    std::lock_guard<std::mutex> lock(_mutex);
    _watchers.erase(id);
  }

 private:
  struct Watcher {
    runtime::Executor executor;
    std::function<void(runtime::ContainerEvent)> event;
    std::function<void(runtime::Error)> lost;
  };

  // Records the call; the failure set for it, if any.
  std::optional<runtime::Error> take(const std::string& operation, const std::string& subject) {
    std::lock_guard<std::mutex> lock(_mutex);
    _calls.push_back(subject.empty() ? operation : operation + " " + subject);
    const auto found = _failures.find(operation);
    if (found == _failures.end()) return std::nullopt;
    auto error = found->second;
    _failures.erase(found);
    return error;
  }

  // Under _mutex.
  std::string insert(runtime::ContainerSpec spec, runtime::ContainerState state) {
    const std::string id = "c" + std::to_string(++_next_id);
    Container c;
    c.info.id = id;
    c.info.name = spec.name;
    c.info.image = spec.image;
    c.info.labels = spec.labels;
    c.info.state = state;
    if (state == runtime::ContainerState::running) c.info.started_unix_ms = ++_clock;
    c.spec = std::move(spec);
    _containers[id] = std::move(c);
    return id;
  }

  template <class T>
  static void complete(const runtime::Executor& executor, runtime::Completion<T> done, runtime::Result<T> result) {
    boost::asio::post(executor, [done = std::move(done), result = std::move(result)]() mutable { done(std::move(result)); });
  }

  std::mutex _mutex;
  std::map<std::string, Container> _containers;
  std::map<std::string, runtime::VolumeSpec> _volumes;
  std::map<std::string, runtime::Error> _failures;
  std::set<std::string> _refused_images;
  std::map<uint64_t, Watcher> _watchers;
  std::vector<std::string> _calls;
  uint64_t _next_id = 0;
  uint64_t _next_watch = 0;
  uint64_t _clock = 1'790'000'000'000;
};

}  // namespace louhi::test
