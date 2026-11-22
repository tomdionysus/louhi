//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/model/codec.h"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "louhi/model/image.h"
#include "louhi/model/names.h"

namespace louhi::model {

namespace {

using dannan::net::Reader;
using dannan::net::Writer;

Writer begin_record(Kind kind) {
  Writer writer;
  writer.u8(static_cast<uint8_t>(kind));
  writer.u8(format_version);
  return writer;
}

void expect(Reader& reader, Kind kind) {
  if (reader.u8() != static_cast<uint8_t>(kind)) throw DecodeError("record is not of the kind expected");
  if (reader.u8() != format_version) throw DecodeError("record format version is not " + std::to_string(format_version));
}

// The checks every decoder ends with: nothing left over, and a valid record.
template <class Record>
Record end_record(const Reader& reader, Record record) {
  reader.finish();
  try {
    record.validate();
  } catch (const std::invalid_argument& invalid) {
    throw DecodeError(std::string("record is not valid: ") + invalid.what());
  }
  return record;
}

void put_bool(Writer& writer, bool value) { writer.u8(value ? 1 : 0); }

bool get_bool(Reader& reader) {
  const uint8_t value = reader.u8();
  if (value > 1) throw DecodeError("flag byte is not 0 or 1");
  return value == 1;
}

// An enum's byte, which must be one of first..last.
template <class Enum>
Enum get_enum(Reader& reader, Enum first, Enum last) {
  const uint8_t value = reader.u8();
  if (value < static_cast<uint8_t>(first) || value > static_cast<uint8_t>(last)) throw DecodeError("enum byte is not one the format defines");
  return static_cast<Enum>(value);
}

template <class Enum>
void put_enum(Writer& writer, Enum value) {
  writer.u8(static_cast<uint8_t>(value));
}

size_t get_count(Reader& reader, size_t maximum) {
  const uint32_t count = reader.u32();
  if (count > maximum) throw DecodeError("count is over its bound");
  return count;
}

void put_count(Writer& writer, size_t count) { writer.u32(static_cast<uint32_t>(count)); }

void put_strings(Writer& writer, const std::vector<std::string>& strings) {
  put_count(writer, strings.size());
  for (const auto& s : strings) writer.string(s);
}

std::vector<std::string> get_strings(Reader& reader, size_t maximum_count, size_t maximum_length) {
  const size_t count = get_count(reader, maximum_count);
  std::vector<std::string> out;
  out.reserve(count);
  for (size_t i = 0; i < count; ++i) out.push_back(reader.string(maximum_length));
  return out;
}

void put_node(Writer& writer, const NodeId& node) { writer.fixed(node.bytes); }
NodeId get_node(Reader& reader) { return NodeId{reader.fixed<16>()}; }
void put_task_id(Writer& writer, const TaskId& id) { writer.fixed(id.bytes); }
TaskId get_task_id(Reader& reader) { return TaskId{reader.fixed<16>()}; }

void put_capacity(Writer& writer, const Capacity& capacity) {
  writer.u32(capacity.cpu_millis);
  writer.u32(capacity.memory_mib);
}

Capacity get_capacity(Reader& reader) {
  Capacity capacity;
  capacity.cpu_millis = reader.u32();
  capacity.memory_mib = reader.u32();
  return capacity;
}

void put_container(Writer& writer, const ContainerDefinition& c) {
  writer.string(c.name);
  writer.string(c.image);
  writer.string(c.image_digest);
  put_strings(writer, c.entry_point);
  put_strings(writer, c.command);
  put_count(writer, c.environment.size());
  for (const auto& variable : c.environment) {
    writer.string(variable.name);
    writer.string(variable.value);
  }
  writer.string(c.working_directory);
  put_count(writer, c.ports.size());
  for (const auto& port : c.ports) {
    writer.u16(port.container_port);
    put_enum(writer, port.protocol);
  }
  put_count(writer, c.mounts.size());
  for (const auto& mount : c.mounts) {
    writer.string(mount.volume);
    writer.string(mount.container_path);
    put_bool(writer, mount.read_only);
  }
  put_bool(writer, c.health_check.has_value());
  if (c.health_check) {
    put_strings(writer, c.health_check->command);
    writer.u16(c.health_check->interval_s);
    writer.u16(c.health_check->timeout_s);
    writer.u16(c.health_check->retries);
    writer.u16(c.health_check->start_period_s);
  }
  writer.u32(c.cpu_millis);
  writer.u32(c.memory_mib);
  put_bool(writer, c.essential);
  put_bool(writer, c.read_only_root);
}

ContainerDefinition get_container(Reader& reader) {
  ContainerDefinition c;
  c.name = reader.string(max_name);
  c.image = reader.string(max_image_reference);
  c.image_digest = reader.string(71);
  c.entry_point = get_strings(reader, max_arguments, max_argument);
  c.command = get_strings(reader, max_arguments, max_argument);
  const size_t variables = get_count(reader, max_environment);
  c.environment.reserve(variables);
  for (size_t i = 0; i < variables; ++i) {
    EnvVar variable;
    variable.name = reader.string(max_env_name);
    variable.value = reader.string(max_env_value);
    c.environment.push_back(std::move(variable));
  }
  c.working_directory = reader.string(max_path);
  const size_t ports = get_count(reader, max_ports);
  c.ports.reserve(ports);
  for (size_t i = 0; i < ports; ++i) {
    PortMapping port;
    port.container_port = reader.u16();
    port.protocol = get_enum(reader, Protocol::tcp, Protocol::udp);
    c.ports.push_back(port);
  }
  const size_t mounts = get_count(reader, max_mounts);
  c.mounts.reserve(mounts);
  for (size_t i = 0; i < mounts; ++i) {
    Mount mount;
    mount.volume = reader.string(max_name);
    mount.container_path = reader.string(max_path);
    mount.read_only = get_bool(reader);
    c.mounts.push_back(std::move(mount));
  }
  if (get_bool(reader)) {
    HealthCheck check;
    check.command = get_strings(reader, max_arguments, max_argument);
    check.interval_s = reader.u16();
    check.timeout_s = reader.u16();
    check.retries = reader.u16();
    check.start_period_s = reader.u16();
    c.health_check = std::move(check);
  }
  c.cpu_millis = reader.u32();
  c.memory_mib = reader.u32();
  c.essential = get_bool(reader);
  c.read_only_root = get_bool(reader);
  return c;
}

}  // namespace

Kind peek_kind(std::span<const uint8_t> bytes) {
  if (bytes.size() < 2) throw DecodeError("record is too short to have a kind");
  if (bytes[0] < static_cast<uint8_t>(Kind::project) || bytes[0] > static_cast<uint8_t>(Kind::security_group))
    throw DecodeError("record kind is not one this build knows");
  return static_cast<Kind>(bytes[0]);
}

Bytes encode(const Project& record) {
  record.validate();
  Writer writer = begin_record(Kind::project);
  writer.string(record.name);
  writer.string(record.description);
  writer.u64(record.created_unix_ms);
  return writer.take();
}

Project decode_project(std::span<const uint8_t> bytes) {
  Reader reader(bytes);
  expect(reader, Kind::project);
  Project record;
  record.name = reader.string(max_name);
  record.description = reader.string(max_description);
  record.created_unix_ms = reader.u64();
  return end_record(reader, std::move(record));
}

Bytes encode(const Quota& record) {
  record.validate();
  Writer writer = begin_record(Kind::quota);
  writer.string(record.project);
  writer.u32(record.max_tasks);
  writer.u32(record.max_cpu_millis);
  writer.u32(record.max_memory_mib);
  writer.u32(record.max_addresses);
  writer.u32(record.max_volumes);
  writer.u32(record.max_load_balancers);
  writer.u64(record.updated_unix_ms);
  return writer.take();
}

Quota decode_quota(std::span<const uint8_t> bytes) {
  Reader reader(bytes);
  expect(reader, Kind::quota);
  Quota record;
  record.project = reader.string(max_name);
  record.max_tasks = reader.u32();
  record.max_cpu_millis = reader.u32();
  record.max_memory_mib = reader.u32();
  record.max_addresses = reader.u32();
  record.max_volumes = reader.u32();
  record.max_load_balancers = reader.u32();
  record.updated_unix_ms = reader.u64();
  return end_record(reader, std::move(record));
}

Bytes encode(const TaskDefinition& record) {
  record.validate();
  Writer writer = begin_record(Kind::task_definition);
  writer.string(record.project);
  writer.string(record.family);
  writer.u32(record.revision);
  writer.u32(record.cpu_millis);
  writer.u32(record.memory_mib);
  writer.u32(record.ephemeral_storage_mib);
  put_count(writer, record.containers.size());
  for (const auto& container : record.containers) put_container(writer, container);
  put_count(writer, record.volumes.size());
  for (const auto& volume : record.volumes) {
    writer.string(volume.name);
    put_enum(writer, volume.kind);
    writer.u32(volume.size_mib);
    // Only a host volume carries a directory: the scratch kinds keep the
    // layout they always had, and a decoder that does not know host refuses
    // its kind before it would misread the rest.
    if (volume.kind == VolumeKind::host) {
      writer.string(volume.host_directory);
      writer.string(volume.host_path);
    }
  }
  put_bool(writer, record.run_once);
  writer.u64(record.registered_unix_ms);
  return writer.take();
}

TaskDefinition decode_task_definition(std::span<const uint8_t> bytes) {
  Reader reader(bytes);
  expect(reader, Kind::task_definition);
  TaskDefinition record;
  record.project = reader.string(max_name);
  record.family = reader.string(max_name);
  record.revision = reader.u32();
  record.cpu_millis = reader.u32();
  record.memory_mib = reader.u32();
  record.ephemeral_storage_mib = reader.u32();
  const size_t containers = get_count(reader, max_containers);
  record.containers.reserve(containers);
  for (size_t i = 0; i < containers; ++i) record.containers.push_back(get_container(reader));
  const size_t volumes = get_count(reader, max_volumes);
  record.volumes.reserve(volumes);
  for (size_t i = 0; i < volumes; ++i) {
    VolumeDefinition volume;
    volume.name = reader.string(max_name);
    volume.kind = get_enum(reader, VolumeKind::memory, VolumeKind::output);
    volume.size_mib = reader.u32();
    if (volume.kind == VolumeKind::host) {
      volume.host_directory = reader.string(max_name);
      volume.host_path = reader.string(max_path);
    }
    record.volumes.push_back(std::move(volume));
  }
  record.run_once = get_bool(reader);
  record.registered_unix_ms = reader.u64();
  return end_record(reader, std::move(record));
}

Bytes encode(const Service& record) {
  record.validate();
  Writer writer = begin_record(Kind::service);
  writer.string(record.project);
  writer.string(record.name);
  writer.string(record.family);
  writer.u32(record.revision);
  writer.u32(record.desired_count);
  writer.u16(record.deployment.minimum_healthy_percent);
  writer.u16(record.deployment.maximum_percent);
  put_bool(writer, record.spread_failure_domains);
  writer.u64(record.updated_unix_ms);
  return writer.take();
}

Service decode_service(std::span<const uint8_t> bytes) {
  Reader reader(bytes);
  expect(reader, Kind::service);
  Service record;
  record.project = reader.string(max_name);
  record.name = reader.string(max_name);
  record.family = reader.string(max_name);
  record.revision = reader.u32();
  record.desired_count = reader.u32();
  record.deployment.minimum_healthy_percent = reader.u16();
  record.deployment.maximum_percent = reader.u16();
  record.spread_failure_domains = get_bool(reader);
  record.updated_unix_ms = reader.u64();
  return end_record(reader, std::move(record));
}

Bytes encode(const Task& record) {
  record.validate();
  Writer writer = begin_record(Kind::task);
  put_task_id(writer, record.id);
  writer.string(record.project);
  writer.string(record.family);
  writer.u32(record.revision);
  writer.string(record.service);
  put_enum(writer, record.desired);
  writer.string(record.stop_reason);
  put_bool(writer, record.node.has_value());
  if (record.node) put_node(writer, *record.node);
  writer.u64(record.placement_epoch);
  writer.string(record.started_by);
  writer.u64(record.created_unix_ms);
  return writer.take();
}

Task decode_task(std::span<const uint8_t> bytes) {
  Reader reader(bytes);
  expect(reader, Kind::task);
  Task record;
  record.id = get_task_id(reader);
  record.project = reader.string(max_name);
  record.family = reader.string(max_name);
  record.revision = reader.u32();
  record.service = reader.string(max_name);
  record.desired = get_enum(reader, DesiredState::running, DesiredState::stopped);
  record.stop_reason = reader.string(max_reason);
  if (get_bool(reader)) record.node = get_node(reader);
  record.placement_epoch = reader.u64();
  record.started_by = reader.string(max_principal);
  record.created_unix_ms = reader.u64();
  return end_record(reader, std::move(record));
}

Bytes encode(const TaskStatus& record) {
  record.validate();
  Writer writer = begin_record(Kind::task_status);
  put_task_id(writer, record.task);
  put_node(writer, record.node);
  writer.u64(record.sequence);
  put_enum(writer, record.state);
  writer.string(record.reason);
  put_count(writer, record.containers.size());
  for (const auto& container : record.containers) {
    writer.string(container.name);
    put_enum(writer, container.state);
    put_bool(writer, container.exit_code.has_value());
    if (container.exit_code) writer.u32(static_cast<uint32_t>(*container.exit_code));
    writer.u32(container.restarts);
    put_enum(writer, container.health);
  }
  writer.u64(record.started_unix_ms);
  writer.u64(record.stopped_unix_ms);
  writer.u64(record.observed_unix_ms);
  return writer.take();
}

TaskStatus decode_task_status(std::span<const uint8_t> bytes) {
  Reader reader(bytes);
  expect(reader, Kind::task_status);
  TaskStatus record;
  record.task = get_task_id(reader);
  record.node = get_node(reader);
  record.sequence = reader.u64();
  record.state = get_enum(reader, TaskState::pending, TaskState::parked);
  record.reason = reader.string(max_reason);
  const size_t containers = get_count(reader, max_containers);
  record.containers.reserve(containers);
  for (size_t i = 0; i < containers; ++i) {
    ContainerStatus container;
    container.name = reader.string(max_name);
    container.state = get_enum(reader, ContainerState::created, ContainerState::exited);
    if (get_bool(reader)) container.exit_code = static_cast<int32_t>(reader.u32());
    container.restarts = reader.u32();
    container.health = get_enum(reader, Health::unknown, Health::unhealthy);
    record.containers.push_back(std::move(container));
  }
  record.started_unix_ms = reader.u64();
  record.stopped_unix_ms = reader.u64();
  record.observed_unix_ms = reader.u64();
  return end_record(reader, std::move(record));
}

Bytes encode(const NodeStatus& record) {
  record.validate();
  Writer writer = begin_record(Kind::node_status);
  put_node(writer, record.node);
  writer.u64(record.sequence);
  writer.string(record.louhi_version);
  writer.string(record.runtime);
  writer.string(record.runtime_version);
  put_capacity(writer, record.total);
  put_capacity(writer, record.allocated);
  writer.u32(record.tasks);
  put_bool(writer, record.ready);
  put_strings(writer, record.conditions);
  writer.u64(record.observed_unix_ms);
  return writer.take();
}

NodeStatus decode_node_status(std::span<const uint8_t> bytes) {
  Reader reader(bytes);
  expect(reader, Kind::node_status);
  NodeStatus record;
  record.node = get_node(reader);
  record.sequence = reader.u64();
  record.louhi_version = reader.string(max_version_text);
  record.runtime = reader.string(max_version_text);
  record.runtime_version = reader.string(max_version_text);
  record.total = get_capacity(reader);
  record.allocated = get_capacity(reader);
  record.tasks = reader.u32();
  record.ready = get_bool(reader);
  record.conditions = get_strings(reader, max_conditions, max_condition);
  record.observed_unix_ms = reader.u64();
  return end_record(reader, std::move(record));
}

namespace {

void put_rules(Writer& writer, const std::vector<SecurityRule>& rules) {
  put_count(writer, rules.size());
  for (const auto& r : rules) {
    put_enum(writer, r.protocol);
    writer.u16(r.port_from);
    writer.u16(r.port_to);
    writer.string(r.cidr);
    writer.string(r.group);
  }
}

std::vector<SecurityRule> get_rules(Reader& reader) {
  const size_t count = get_count(reader, max_security_rules);
  std::vector<SecurityRule> rules;
  rules.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    SecurityRule r;
    r.protocol = get_enum(reader, RuleProtocol::any, RuleProtocol::icmp);
    r.port_from = reader.u16();
    r.port_to = reader.u16();
    r.cidr = reader.string(max_range_text);
    r.group = reader.string(max_name);
    rules.push_back(std::move(r));
  }
  return rules;
}

}  // namespace

