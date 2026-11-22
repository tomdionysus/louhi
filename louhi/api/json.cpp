//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/api/json.h"

#include <boost/json.hpp>
#include <limits>
#include <set>
#include <utility>
#include <vector>

#include "louhi/model/names.h"

namespace louhi::api {

namespace json = boost::json;

namespace {

[[noreturn]] void invalid(const std::string& why) { throw Invalid(why); }

// One object of a request: its members read by name, and at the end any it
// holds that the request does not define refused.
class Members {
 public:
  Members(const json::object& object, std::string where) : _object(object), _where(std::move(where)) {}

  const json::value* get(std::string_view key) {
    _known.emplace(key);
    const json::value* value = _object.if_contains(json::string_view(key.data(), key.size()));
    return value == nullptr || value->is_null() ? nullptr : value;
  }

  std::string string(std::string_view key, size_t maximum, bool required = false) {
    const json::value* value = get(key);
    if (value == nullptr) {
      if (required) invalid(name(key) + " is missing");
      return {};
    }
    if (!value->is_string()) invalid(name(key) + " is not a string");
    const auto& text = value->get_string();
    if (text.size() > maximum) invalid(name(key) + " is longer than " + std::to_string(maximum) + " bytes");
    return std::string(text.data(), text.size());
  }

  std::optional<uint64_t> number(std::string_view key, uint64_t least, uint64_t most) {
    const json::value* value = get(key);
    if (value == nullptr) return std::nullopt;
    uint64_t n = 0;
    if (value->is_uint64())
      n = value->get_uint64();
    else if (value->is_int64() && value->get_int64() >= 0)
      n = static_cast<uint64_t>(value->get_int64());
    else
      invalid(name(key) + " is not a whole number of 0 or more");
    if (n < least || n > most) invalid(name(key) + " is not " + std::to_string(least) + " to " + std::to_string(most));
    return n;
  }

  template <typename T>
  T number_or(std::string_view key, uint64_t least, uint64_t most, T otherwise) {
    const auto n = number(key, least, most);
    return n ? static_cast<T>(*n) : otherwise;
  }

  bool boolean(std::string_view key, bool otherwise) {
    const json::value* value = get(key);
    if (value == nullptr) return otherwise;
    if (!value->is_bool()) invalid(name(key) + " is not true or false");
    return value->get_bool();
  }

  const json::array* array(std::string_view key, size_t maximum) {
    const json::value* value = get(key);
    if (value == nullptr) return nullptr;
    if (!value->is_array()) invalid(name(key) + " is not a list");
    if (value->get_array().size() > maximum) invalid(name(key) + " has more than " + std::to_string(maximum) + " entries");
    return &value->get_array();
  }

  const json::object* object(std::string_view key) {
    const json::value* value = get(key);
    if (value == nullptr) return nullptr;
    if (!value->is_object()) invalid(name(key) + " is not an object");
    return &value->get_object();
  }

  std::vector<std::string> strings(std::string_view key, size_t count, size_t length) {
    std::vector<std::string> out;
    const json::array* list = array(key, count);
    if (list == nullptr) return out;
    for (const auto& entry : *list) {
      if (!entry.is_string()) invalid(name(key) + " holds something that is not a string");
      if (entry.get_string().size() > length) invalid(name(key) + " holds a string longer than " + std::to_string(length) + " bytes");
      out.emplace_back(entry.get_string().data(), entry.get_string().size());
    }
    return out;
  }

  // Refuses a member the request does not define. Its name is not repeated:
  // it is the caller's text.
  void done() const {
    for (const auto& entry : _object)
      if (_known.count(std::string(entry.key().data(), entry.key().size())) == 0) invalid(_where + " has a member it does not define");
  }

  std::string name(std::string_view key) const { return _where + " " + std::string(key); }
  const std::string& where() const { return _where; }

