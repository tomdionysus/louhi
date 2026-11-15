//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <map>
#include <string>

#include "dannan/contract/thread_safety.h"
#include "louhi/store/store.h"

// The store in one process's memory: for tests, and for a single node until
// the Dagda store exists. Always ready; the later write wins; nothing
// survives the process.
namespace louhi::store {

class MemoryStore final : public Store {
 public:
  bool ready() const override { return true; }
  std::optional<Bytes> get(const std::string& key) const override;
  std::vector<std::pair<std::string, Bytes>> list(const std::string& prefix) const override;
  void put(const std::string& key, Bytes value, Deadline deadline, Executor executor, Completion<Done> done) override;
  void erase(const std::string& key, Deadline deadline, Executor executor, Completion<Done> done) override;
  uint64_t watch(const std::string& prefix, Executor executor, std::function<void()> changed) override;
  void unwatch(uint64_t id) override;

 private:
  struct Watcher {
    std::string prefix;
    Executor executor;
    std::function<void()> changed;
  };
  void notify(const std::string& key) DANNAN_EXCLUDES(_mutex);

  mutable dannan::contract::Mutex _mutex;
  std::map<std::string, Bytes> _records DANNAN_GUARDED_BY(_mutex);
  std::map<uint64_t, Watcher> _watchers DANNAN_GUARDED_BY(_mutex);
  uint64_t _next_watch DANNAN_GUARDED_BY(_mutex) = 0;
};

}  // namespace louhi::store