Bytes encode(const Vpc& record) {
  record.validate();
  Writer writer = begin_record(Kind::vpc);
  writer.string(record.project);
  writer.string(record.name);
  writer.string(record.cidr);
  writer.string(record.description);
  writer.u64(record.created_unix_ms);
  return writer.take();
}

Vpc decode_vpc(std::span<const uint8_t> bytes) {
  Reader reader(bytes);
  expect(reader, Kind::vpc);
  Vpc record;
  record.project = reader.string(max_name);
  record.name = reader.string(max_name);
  record.cidr = reader.string(max_range_text);
  record.description = reader.string(max_description);
  record.created_unix_ms = reader.u64();
  return end_record(reader, std::move(record));
}

Bytes encode(const Subnet& record) {
  record.validate();
  Writer writer = begin_record(Kind::subnet);
  writer.string(record.project);
  writer.string(record.vpc);
  writer.string(record.name);
  writer.string(record.cidr);
  writer.string(record.route_table);
  writer.u64(record.created_unix_ms);
  return writer.take();
}

Subnet decode_subnet(std::span<const uint8_t> bytes) {
  Reader reader(bytes);
  expect(reader, Kind::subnet);
  Subnet record;
  record.project = reader.string(max_name);
  record.vpc = reader.string(max_name);
  record.name = reader.string(max_name);
  record.cidr = reader.string(max_range_text);
  record.route_table = reader.string(max_name);
  record.created_unix_ms = reader.u64();
  return end_record(reader, std::move(record));
}

