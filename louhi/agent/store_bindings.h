//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include "dannan/status/log.h"
#include "louhi/agent/agent.h"
#include "louhi/store/store.h"

// The agent's inputs and outputs over the store: what is placed on this node
// is read from the Task records and the revisions they name; what the node
// observes is written as TaskStatus records under the node's own keys, so no
// other node ever writes them.
namespace louhi::agent {

class StoreDesired final : public Desired {
 public:
  StoreDesired(const store::Store& store, model::NodeId self, dannan::status::ScopedLogger logger);
  bool ready() const override;
  // A record that does not decode, or a task whose revision is not in the
  // store, is left out and logged: the agent cannot run what it cannot read.
  std::vector<Assignment> assignments() const override;

 private:
  const store::Store& _store;
  const model::NodeId _self;
  const dannan::status::ScopedLogger _logger;
};

class StoreReports final : public Reports {
 public:
  StoreReports(store::Store& store, store::Executor executor, dannan::status::ScopedLogger logger, store::Deadline deadline = store::Deadline(30'000));
  // A write that fails is logged; the agent reports again on its next change,
  // and a later sequence replaces an earlier one.
  void report(const model::TaskStatus& status) override;

 private:
  store::Store& _store;
  const store::Executor _executor;
  const dannan::status::ScopedLogger _logger;
  const store::Deadline _deadline;
};

}  // namespace louhi::agent
