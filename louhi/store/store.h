//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "louhi/model/codec.h"
#include "louhi/runtime/driver.h"

// Where Louhi's records live (design, "Records"), as Louhi needs it: records
// by key (model/keys.h), read from this node's copy, listed by prefix, written
// asynchronously, and watched by prefix. Dagda's head spaces are the store the
// design names; until Louhi can pin a Dagda ref (ACTIVE.md), MemoryStore
// stands in, and the Dagda implementation later replaces one component.
//
// Reads are of this node's copy and wait on nothing. A write completes once
// on the caller's executor, at the latest by its deadline. A watch is a
// prompt that something under its prefix may have changed, never the change
// itself: the watcher reads again (discipline 1).
//
// What this interface does not promise, because Dagda does not: no
// uniqueness, no transaction, no linearizable write. Two nodes writing one
// key concurrently are reconciled by the kind's merge in the Dagda
// implementation; here the later write wins.
namespace louhi::store {

using Bytes = model::Bytes;
using runtime::Completion;
using runtime::Deadline;
using runtime::Done;
using runtime::Executor;

class Store {
 public:
  virtual ~Store() = default;

  // Whether this node's copy is whole yet. Until it is, absence proves
  // nothing. Waits on nothing; thread-safe.
  virtual bool ready() const = 0;
  // Waits on nothing; thread-safe.
  virtual std::optional<Bytes> get(const std::string& key) const = 0;
  // Every record under `prefix`, in key order. Waits on nothing; thread-safe.
  virtual std::vector<std::pair<std::string, Bytes>> list(const std::string& prefix) const = 0;

  virtual void put(const std::string& key, Bytes value, Deadline deadline, Executor executor, Completion<Done> done) = 0;
  // Removing a key that is not there succeeds.
  virtual void erase(const std::string& key, Deadline deadline, Executor executor, Completion<Done> done) = 0;

  // `changed` is posted to `executor` after any write under `prefix`, here or
  // arriving from a peer; several writes may give one prompt. Thread-safe.
  virtual uint64_t watch(const std::string& prefix, Executor executor, std::function<void()> changed) = 0;
  virtual void unwatch(uint64_t id) = 0;
};

}  // namespace louhi::store