 private:
  const json::object& _object;
  std::string _where;
  std::set<std::string, std::less<>> _known;
};

const json::object& as_object(const json::value& value, const std::string& what) {
  if (!value.is_object()) invalid(what + " is not an object");
  return value.get_object();
}

// The project or family a body names, which must be the path's when given.
void same_as_path(Members& members, std::string_view key, std::string_view path) {
  const std::string given = members.string(key, model::max_name);
  if (!given.empty() && given != path) invalid(members.name(key) + " is not the one the path names");
}

model::Protocol protocol_of(const std::string& text, const std::string& what) {
  if (text.empty() || text == "tcp") return model::Protocol::tcp;
  if (text == "udp") return model::Protocol::udp;
  invalid(what + " is not tcp or udp");
}

model::HealthCheck health_check_body(const json::object& object, const std::string& where) {
  Members m(object, where);
  model::HealthCheck out;
  out.command = m.strings("command", model::max_arguments, model::max_argument);
  out.interval_s = m.number_or<uint16_t>("interval_s", 0, 65535, out.interval_s);
  out.timeout_s = m.number_or<uint16_t>("timeout_s", 0, 65535, out.timeout_s);
  out.retries = m.number_or<uint16_t>("retries", 0, 65535, out.retries);
  out.start_period_s = m.number_or<uint16_t>("start_period_s", 0, 65535, out.start_period_s);
  m.done();
  return out;
}

model::ContainerDefinition container_body(const json::object& object, size_t index) {
  Members m(object, "containers[" + std::to_string(index) + "]");
  model::ContainerDefinition out;
  out.name = m.string("name", model::max_name, true);
  out.image = m.string("image", 4096, true);
  // The system's, resolved at registration; a described definition can be
  // registered again as is.
  m.get("image_digest");
  out.entry_point = m.strings("entry_point", model::max_arguments, model::max_argument);
  out.command = m.strings("command", model::max_arguments, model::max_argument);
  if (const json::array* environment = m.array("environment", model::max_environment)) {
    for (size_t i = 0; i < environment->size(); ++i) {
      Members e(as_object((*environment)[i], m.name("environment")), m.name("environment[" + std::to_string(i) + "]"));
      model::EnvVar variable;
      variable.name = e.string("name", model::max_env_name, true);
      variable.value = e.string("value", model::max_env_value);
      e.done();
      out.environment.push_back(std::move(variable));
    }
  }
  out.working_directory = m.string("working_directory", model::max_path);
  if (const json::array* ports = m.array("ports", model::max_ports)) {
    for (size_t i = 0; i < ports->size(); ++i) {
      Members p(as_object((*ports)[i], m.name("ports")), m.name("ports[" + std::to_string(i) + "]"));
      model::PortMapping port;
      port.container_port = p.number_or<uint16_t>("container_port", 1, 65535, 0);
      if (port.container_port == 0) invalid(p.name("container_port") + " is missing");
      port.protocol = protocol_of(p.string("protocol", 8), p.name("protocol"));
      p.done();
      out.ports.push_back(port);
    }
  }
  if (const json::array* mounts = m.array("mounts", model::max_mounts)) {
    for (size_t i = 0; i < mounts->size(); ++i) {
      Members v(as_object((*mounts)[i], m.name("mounts")), m.name("mounts[" + std::to_string(i) + "]"));
      model::Mount mount;
      mount.volume = v.string("volume", model::max_name, true);
      mount.container_path = v.string("container_path", model::max_path, true);
      mount.read_only = v.boolean("read_only", false);
      v.done();
      out.mounts.push_back(std::move(mount));
    }
  }
  if (const json::object* check = m.object("health_check")) out.health_check = health_check_body(*check, m.name("health_check"));
  out.cpu_millis = m.number_or<uint32_t>("cpu_millis", 0, model::max_task_cpu_millis, 0);
  out.memory_mib = m.number_or<uint32_t>("memory_mib", 0, model::max_task_memory_mib, 0);
  out.essential = m.boolean("essential", true);
  out.read_only_root = m.boolean("read_only_root", false);
  m.done();
  return out;
}

json::array strings_json(const std::vector<std::string>& strings) {
  json::array out;
  for (const auto& s : strings) out.emplace_back(s);
  return out;
}

const char* protocol_name(model::Protocol protocol) { return protocol == model::Protocol::udp ? "udp" : "tcp"; }

const char* task_state_name(model::TaskState state) {
  switch (state) {
    case model::TaskState::pending:
      return "pending";
    case model::TaskState::pulling:
      return "pulling";
    case model::TaskState::starting:
      return "starting";
    case model::TaskState::running:
      return "running";
    case model::TaskState::stopping:
      return "stopping";
    case model::TaskState::stopped:
      return "stopped";
    case model::TaskState::parked:
      return "parked";
  }
  return "unknown";
}

const char* container_state_name(model::ContainerState state) {
  switch (state) {
    case model::ContainerState::created:
      return "created";
    case model::ContainerState::running:
      return "running";
    case model::ContainerState::exited:
      return "exited";
  }
  return "unknown";
}

const char* health_name(model::Health health) {
  switch (health) {
    case model::Health::healthy:
      return "healthy";
    case model::Health::unhealthy:
      return "unhealthy";
    case model::Health::unknown:
      break;
  }
  return "unknown";
}

json::object capacity_json(const model::Capacity& capacity) { return {{"cpu_millis", capacity.cpu_millis}, {"memory_mib", capacity.memory_mib}}; }

}  // namespace

json::object parse_body(std::string_view text) {
  if (text.size() > max_body) invalid("the body is larger than " + std::to_string(max_body) + " bytes");
  if (text.empty()) return {};
  json::parse_options options;
  options.max_depth = max_depth;
  boost::system::error_code failed;
  json::value value = json::parse(json::string_view(text.data(), text.size()), failed, json::storage_ptr(), options);
  if (failed) invalid("the body is not JSON");
  if (!value.is_object()) invalid("the body is not an object");
  return std::move(value.get_object());
}

ProjectBody project_body(const json::object& body, std::string_view project) {
  Members m(body, "project");
  same_as_path(m, "name", project);
  ProjectBody out;
  out.description = m.string("description", model::max_description);
  if (out.description.find('\0') != std::string::npos) invalid("project description holds a NUL byte");
  m.get("created_unix_ms");  // The system's; a described project can be put back.
  m.done();
  return out;
}

model::Quota quota_body(const json::object& body, std::string_view project) {
  Members m(body, "quota");
  same_as_path(m, "project", project);
  model::Quota out;
  out.project = std::string(project);
  constexpr uint64_t most = std::numeric_limits<uint32_t>::max();
  out.max_tasks = m.number_or<uint32_t>("max_tasks", 0, most, 0);
  out.max_cpu_millis = m.number_or<uint32_t>("max_cpu_millis", 0, most, 0);
  out.max_memory_mib = m.number_or<uint32_t>("max_memory_mib", 0, most, 0);
  out.max_addresses = m.number_or<uint32_t>("max_addresses", 0, most, 0);
  out.max_volumes = m.number_or<uint32_t>("max_volumes", 0, most, 0);
  out.max_load_balancers = m.number_or<uint32_t>("max_load_balancers", 0, most, 0);
  m.get("updated_unix_ms");  // The system's; a described quota can be put back.
  m.done();
  try {
    out.validate();
  } catch (const std::invalid_argument& error) {
    invalid(error.what());
  }
  return out;
}

model::TaskDefinition task_definition_body(const json::object& body, std::string_view project) {
  Members m(body, "task definition");
  same_as_path(m, "project", project);
  model::TaskDefinition out;
  out.project = std::string(project);
  out.family = m.string("family", model::max_name, true);
  out.cpu_millis = m.number_or<uint32_t>("cpu_millis", 0, model::max_task_cpu_millis, 0);
  out.memory_mib = m.number_or<uint32_t>("memory_mib", 0, model::max_task_memory_mib, 0);
  out.ephemeral_storage_mib = m.number_or<uint32_t>("ephemeral_storage_mib", 0, model::max_ephemeral_storage_mib, default_ephemeral_storage_mib);
  if (const json::array* containers = m.array("containers", model::max_containers))
    for (size_t i = 0; i < containers->size(); ++i) out.containers.push_back(container_body(as_object((*containers)[i], "containers"), i));
  if (const json::array* volumes = m.array("volumes", model::max_volumes)) {
    for (size_t i = 0; i < volumes->size(); ++i) {
      Members v(as_object((*volumes)[i], "volumes"), "volumes[" + std::to_string(i) + "]");
      model::VolumeDefinition volume;
      volume.name = v.string("name", model::max_name, true);
      const std::string kind = v.string("kind", 16);
      if (kind == "disk") {
        volume.kind = model::VolumeKind::disk;
      } else if (kind == "host") {
        volume.kind = model::VolumeKind::host;
      } else if (kind == "output") {
        volume.kind = model::VolumeKind::output;
      } else if (kind.empty() || kind == "memory") {
        volume.kind = model::VolumeKind::memory;
      } else {
        invalid(v.name("kind") + " is not memory, disk, host or output");
      }
      volume.size_mib = v.number_or<uint32_t>("size_mib", 0, model::max_scratch_mib, 0);
      volume.host_directory = v.string("host_directory", model::max_name);
      volume.host_path = v.string("host_path", model::max_path);
      v.done();
      out.volumes.push_back(std::move(volume));
    }
  }
  out.run_once = m.boolean("run_once", false);
  // The system's; a described definition can be registered again as is.
  m.get("revision");
  m.get("registered_unix_ms");
  m.done();
  // Validated as the revision it will become.
  model::TaskDefinition check = out;
  check.revision = 1;
  try {
    check.validate();
  } catch (const std::invalid_argument& error) {
    invalid(error.what());
  }
  return out;
}

RunTaskBody run_task_body(const json::object& body) {
  Members m(body, "run task");
  RunTaskBody out;
  out.family = m.string("family", model::max_name, true);
  if (!model::is_name(out.family)) invalid("run task family is not a name");
  if (const auto revision = m.number("revision", 1, std::numeric_limits<uint32_t>::max())) out.revision = static_cast<uint32_t>(*revision);
  out.count = m.number_or<uint32_t>("count", 1, max_run_count, 1);
  out.client_token = m.string("client_token", max_client_token);
  m.done();
  return out;
}

StopTaskBody stop_task_body(const json::object& body) {
  Members m(body, "stop task");
  StopTaskBody out;
  out.reason = m.string("reason", model::max_reason);
  m.done();
  return out;
}

json::object to_json(const model::Project& record) {
  return {{"name", record.name}, {"description", record.description}, {"created_unix_ms", record.created_unix_ms}};
}

json::object to_json(const model::Quota& record) {
  return {{"project", record.project},
          {"max_tasks", record.max_tasks},
          {"max_cpu_millis", record.max_cpu_millis},
          {"max_memory_mib", record.max_memory_mib},
          {"max_addresses", record.max_addresses},
          {"max_volumes", record.max_volumes},
          {"max_load_balancers", record.max_load_balancers},
          {"updated_unix_ms", record.updated_unix_ms}};
}

json::object to_json(const model::TaskDefinition& record) {
  json::array containers;
  for (const auto& c : record.containers) {
    json::object container{{"name", c.name}, {"image", c.image}};
    if (!c.image_digest.empty()) container["image_digest"] = c.image_digest;
    if (!c.entry_point.empty()) container["entry_point"] = strings_json(c.entry_point);
    if (!c.command.empty()) container["command"] = strings_json(c.command);
    if (!c.environment.empty()) {
      json::array environment;
      for (const auto& variable : c.environment) environment.push_back(json::object{{"name", variable.name}, {"value", variable.value}});
      container["environment"] = std::move(environment);
    }
    if (!c.working_directory.empty()) container["working_directory"] = c.working_directory;
    if (!c.ports.empty()) {
      json::array ports;
      for (const auto& port : c.ports) ports.push_back(json::object{{"container_port", port.container_port}, {"protocol", protocol_name(port.protocol)}});
      container["ports"] = std::move(ports);
    }
    if (!c.mounts.empty()) {
      json::array mounts;
      for (const auto& mount : c.mounts)
        mounts.push_back(json::object{{"volume", mount.volume}, {"container_path", mount.container_path}, {"read_only", mount.read_only}});
      container["mounts"] = std::move(mounts);
    }
    if (c.health_check) {
      container["health_check"] = json::object{{"command", strings_json(c.health_check->command)},
                                               {"interval_s", c.health_check->interval_s},
                                               {"timeout_s", c.health_check->timeout_s},
                                               {"retries", c.health_check->retries},
                                               {"start_period_s", c.health_check->start_period_s}};
    }
    if (c.cpu_millis != 0) container["cpu_millis"] = c.cpu_millis;
    if (c.memory_mib != 0) container["memory_mib"] = c.memory_mib;
    container["essential"] = c.essential;
    container["read_only_root"] = c.read_only_root;
    containers.push_back(std::move(container));
  }
  json::array volumes;
  for (const auto& v : record.volumes) {
    const char* kind = v.kind == model::VolumeKind::disk     ? "disk"
                       : v.kind == model::VolumeKind::host   ? "host"
                       : v.kind == model::VolumeKind::output ? "output"
                                                             : "memory";
    json::object volume{{"name", v.name}, {"kind", kind}};
    if (v.kind == model::VolumeKind::host) {
      volume["host_directory"] = v.host_directory;
      volume["host_path"] = v.host_path;
    } else {
      volume["size_mib"] = v.size_mib;
    }
    volumes.push_back(std::move(volume));
  }
  return {{"project", record.project},           {"family", record.family},
          {"revision", record.revision},         {"cpu_millis", record.cpu_millis},
          {"memory_mib", record.memory_mib},     {"ephemeral_storage_mib", record.ephemeral_storage_mib},
          {"containers", std::move(containers)}, {"volumes", std::move(volumes)},
          {"run_once", record.run_once},         {"registered_unix_ms", record.registered_unix_ms}};
}

json::object to_json(const model::Task& record) {
  json::object out{{"id", model::to_string(record.id)},
                   {"project", record.project},
                   {"family", record.family},
                   {"revision", record.revision},
                   {"desired", record.desired == model::DesiredState::stopped ? "stopped" : "running"},
                   {"placement_epoch", record.placement_epoch},
                   {"started_by", record.started_by},
                   {"created_unix_ms", record.created_unix_ms}};
  if (!record.service.empty()) out["service"] = record.service;
  if (!record.stop_reason.empty()) out["stop_reason"] = record.stop_reason;
  if (record.node) out["node"] = dannan::identity::to_string(*record.node);
  return out;
}

json::object to_json(const model::TaskStatus& record) {
  json::array containers;
  for (const auto& c : record.containers) {
    json::object container{{"name", c.name}, {"state", container_state_name(c.state)}, {"restarts", c.restarts}, {"health", health_name(c.health)}};
    if (c.exit_code) container["exit_code"] = *c.exit_code;
    containers.push_back(std::move(container));
  }
  json::object out{{"task", model::to_string(record.task)},
                   {"node", dannan::identity::to_string(record.node)},
                   {"sequence", record.sequence},
                   {"state", task_state_name(record.state)},
                   {"containers", std::move(containers)},
                   {"started_unix_ms", record.started_unix_ms},
                   {"stopped_unix_ms", record.stopped_unix_ms},
                   {"observed_unix_ms", record.observed_unix_ms}};
  if (!record.reason.empty()) out["reason"] = record.reason;
  return out;
}

json::object to_json(const model::NodeStatus& record) {
  return {{"node", dannan::identity::to_string(record.node)},
          {"sequence", record.sequence},
          {"louhi_version", record.louhi_version},
          {"runtime", record.runtime},
          {"runtime_version", record.runtime_version},
          {"total", capacity_json(record.total)},
          {"allocated", capacity_json(record.allocated)},
          {"tasks", record.tasks},
          {"ready", record.ready},
          {"conditions", strings_json(record.conditions)},
          {"observed_unix_ms", record.observed_unix_ms}};
}

}  // namespace louhi::api
