//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/net/firewall.h"

#include <boost/asio/ip/address_v4.hpp>
#include <stdexcept>

namespace louhi::net {

namespace {

constexpr size_t max_interface = 15;

void require_interface(const std::string& interface) {
  bool ok = !interface.empty() && interface.size() <= max_interface;
  for (const char c : interface) ok = ok && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-');
  if (!ok) throw std::invalid_argument("an interface name is 1 to 15 of lowercase letters, digits and '-'");
}

std::string member_range(const std::string& address) {
  boost::system::error_code failed;
  const auto parsed = boost::asio::ip::make_address_v4(address, failed);
  // One spelling, as a range's: what is printed back is what was given.
  if (failed || parsed.to_string() != address) throw std::invalid_argument("a group member is not an IPv4 address");
  return address + "/32";
}

std::vector<ResolvedRule> resolve_rules(const std::vector<model::SecurityRule>& rules, const std::map<std::string, std::vector<std::string>>& members) {
  std::vector<ResolvedRule> out;
  for (const auto& rule : rules) {
    ResolvedRule resolved{rule.protocol, rule.port_from, rule.port_to, {}};
    if (!rule.cidr.empty()) {
      resolved.peers.push_back(rule.cidr);
    } else {
      const auto found = members.find(rule.group);
      if (found == members.end()) throw std::invalid_argument("a rule names a group whose members are not given");
      for (const auto& address : found->second) resolved.peers.push_back(member_range(address));
    }
    if (!resolved.peers.empty()) out.push_back(std::move(resolved));
  }
  return out;
}

std::string match(const ResolvedRule& rule) {
  std::string out;
  switch (rule.protocol) {
    case model::RuleProtocol::tcp:
    case model::RuleProtocol::udp: {
      out = rule.protocol == model::RuleProtocol::tcp ? " tcp dport " : " udp dport ";
      out += std::to_string(rule.port_from);
      if (rule.port_to != rule.port_from) out += "-" + std::to_string(rule.port_to);
      break;
    }
    case model::RuleProtocol::icmp:
      out = " meta l4proto icmp";
      break;
    case model::RuleProtocol::any:
      break;
  }
  return out;
}

void direction(std::string& out, const std::string& side, const std::string& interface, const std::string& address, const std::vector<ResolvedRule>& rules) {
  const std::string head = "    " + side + " \"" + interface + "\"";
  out += head + " ct state established,related accept\n";
  out += head + " ct state invalid drop\n";
  for (const auto& rule : rules) {
    out += head + " ip " + address + " {";
    for (size_t i = 0; i < rule.peers.size(); ++i) out += (i ? ", " : " ") + rule.peers[i];
    out += " }" + match(rule) + " accept\n";
  }
  out += head + " drop\n";
}

}  // namespace

InterfacePolicy resolve(const std::string& interface, const std::vector<model::SecurityGroup>& groups,
                        const std::map<std::string, std::vector<std::string>>& members) {
  require_interface(interface);
  InterfacePolicy policy;
  policy.interface = interface;
  for (const auto& group : groups) {
    for (auto& rule : resolve_rules(group.ingress, members)) policy.ingress.push_back(std::move(rule));
    for (auto& rule : resolve_rules(group.egress, members)) policy.egress.push_back(std::move(rule));
  }
  return policy;
}

std::string table_name(const std::string& interface) {
  require_interface(interface);
  std::string name = "louhi_" + interface;
  for (auto& c : name)
    if (c == '-') c = '_';
  return name;
}

std::string render(const InterfacePolicy& policy) {
  const std::string table = "inet " + table_name(policy.interface);
  std::string out;
  out += "add table " + table + "\n";
  out += "delete table " + table + "\n";
  out += "table " + table + " {\n";
  out += "  chain forward {\n";
  out += "    type filter hook forward priority filter; policy accept;\n";
  // Into the task: what the host sends out of its interface.
  direction(out, "oifname", policy.interface, "saddr", policy.ingress);
  // Out of it: what the host receives on its interface.
  direction(out, "iifname", policy.interface, "daddr", policy.egress);
  out += "  }\n";
  out += "}\n";
  return out;
}

}  // namespace louhi::net
