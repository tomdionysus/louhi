//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "louhi/model/network.h"
#include "louhi/runtime/driver.h"

// A task's security groups as nftables rules (design, "The network records
// and security groups in detail"): resolve() turns the groups into rules with
// every peer an address range, render() writes them as one table of the
// task's own, and Firewall applies it. The first two are pure functions;
// only Firewall touches a node.
namespace louhi::net {

// One allow rule with its peers as ranges ("a.b.c.d/n"), never empty.
struct ResolvedRule {
  model::RuleProtocol protocol = model::RuleProtocol::any;
  uint16_t port_from = 0;
  uint16_t port_to = 0;
  std::vector<std::string> peers;
  bool operator==(const ResolvedRule&) const = default;
};

// What one task interface allows, each direction alone.
struct InterfacePolicy {
  std::string interface;
  std::vector<ResolvedRule> ingress;
  std::vector<ResolvedRule> egress;
};

// The rules of `groups`, in order, for the task whose host-side interface is
// `interface`. `members` holds every group a rule names, with its members'
// IPv4 addresses; a rule naming a group with none is left out, as it matches
// nothing. Throws std::invalid_argument for an interface that is not one (1
// to 15 of lowercase letters, digits and '-'), a group not in `members`, or
// a member that is not an IPv4 address. The groups are valid records.
InterfacePolicy resolve(const std::string& interface, const std::vector<model::SecurityGroup>& groups,
                        const std::map<std::string, std::vector<std::string>>& members);

// The table's name for an interface: "louhi_" and the name, '-' as '_'.
std::string table_name(const std::string& interface);

// An nftables script that replaces the interface's table whole, atomically
// when applied as one: established and related accepted, invalid dropped, the
// rules accepted, the rest dropped, into the task (by the interface as output)
// and out of it (as input). The policy is resolve()'s.
std::string render(const InterfacePolicy& policy);

// Applies and removes a task's table on this node. The driver that runs
// `nft` comes with the task namespaces (design, step 9); tests use fakes.
class Firewall {
 public:
  virtual ~Firewall() = default;
  // Applies render()'s script as one transaction. Completes once, posted to
  // `executor`, by `deadline`.
  virtual void apply(std::string script, runtime::Deadline deadline, runtime::Executor executor, runtime::Completion<runtime::Done> done) = 0;
  // Removes the table; one not there is no error.
  virtual void remove(std::string table, runtime::Deadline deadline, runtime::Executor executor, runtime::Completion<runtime::Done> done) = 0;
};

}  // namespace louhi::net
