//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/model/network.h"

#include <set>
#include <stdexcept>

#include "louhi/model/names.h"
#include "louhi/model/records.h"

namespace louhi::model {

namespace {

using dannan::listen::Prefix;

void require(bool condition, const std::string& why) {
  if (!condition) throw std::invalid_argument(why);
}

void require_range(const std::string& what, const std::string& text) {
  try {
    parse_range(text);
  } catch (const std::invalid_argument&) {
    throw std::invalid_argument(what + " is not an IPv4 range with no host bits set");
  }
}

void require_network(const std::string& what, const std::string& text) {
  require_range(what, text);
  const auto length = parse_range(text).length;
  require(length >= min_network_prefix && length <= max_network_prefix,
          what + " is not /" + std::to_string(min_network_prefix) + " to /" + std::to_string(max_network_prefix));
}

void validate_rules(const std::string& what, const std::vector<SecurityRule>& rules) {
  require(rules.size() <= max_security_rules, what + " has more than " + std::to_string(max_security_rules) + " rules");
  for (const auto& r : rules) {
    require(r.protocol >= RuleProtocol::any && r.protocol <= RuleProtocol::icmp, what + " rule protocol is not any, tcp, udp or icmp");
    if (r.protocol == RuleProtocol::tcp || r.protocol == RuleProtocol::udp) {
      require(r.port_from >= 1 && r.port_from <= r.port_to, what + " rule ports are not 1 to 65535, the first at most the last");
    } else {
      require(r.port_from == 0 && r.port_to == 0, what + " rule has ports for a protocol without them");
    }
    require(r.cidr.empty() != r.group.empty(), what + " rule does not name exactly one peer, a range or a group");
    if (!r.cidr.empty()) require_range(what + " rule range", r.cidr);
    if (!r.group.empty()) require_name(what + " rule group", r.group);
  }
}

}  // namespace

Prefix parse_range(std::string_view text) {
  require(text.size() <= max_range_text && text.find('/') != std::string_view::npos, "a range is a.b.c.d/n");
  const Prefix prefix = Prefix::parse(text);
  require(prefix.address.is_v4(), "a range is IPv4");
  // One spelling per range: what is printed back is what was written.
  require(prefix.address.to_string() + "/" + std::to_string(prefix.length) == text, "a range is written a.b.c.d/n with no leading zeros");
  // Host bits set are refused by Prefix::parse itself (Dannan 0.10.0).
  return prefix;
}

bool range_within(const Prefix& inner, const Prefix& outer) noexcept { return inner.length >= outer.length && outer.contains(inner.address); }

bool ranges_overlap(const Prefix& a, const Prefix& b) noexcept { return a.contains(b.address) || b.contains(a.address); }

void Vpc::validate() const {
  require_name("vpc project", project);
  require_name("vpc name", name);
  require_network("vpc range", cidr);
  require(description.size() <= max_description && description.find('\0') == std::string::npos,
          "vpc description is not text of at most " + std::to_string(max_description) + " bytes");
}

void Subnet::validate() const {
  require_name("subnet project", project);
  require_name("subnet vpc", vpc);
  require_name("subnet name", name);
  require_network("subnet range", cidr);
  if (!route_table.empty()) require_name("subnet route table", route_table);
}

std::optional<std::string> check_subnet(const Vpc& vpc, const std::vector<Subnet>& others, const Subnet& subnet) {
  if (subnet.project != vpc.project || subnet.vpc != vpc.name) return "the subnet is not of this VPC";
  const auto range = parse_range(subnet.cidr);
  if (!range_within(range, parse_range(vpc.cidr))) return "the subnet's range is not inside the VPC's";
  for (const auto& other : others) {
    if (other.project != subnet.project || other.vpc != subnet.vpc || other.name == subnet.name) continue;
    if (ranges_overlap(range, parse_range(other.cidr))) return "the subnet's range overlaps subnet " + other.name + "'s";
  }
  return std::nullopt;
}

void RouteTable::validate() const {
  require_name("route table project", project);
  require_name("route table vpc", vpc);
  require_name("route table name", name);
  require(routes.size() <= max_routes, "route table has more than " + std::to_string(max_routes) + " routes");
  std::set<std::string> destinations;
  for (const auto& r : routes) {
    require_range("route destination", r.destination);
    require_name("route gateway", r.gateway);
    require(destinations.insert(r.destination).second, "route table names one destination twice");
  }
}

std::optional<std::string> check_route_table(const Vpc& vpc, const RouteTable& table) {
  if (table.project != vpc.project || table.vpc != vpc.name) return "the route table is not of this VPC";
  const auto local = parse_range(vpc.cidr);
  // Ranges nest or are apart, so a route either holds the VPC's (a default
  // route does, and local still wins by its longer prefix) or lies inside it,
  // which would take from local what is local.
  for (const auto& r : table.routes) {
    if (range_within(parse_range(r.destination), local)) return "route " + r.destination + " lies inside the VPC's own range, which is local";
  }
  return std::nullopt;
}

RouteTo route(const Vpc& vpc, const RouteTable& table, const boost::asio::ip::address_v4& destination) {
  const boost::asio::ip::address address(destination);
  if (parse_range(vpc.cidr).contains(address)) return RouteTo{RouteTo::Kind::local, {}};
  RouteTo best;
  int best_length = -1;
  for (const auto& r : table.routes) {
    const auto range = parse_range(r.destination);
    if (range.contains(address) && range.length > best_length) {
      best = RouteTo{RouteTo::Kind::gateway, r.gateway};
      best_length = range.length;
    }
  }
  return best;
}

void SecurityGroup::validate() const {
  require_name("security group project", project);
  require_name("security group vpc", vpc);
  require_name("security group name", name);
  require(description.size() <= max_description && description.find('\0') == std::string::npos,
          "security group description is not text of at most " + std::to_string(max_description) + " bytes");
  validate_rules("security group ingress", ingress);
  validate_rules("security group egress", egress);
}

}  // namespace louhi::model
