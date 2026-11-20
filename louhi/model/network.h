//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/ip/address_v4.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dannan/listen/limits.h"

// The network records (design, "The network records and security groups in
// detail"): a project's VPCs, their subnets, route tables and security
// groups. Declared state, as records.h's are, with the same validate() rule:
// std::invalid_argument naming the field at fault, never its value. What
// needs more than one record (a subnet against its VPC and its siblings, a
// route table against its VPC) is a check of its own, answering why not or
// nothing.
namespace louhi::model {

inline constexpr uint8_t min_network_prefix = 16;
inline constexpr uint8_t max_network_prefix = 28;
inline constexpr size_t max_routes = 50;
inline constexpr size_t max_security_rules = 60;
// A range's text: "255.255.255.255/32".
inline constexpr size_t max_range_text = 18;

// An IPv4 range with no host bits set, in Dannan's Prefix. Throws
// std::invalid_argument for any other text, an IPv6 range included.
dannan::listen::Prefix parse_range(std::string_view text);
// Whether every address of `inner` is in `outer`, and whether two ranges
// share any address.
bool range_within(const dannan::listen::Prefix& inner, const dannan::listen::Prefix& outer) noexcept;
bool ranges_overlap(const dannan::listen::Prefix& a, const dannan::listen::Prefix& b) noexcept;

// A project's isolated address space. Two VPCs may overlap.
struct Vpc {
  std::string project;
  std::string name;
  std::string cidr;
  std::string description;
  uint64_t created_unix_ms = 0;
  bool operator==(const Vpc&) const = default;
  void validate() const;
};

// A range of a VPC, and the route table its tasks send by (none: the VPC's
// own range only).
struct Subnet {
  std::string project;
  std::string vpc;
  std::string name;
  std::string cidr;
  std::string route_table;
  uint64_t created_unix_ms = 0;
  bool operator==(const Subnet&) const = default;
  void validate() const;
};

// Why `subnet` may not be made in `vpc` beside `others` (the VPC's subnets
// as this node sees them; one of its own name is the subnet itself and not
// counted): outside the VPC, of another VPC, or overlapping a sibling, named.
// Nothing when it may.
std::optional<std::string> check_subnet(const Vpc& vpc, const std::vector<Subnet>& others, const Subnet& subnet);

// A destination range and the gateway it goes to.
struct Route {
  std::string destination;
  std::string gateway;
  bool operator==(const Route&) const = default;
};

// Routes of a VPC. The VPC's own range is local in every table, implicitly.
struct RouteTable {
  std::string project;
  std::string vpc;
  std::string name;
  std::vector<Route> routes;
  uint64_t updated_unix_ms = 0;
  bool operator==(const RouteTable&) const = default;
  void validate() const;
};

// Why `table` may not stand in `vpc`: a route inside the VPC's own range
// (one holding it, as a default route does, is allowed), or the table
// another VPC's. Nothing when it may.
std::optional<std::string> check_route_table(const Vpc& vpc, const RouteTable& table);

// Where a destination goes: the VPC itself, a gateway by name, or nowhere.
struct RouteTo {
  enum class Kind : uint8_t { local, gateway, none };
  Kind kind = Kind::none;
  std::string gateway;
};

// The VPC's own range first, then the longest prefix holding `destination`.
// Both records are valid.
RouteTo route(const Vpc& vpc, const RouteTable& table, const boost::asio::ip::address_v4& destination);

enum class RuleProtocol : uint8_t { any = 1, tcp = 2, udp = 3, icmp = 4 };

// One allow rule: a protocol, ports for tcp and udp (1 to 65535, from at most
// to; both 0 otherwise), and one peer, a range or a group of the VPC.
struct SecurityRule {
  RuleProtocol protocol = RuleProtocol::tcp;
  uint16_t port_from = 0;
  uint16_t port_to = 0;
  std::string cidr;
  std::string group;
  bool operator==(const SecurityRule&) const = default;
};

// Stateful allow rules on a task's interface, each direction alone.
struct SecurityGroup {
  std::string project;
  std::string vpc;
  std::string name;
  std::string description;
  std::vector<SecurityRule> ingress;
  std::vector<SecurityRule> egress;
  uint64_t updated_unix_ms = 0;
  bool operator==(const SecurityGroup&) const = default;
  void validate() const;
};

}  // namespace louhi::model