Bytes encode(const RouteTable& record) {
  record.validate();
  Writer writer = begin_record(Kind::route_table);
  writer.string(record.project);
  writer.string(record.vpc);
  writer.string(record.name);
  put_count(writer, record.routes.size());
  for (const auto& r : record.routes) {
    writer.string(r.destination);
    writer.string(r.gateway);
  }
  writer.u64(record.updated_unix_ms);
  return writer.take();
}

RouteTable decode_route_table(std::span<const uint8_t> bytes) {
  Reader reader(bytes);
  expect(reader, Kind::route_table);
  RouteTable record;
  record.project = reader.string(max_name);
  record.vpc = reader.string(max_name);
  record.name = reader.string(max_name);
  const size_t count = get_count(reader, max_routes);
  record.routes.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    Route r;
    r.destination = reader.string(max_range_text);
    r.gateway = reader.string(max_name);
    record.routes.push_back(std::move(r));
  }
  record.updated_unix_ms = reader.u64();
  return end_record(reader, std::move(record));
}

Bytes encode(const SecurityGroup& record) {
  record.validate();
  Writer writer = begin_record(Kind::security_group);
  writer.string(record.project);
  writer.string(record.vpc);
  writer.string(record.name);
  writer.string(record.description);
  put_rules(writer, record.ingress);
  put_rules(writer, record.egress);
  writer.u64(record.updated_unix_ms);
  return writer.take();
}

SecurityGroup decode_security_group(std::span<const uint8_t> bytes) {
  Reader reader(bytes);
  expect(reader, Kind::security_group);
  SecurityGroup record;
  record.project = reader.string(max_name);
  record.vpc = reader.string(max_name);
  record.name = reader.string(max_name);
  record.description = reader.string(max_description);
  record.ingress = get_rules(reader);
  record.egress = get_rules(reader);
  record.updated_unix_ms = reader.u64();
  return end_record(reader, std::move(record));
}

}  // namespace louhi::model
