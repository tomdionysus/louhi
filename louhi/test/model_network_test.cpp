//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The network records (design, "The network records and security groups in
// detail"):
// - a range is IPv4, written with no host bits, /16 to /28 for a VPC or a
//   subnet; anything else is refused;
// - a subnet lies inside its VPC and overlaps no other subnet of it;
// - a route table holds the VPC's range as local, refuses a route that
//   overlaps it or names a destination twice, and routes by the longest
//   prefix;
// - a security group's rules name a protocol, ports only for tcp and udp,
//   and exactly one peer, a range or a group;
// - each record round-trips through its codec, refuses what it does not
//   define, and has a key in its project and VPC.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "louhi/model/codec.h"
#include "louhi/model/keys.h"
#include "louhi/model/network.h"

namespace louhi::model {
namespace {

Vpc vpc(const std::string& cidr = "10.0.0.0/16") {
  Vpc v;
  v.project = "web";
  v.name = "main";
  v.cidr = cidr;
  return v;
}

Subnet subnet(const std::string& name, const std::string& cidr) {
  Subnet s;
  s.project = "web";
  s.vpc = "main";
  s.name = name;
  s.cidr = cidr;
  return s;
}

RouteTable table(std::vector<Route> routes) {
  RouteTable t;
  t.project = "web";
  t.vpc = "main";
  t.name = "public";
  t.routes = std::move(routes);
  return t;
}

SecurityRule rule(RuleProtocol protocol, uint16_t from, uint16_t to, std::string cidr, std::string group = "") {
  SecurityRule r;
  r.protocol = protocol;
  r.port_from = from;
  r.port_to = to;
  r.cidr = std::move(cidr);
  r.group = std::move(group);
  return r;
}

SecurityGroup group(std::vector<SecurityRule> ingress, std::vector<SecurityRule> egress = {}) {
  SecurityGroup g;
  g.project = "web";
  g.vpc = "main";
  g.name = "frontend";
  g.ingress = std::move(ingress);
  g.egress = std::move(egress);
  return g;
}

TEST(NetworkRanges, AreIpv4WithNoHostBits) {
  EXPECT_EQ(parse_range("10.0.0.0/16").length, 16);
  EXPECT_EQ(parse_range("0.0.0.0/0").length, 0);
  EXPECT_EQ(parse_range("10.1.2.3/32").length, 32);
  for (const char* bad : {"10.0.0.1/16", "10.0.0.0", "10.0.0.0/33", "2001:db8::/64", "ten/8", "", "10.0.0.0/16 ", "010.0.0.0/16"})
    EXPECT_THROW(parse_range(bad), std::invalid_argument) << bad;
}

TEST(NetworkRanges, ContainAndOverlap) {
  EXPECT_TRUE(range_within(parse_range("10.0.1.0/24"), parse_range("10.0.0.0/16")));
  EXPECT_FALSE(range_within(parse_range("10.0.0.0/16"), parse_range("10.0.1.0/24")));
  EXPECT_FALSE(range_within(parse_range("10.1.0.0/24"), parse_range("10.0.0.0/16")));
  EXPECT_TRUE(ranges_overlap(parse_range("10.0.0.0/16"), parse_range("10.0.255.0/24")));
  EXPECT_FALSE(ranges_overlap(parse_range("10.0.0.0/24"), parse_range("10.0.1.0/24")));
}

TEST(NetworkRecords, AVpcAndASubnetAreSixteenToTwentyEight) {
  EXPECT_NO_THROW(vpc("10.0.0.0/16").validate());
  EXPECT_NO_THROW(vpc("192.168.4.0/28").validate());
  EXPECT_THROW(vpc("10.0.0.0/15").validate(), std::invalid_argument);
  EXPECT_THROW(vpc("10.0.0.0/29").validate(), std::invalid_argument);
  EXPECT_THROW(vpc("10.0.0.1/16").validate(), std::invalid_argument);
  auto v = vpc();
  v.name = "Not A Name";
  EXPECT_THROW(v.validate(), std::invalid_argument);
  EXPECT_NO_THROW(subnet("a", "10.0.1.0/24").validate());
  EXPECT_THROW(subnet("a", "10.0.1.0/29").validate(), std::invalid_argument);
  auto s = subnet("a", "10.0.1.0/24");
  s.route_table = "Bad Name";
  EXPECT_THROW(s.validate(), std::invalid_argument);
}

TEST(NetworkRecords, ASubnetLiesInItsVpcAndOverlapsNoOther) {
  const std::vector<Subnet> others{subnet("a", "10.0.1.0/24"), subnet("b", "10.0.2.0/24")};
  EXPECT_FALSE(check_subnet(vpc(), others, subnet("c", "10.0.3.0/24")));
  EXPECT_TRUE(check_subnet(vpc(), others, subnet("c", "10.1.0.0/24"))) << "outside the VPC";
  const auto inside = check_subnet(vpc(), others, subnet("c", "10.0.1.128/25"));
  ASSERT_TRUE(inside) << "inside a";
  EXPECT_NE(inside->find("a"), std::string::npos) << "names the subnet it overlaps: " << *inside;
  EXPECT_TRUE(check_subnet(vpc(), others, subnet("c", "10.0.0.0/22"))) << "holding a and b";
  EXPECT_FALSE(check_subnet(vpc(), others, subnet("a", "10.0.1.0/24"))) << "a subnet is checked against the others, not itself";
  auto elsewhere = subnet("c", "10.0.3.0/24");
  elsewhere.vpc = "other";
  EXPECT_TRUE(check_subnet(vpc(), others, elsewhere)) << "a subnet of another VPC";
}

TEST(NetworkRecords, ARouteTableRefusesARouteOverlappingTheVpcOrNamedTwice) {
  const Route internet{"0.0.0.0/0", "nat"};
  const Route office{"192.168.0.0/16", "vpn"};
  EXPECT_NO_THROW(table({internet, office}).validate());
  EXPECT_THROW(table({internet, internet}).validate(), std::invalid_argument);
  EXPECT_THROW(table({Route{"192.168.0.1/16", "vpn"}}).validate(), std::invalid_argument);
  EXPECT_THROW(table({Route{"192.168.0.0/16", "Bad Name"}}).validate(), std::invalid_argument);
  std::vector<Route> many(51, internet);
  for (size_t i = 0; i < many.size(); ++i) many[i].destination = "10." + std::to_string(100 + i) + ".0.0/16";
  EXPECT_THROW(table(many).validate(), std::invalid_argument);
  // The VPC's own range is local: a route inside it is refused; one beside
  // it, or holding it as a default route does, is not.
  EXPECT_FALSE(check_route_table(vpc(), table({internet, office})));
  EXPECT_TRUE(check_route_table(vpc(), table({Route{"10.0.4.0/24", "nat"}})));
  EXPECT_FALSE(check_route_table(vpc(), table({Route{"10.1.0.0/16", "peer"}})));
}

TEST(NetworkRecords, RoutingIsLocalThenTheLongestPrefix) {
  const auto t = table({Route{"0.0.0.0/0", "nat"}, Route{"192.168.0.0/16", "vpn"}, Route{"192.168.7.0/24", "lab"}});
  const auto at = [&](const char* address) { return route(vpc(), t, boost::asio::ip::make_address_v4(address)); };
  EXPECT_EQ(at("10.0.3.4").kind, RouteTo::Kind::local);
  EXPECT_EQ(at("8.8.8.8").gateway, "nat");
  EXPECT_EQ(at("192.168.1.1").gateway, "vpn");
  EXPECT_EQ(at("192.168.7.9").gateway, "lab");
  EXPECT_EQ(route(vpc(), table({Route{"192.168.0.0/16", "vpn"}}), boost::asio::ip::make_address_v4("8.8.8.8")).kind, RouteTo::Kind::none);
}

TEST(NetworkRecords, ASecurityRuleNamesItsProtocolPortsAndOnePeer) {
  EXPECT_NO_THROW(group({rule(RuleProtocol::tcp, 443, 443, "0.0.0.0/0"), rule(RuleProtocol::udp, 5000, 5100, "", "backend"),
                         rule(RuleProtocol::icmp, 0, 0, "10.0.0.0/16"), rule(RuleProtocol::any, 0, 0, "10.0.0.0/16")})
                      .validate());
  EXPECT_THROW(group({rule(RuleProtocol::tcp, 0, 0, "0.0.0.0/0")}).validate(), std::invalid_argument) << "tcp needs ports";
  EXPECT_THROW(group({rule(RuleProtocol::tcp, 90, 80, "0.0.0.0/0")}).validate(), std::invalid_argument) << "ports backwards";
  EXPECT_THROW(group({rule(RuleProtocol::icmp, 1, 1, "0.0.0.0/0")}).validate(), std::invalid_argument) << "icmp has no ports";
  EXPECT_THROW(group({rule(RuleProtocol::tcp, 80, 80, "", "")}).validate(), std::invalid_argument) << "no peer";
  EXPECT_THROW(group({rule(RuleProtocol::tcp, 80, 80, "0.0.0.0/0", "backend")}).validate(), std::invalid_argument) << "two peers";
  EXPECT_THROW(group({rule(RuleProtocol::tcp, 80, 80, "10.0.0.1/16")}).validate(), std::invalid_argument);
  EXPECT_THROW(group({}, std::vector<SecurityRule>(61, rule(RuleProtocol::any, 0, 0, "0.0.0.0/0"))).validate(), std::invalid_argument);
}

TEST(NetworkCodec, EachRecordRoundTripsAndKeepsItsKind) {
  auto v = vpc();
  v.description = "the site";
  v.created_unix_ms = 7;
  EXPECT_EQ(decode_vpc(encode(v)), v);
  auto s = subnet("a", "10.0.1.0/24");
  s.route_table = "public";
  EXPECT_EQ(decode_subnet(encode(s)), s);
  const auto t = table({Route{"0.0.0.0/0", "nat"}});
  EXPECT_EQ(decode_route_table(encode(t)), t);
  const auto g = group({rule(RuleProtocol::tcp, 80, 443, "0.0.0.0/0")}, {rule(RuleProtocol::any, 0, 0, "", "backend")});
  EXPECT_EQ(decode_security_group(encode(g)), g);
  EXPECT_EQ(peek_kind(encode(g)), Kind::security_group);
  EXPECT_THROW(decode_vpc(encode(s)), dannan::net::DecodeError);
  auto trailing = encode(v);
  trailing.push_back(0);
  EXPECT_THROW(decode_vpc(trailing), dannan::net::DecodeError);
  auto bad = vpc("10.0.0.1/16");
  EXPECT_THROW(encode(bad), std::invalid_argument);
}

TEST(NetworkKeys, LiveInTheirProjectAndVpc) {
  EXPECT_EQ(key(vpc()), "vpc/web/main");
  EXPECT_EQ(key(subnet("a", "10.0.1.0/24")), "subnet/web/main/a");
  EXPECT_EQ(key(table({})), "routetable/web/main/public");
  EXPECT_EQ(key(group({})), "securitygroup/web/main/frontend");
  EXPECT_EQ(subnets_prefix("web", "main"), "subnet/web/main/");
  EXPECT_EQ(security_groups_prefix("web", "main"), "securitygroup/web/main/");
}

}  // namespace
}  // namespace louhi::model
