//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/store/memory_store.h"

#include <boost/asio/post.hpp>

namespace louhi::store {

namespace asio = boost::asio;

std::optional<Bytes> MemoryStore::get(const std::string& key) const {
  dannan::contract::Lock lock(_mutex);
  const auto found = _records.find(key);
  if (found == _records.end()) return std::nullopt;
  return found->second;
}

std::vector<std::pair<std::string, Bytes>> MemoryStore::list(const std::string& prefix) const {
  dannan::contract::Lock lock(_mutex);
  std::vector<std::pair<std::string, Bytes>> out;
  for (auto it = _records.lower_bound(prefix); it != _records.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) out.emplace_back(*it);
  return out;
}

void MemoryStore::put(const std::string& key, Bytes value, Deadline, Executor executor, Completion<Done> done) {
  {
    dannan::contract::Lock lock(_mutex);
    _records[key] = std::move(value);
  }
  notify(key);
  asio::post(executor, [done = std::move(done)] { done(Done{}); });
}

void MemoryStore::erase(const std::string& key, Deadline, Executor executor, Completion<Done> done) {
  bool erased = false;
  {
    dannan::contract::Lock lock(_mutex);
    erased = _records.erase(key) == 1;
  }
  if (erased) notify(key);
  asio::post(executor, [done = std::move(done)] { done(Done{}); });
}

uint64_t MemoryStore::watch(const std::string& prefix, Executor executor, std::function<void()> changed) {
  dannan::contract::Lock lock(_mutex);
  const uint64_t id = ++_next_watch;
  _watchers[id] = Watcher{prefix, std::move(executor), std::move(changed)};
  return id;
}

void MemoryStore::unwatch(uint64_t id) {
  dannan::contract::Lock lock(_mutex);
  _watchers.erase(id);
}

void MemoryStore::notify(const std::string& key) {
  std::vector<Watcher> due;
  {
    dannan::contract::Lock lock(_mutex);
    for (const auto& [id, w] : _watchers) {
      if (key.compare(0, w.prefix.size(), w.prefix) == 0) due.push_back(w);
    }
  }
  for (const auto& w : due) asio::post(w.executor, w.changed);
}

}  // namespace louhi::store
