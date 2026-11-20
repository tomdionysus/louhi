//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "dannan/agree/agreement.h"
#include "louhi/admit/registers.h"

// Louhi's registers on Dannan's agreement (design, "Placement and the
// service scheduler" and "Projects and quotas"): where each lives, and the
// two calls the API makes of them, a family's next revision and a project's
// admission.
//
// Every call completes once, posted to the caller's executor, at the latest
// at its deadline, with evidence: what was decided, or why nothing was, and
// whether it may have taken effect all the same (`unknown`). Asking again
// with the same request, or for the same tasks, is always safe (registers.h).
//
// What serves a register's slot (its replica set and version) is the
// caller's to say: in tests the simulation's genesis set; in real use it
// waits for Dagda's AcceptorStore (ACTIVE.md), and is chosen then.
namespace louhi::admit {

using Executor = boost::asio::any_io_executor;
using NodeId = dannan::identity::NodeId;

// The agreement space Louhi's registers are in, and the capability a node
// advertises to propose in it.
inline constexpr const char* agreement_space = "louhi";

// The registers' names: a family's revisions, a project's admission.
std::string revisions_register(const std::string& project, const std::string& family);
std::string admission_register(const std::string& project);

// The set serving a slot.
struct Serving {
  std::vector<NodeId> replicas;
  uint64_t version = 0;
};

// Changes one register. Implemented on Dannan's Agreement; a test may give
// any other.
class Registers {
 public:
  virtual ~Registers() = default;
  // Runs `change` on `name`'s current value until committed or `deadline`;
  // `done` is posted to `executor` once. Thread-safe.
  virtual void change(const std::string& name, dannan::agree::Change change, std::chrono::milliseconds deadline, Executor executor,
                      std::function<void(dannan::agree::Outcome)> done) = 0;
};

// Registers on a node's Agreement in `space`, each in the slot slot_of gives
// its name, on the set `serving` names for that slot. The Agreement must
// outlive it.
class AgreementRegisters final : public Registers {
 public:
  AgreementRegisters(dannan::agree::Agreement& agreement, std::function<Serving(dannan::agree::Slot)> serving, std::string space = agreement_space);
  void change(const std::string& name, dannan::agree::Change change, std::chrono::milliseconds deadline, Executor executor,
              std::function<void(dannan::agree::Outcome)> done) override;

 private:
  dannan::agree::Agreement& _agreement;
  const std::function<Serving(dannan::agree::Slot)> _serving;
  const std::string _space;
};

// What a take of a revision came to. `finished`: the register answered, and
// `revision` is the one given, or none when the family has given them all.
// Otherwise `why`, and `unknown` when a revision may have been given all the
// same: asking again with the same request answers it.
struct RevisionEvidence {
  bool finished = false;
  std::optional<uint32_t> revision;
  bool unknown = false;
  std::string why;
};

void take_revision(Registers& registers, const std::string& project, const std::string& family, RequestId request, std::chrono::milliseconds deadline,
                   Executor executor, std::function<void(RevisionEvidence)> done);

// What an admission or a release came to. `finished`: the register answered,
// and `decision` is its answer. Otherwise `why`, and `unknown` when it may
// have taken effect all the same: asking again for the same tasks says.
struct AdmissionEvidence {
  bool finished = false;
  Decision decision;
  bool unknown = false;
  std::string why;
};

// Throws std::invalid_argument as admit() does.
void admit_tasks(Registers& registers, const model::Quota& quota, std::vector<model::TaskId> tasks, TaskShare share, std::chrono::milliseconds deadline,
                 Executor executor, std::function<void(AdmissionEvidence)> done);
void release_task(Registers& registers, const std::string& project, model::TaskId task, std::chrono::milliseconds deadline, Executor executor,
                  std::function<void(AdmissionEvidence)> done);

}  // namespace louhi::admit
