//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/admit/agreement.h"

#include <span>
#include <utility>

namespace louhi::admit {

namespace {

dannan::agree::Name name_bytes(const std::string& name) { return dannan::agree::Name(name.begin(), name.end()); }

// The register's answer decoded, or why it could not be: a result that does
// not decode is a register value that did not (registers.cpp, guarded).
template <class Evidence, class Decode, class Apply>
Evidence evidence_of(const dannan::agree::Outcome& outcome, Decode decode, Apply apply) {
  Evidence evidence;
  if (!outcome.committed) {
    evidence.unknown = outcome.unknown;
    evidence.why = outcome.why.empty() ? "not committed" : outcome.why;
    return evidence;
  }
  try {
    apply(evidence, decode(std::span<const uint8_t>(outcome.result)));
    evidence.finished = true;
  } catch (const dannan::net::DecodeError& error) {
    evidence.why = std::string("the register's value could not be read: ") + error.what();
  }
  return evidence;
}

}  // namespace

std::string revisions_register(const std::string& project, const std::string& family) { return "revisions/" + project + "/" + family; }

std::string admission_register(const std::string& project) { return "admission/" + project; }

AgreementRegisters::AgreementRegisters(dannan::agree::Agreement& agreement, std::function<Serving(dannan::agree::Slot)> serving, std::string space)
    : _agreement(agreement), _serving(std::move(serving)), _space(std::move(space)) {}

void AgreementRegisters::change(const std::string& name, dannan::agree::Change change, std::chrono::milliseconds deadline, Executor executor,
                                std::function<void(dannan::agree::Outcome)> done) {
  auto bytes = name_bytes(name);
  const auto slot = dannan::agree::slot_of(bytes, _agreement.table().slots);
  auto serving = _serving(slot);
  _agreement.change(_space, slot, {std::move(bytes)}, std::move(serving.replicas), serving.version, std::move(change), {}, deadline, std::move(executor),
                    std::move(done));
}

void take_revision(Registers& registers, const std::string& project, const std::string& family, RequestId request, std::chrono::milliseconds deadline,
                   Executor executor, std::function<void(RevisionEvidence)> done) {
  registers.change(revisions_register(project, family), admit::take_revision(request), deadline, std::move(executor),
                   [done = std::move(done)](dannan::agree::Outcome outcome) {
                     done(evidence_of<RevisionEvidence>(outcome, decode_revision_taken,
                                                        [](RevisionEvidence& evidence, const RevisionTaken& taken) { evidence.revision = taken.revision; }));
                   });
}

namespace {

void decide(Registers& registers, const std::string& project, dannan::agree::Change change, std::chrono::milliseconds deadline, Executor executor,
            std::function<void(AdmissionEvidence)> done) {
  registers.change(admission_register(project), std::move(change), deadline, std::move(executor), [done = std::move(done)](dannan::agree::Outcome outcome) {
    done(evidence_of<AdmissionEvidence>(outcome, decode_decision, [](AdmissionEvidence& evidence, const Decision& decision) { evidence.decision = decision; }));
  });
}

}  // namespace

void admit_tasks(Registers& registers, const model::Quota& quota, std::vector<model::TaskId> tasks, TaskShare share, std::chrono::milliseconds deadline,
                 Executor executor, std::function<void(AdmissionEvidence)> done) {
  decide(registers, quota.project, admit(std::move(tasks), share, quota), deadline, std::move(executor), std::move(done));
}

void release_task(Registers& registers, const std::string& project, model::TaskId task, std::chrono::milliseconds deadline, Executor executor,
                  std::function<void(AdmissionEvidence)> done) {
  decide(registers, project, release(task), deadline, std::move(executor), std::move(done));
}

}  // namespace louhi::admit
