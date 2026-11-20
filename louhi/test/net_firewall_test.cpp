//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// A task's security groups as nftables rules (design, "The network records
// and security groups in detail"):
// - a group peer is resolved to its members' addresses, and one with none
//   matches nothing; a range is itself; several groups' rules are joined;
// - each direction alone, on the task's host-side interface: established
//   and related accepted, invalid dropped, the rules accepted, the rest
//   dropped; ingress matches the source, egress the destination;
// - tcp and udp with their ports, icmp, and any protocol;
// - the table is the task's own, replaced whole, and an interface name that
//   is not one is refused, so no record writes into the rules.

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "louhi/net/firewall.h"

namespace louhi::net {
namespace {

using model::RuleProtocol;
using model::SecurityGroup;
using model::SecurityRule;

SecurityRule rule(RuleProtocol protocol, uint16_t from, uint16_t to, std::string cidr, std::string group = "") {
  SecurityRule r;
  r.protocol = protocol;
  r.port_from = from;
  r.port_to = to;
  r.cidr = std::move(cidr);
  r.group = std::move(group);
  return r;
}

SecurityGroup group(std::string name, std::vector<SecurityRule> ingress, std::vector<SecurityRule> egress = {}) {
  SecurityGroup g;
  g.project = "web";
  g.vpc = "main";
  g.name = std::move(name);
  g.ingress = std::move(ingress);
  g.egress = std::move(egress);
  return g;
}

// Whether `text` holds `line` as one whole line, ignoring its indent.
bool has_line(const std::string& text, const std::string& line) {
  size_t at = 0;
  while (at < text.size()) {
    const size_t end = text.find('\n', at);
    std::string current = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
    current.erase(0, current.find_first_not_of(' '));
    if (current == line) return true;
    if (end == std::string::npos) break;
    at = end + 1;
  }
  return false;
}

TEST(NetFirewall, AGroupPeerIsItsMembersAndOneWithNoneMatchesNothing) {
  const std::map<std::string, std::vector<std::string>> members{{"backend", {"10.0.2.5", "10.0.2.6"}}, {"empty", {}}};
  const auto policy = resolve("veth-t1",
                              {group("frontend", {rule(RuleProtocol::tcp, 80, 80, "", "backend"), rule(RuleProtocol::tcp, 22, 22, "", "empty"),
                                                  rule(RuleProtocol::tcp, 443, 443, "0.0.0.0/0")})},
                              members);
  ASSERT_EQ(policy.ingress.size(), 2u);
  EXPECT_EQ(policy.ingress[0].peers, (std::vector<std::string>{"10.0.2.5/32", "10.0.2.6/32"}));
  EXPECT_EQ(policy.ingress[1].peers, (std::vector<std::string>{"0.0.0.0/0"}));
}

TEST(NetFirewall, AGroupNotKnownIsRefused) {
  EXPECT_THROW(resolve("veth-t1", {group("frontend", {rule(RuleProtocol::tcp, 80, 80, "", "missing")})}, {}), std::invalid_argument);
}

TEST(NetFirewall, EachDirectionAcceptsItsRulesAndDropsTheRest) {
  const auto policy = resolve("veth-t1",
                              {group("frontend", {rule(RuleProtocol::tcp, 80, 443, "0.0.0.0/0")}),
                               group("ops", {rule(RuleProtocol::icmp, 0, 0, "10.0.0.0/16")}, {rule(RuleProtocol::udp, 53, 53, "10.0.0.2/32")})},
                              {});
  const std::string text = render(policy);
  EXPECT_TRUE(has_line(text, "table inet louhi_veth_t1 {")) << text;
  EXPECT_TRUE(has_line(text, "type filter hook forward priority filter; policy accept;")) << text;
  // Into the task.
  EXPECT_TRUE(has_line(text, "oifname \"veth-t1\" ct state established,related accept")) << text;
  EXPECT_TRUE(has_line(text, "oifname \"veth-t1\" ct state invalid drop")) << text;
  EXPECT_TRUE(has_line(text, "oifname \"veth-t1\" ip saddr { 0.0.0.0/0 } tcp dport 80-443 accept")) << text;
  EXPECT_TRUE(has_line(text, "oifname \"veth-t1\" ip saddr { 10.0.0.0/16 } meta l4proto icmp accept")) << text;
  EXPECT_TRUE(has_line(text, "oifname \"veth-t1\" drop")) << text;
  // Out of it.
  EXPECT_TRUE(has_line(text, "iifname \"veth-t1\" ct state established,related accept")) << text;
  EXPECT_TRUE(has_line(text, "iifname \"veth-t1\" ip daddr { 10.0.0.2/32 } udp dport 53 accept")) << text;
  EXPECT_TRUE(has_line(text, "iifname \"veth-t1\" drop")) << text;
  // The drop comes after every accept of its direction.
  EXPECT_LT(text.find("oifname \"veth-t1\" ip saddr { 10.0.0.0/16 }"), text.find("oifname \"veth-t1\" drop"));
}

TEST(NetFirewall, AnyProtocolMatchesOnlyThePeer) {
  const auto text = render(resolve("veth-t1", {group("all", {rule(RuleProtocol::any, 0, 0, "10.0.0.0/16")})}, {}));
  EXPECT_TRUE(has_line(text, "oifname \"veth-t1\" ip saddr { 10.0.0.0/16 } accept")) << text;
}

TEST(NetFirewall, NoGroupsAllowNothing) {
  const auto text = render(resolve("veth-t1", {}, {}));
  EXPECT_TRUE(has_line(text, "oifname \"veth-t1\" drop")) << text;
  EXPECT_TRUE(has_line(text, "iifname \"veth-t1\" drop")) << text;
  size_t accepting = 0;
  for (size_t at = text.find(" accept\n"); at != std::string::npos; at = text.find(" accept\n", at + 1)) ++accepting;
  EXPECT_EQ(accepting, 2u) << "nothing accepted but what was established, each way: " << text;
}

TEST(NetFirewall, TheTableIsReplacedWhole) {
  const auto text = render(resolve("veth-t1", {}, {}));
  // Created if absent, then deleted, then made: one script, applied at once.
  EXPECT_TRUE(has_line(text, "add table inet louhi_veth_t1")) << text;
  EXPECT_TRUE(has_line(text, "delete table inet louhi_veth_t1")) << text;
  EXPECT_LT(text.find("add table"), text.find("delete table"));
  EXPECT_LT(text.find("delete table"), text.find("table inet louhi_veth_t1 {"));
  EXPECT_EQ(table_name("veth-t1"), "louhi_veth_t1");
}

TEST(NetFirewall, AnInterfaceNameThatIsNotOneIsRefused) {
  for (const char* bad : {"", "veth\"; flush ruleset", "veth t1", "a-name-far-too-long", "VETH"})
    EXPECT_THROW(resolve(bad, {}, {}), std::invalid_argument) << bad;
}

}  // namespace
}  // namespace louhi::net
