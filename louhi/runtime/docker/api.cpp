//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/runtime/docker/api.h"

#include <algorithm>
#include <boost/json.hpp>
#include <limits>

#include "dannan/encoding/percent.h"
#include "louhi/model/image.h"
#include "louhi/runtime/labels.h"

namespace louhi::runtime::docker {

namespace json = boost::json;

namespace {

[[noreturn]] void malformed(const std::string& why) { throw MalformedReply("docker reply: " + why); }

json::value parse_json(std::string_view text) {
  if (text.size() > max_reply_bytes) malformed("larger than " + std::to_string(max_reply_bytes) + " bytes");
  json::parse_options options;
  options.max_depth = max_json_depth;
  boost::system::error_code failed;
  json::value value = json::parse(json::string_view(text.data(), text.size()), failed, json::storage_ptr(), options);
  if (failed) malformed("not JSON");
  return value;
}

const json::object& as_object(const json::value& value, const char* what) {
  if (!value.is_object()) malformed(std::string(what) + " is not an object");
  return value.get_object();
}

const json::value* member(const json::object& object, std::string_view key) { return object.if_contains(json::string_view(key.data(), key.size())); }

std::string bounded(const json::string& text, const char* what, size_t maximum) {
  if (text.size() > maximum) malformed(std::string(what) + " is longer than " + std::to_string(maximum) + " bytes");
  return std::string(text.data(), text.size());
}

// A string member; absent or null is empty.
std::string string_member(const json::object& object, std::string_view key, size_t maximum = max_label_text) {
  const json::value* value = member(object, key);
  if (value == nullptr || value->is_null()) return {};
  if (!value->is_string()) malformed(std::string(key) + " is not a string");
  return bounded(value->get_string(), std::string(key).c_str(), maximum);
}

std::string required_string(const json::object& object, std::string_view key, size_t maximum = max_label_text) {
  const json::value* value = member(object, key);
  if (value == nullptr || !value->is_string()) malformed(std::string(key) + " is missing or not a string");
  return bounded(value->get_string(), std::string(key).c_str(), maximum);
}

int64_t integer_member(const json::object& object, std::string_view key) {
  const json::value* value = member(object, key);
  if (value == nullptr || value->is_null()) return 0;
  if (value->is_int64()) return value->get_int64();
  if (value->is_uint64() && value->get_uint64() <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return static_cast<int64_t>(value->get_uint64());
  malformed(std::string(key) + " is not an integer");
}

bool bool_member(const json::object& object, std::string_view key) {
  const json::value* value = member(object, key);
  if (value == nullptr || value->is_null()) return false;
  if (!value->is_bool()) malformed(std::string(key) + " is not a boolean");
  return value->get_bool();
}

const json::object* object_member(const json::object& object, std::string_view key) {
  const json::value* value = member(object, key);
  if (value == nullptr || value->is_null()) return nullptr;
  return &as_object(*value, std::string(key).c_str());
}

Labels labels_of(const json::object* object, bool louhi_only) {
  Labels labels;
  if (object == nullptr) return labels;
  if (object->size() > max_labels) malformed("more than " + std::to_string(max_labels) + " labels");
  for (const auto& entry : *object) {
    const std::string key(entry.key().data(), entry.key().size());
    if (key.size() > max_label_text) malformed("a label name is too long");
    if (louhi_only && key.rfind("louhi.", 0) != 0) continue;
    if (!entry.value().is_string()) malformed("a label value is not a string");
    labels.emplace(key, bounded(entry.value().get_string(), "a label value", max_label_text));
  }
  return labels;
}

json::object labels_json(const Labels& labels) {
  json::object out;
  for (const auto& [key, value] : labels) out[key] = value;
  return out;
}

json::array strings_json(const std::vector<std::string>& strings) {
  json::array out;
  for (const auto& s : strings) out.emplace_back(s);
  return out;
}

ContainerState state_of(const std::string& status) {
  if (status == "created") return ContainerState::created;
  if (status == "running") return ContainerState::running;
  if (status == "paused") return ContainerState::paused;
  if (status == "restarting") return ContainerState::restarting;
  if (status == "exited") return ContainerState::exited;
  if (status == "removing") return ContainerState::removing;
  if (status == "dead") return ContainerState::dead;
  malformed("container state is not one Docker defines");
}

Health health_of(const std::string& status) {
  if (status.empty() || status == "none") return Health::none;
  if (status == "starting") return Health::starting;
  if (status == "healthy") return Health::healthy;
  if (status == "unhealthy") return Health::unhealthy;
  malformed("health status is not one Docker defines");
}

std::optional<int32_t> exit_code_text(const std::string& text) {
  if (text.empty() || text.size() > 11) return std::nullopt;
  size_t i = 0;
  bool negative = false;
  if (text[0] == '-') {
    negative = true;
    i = 1;
    if (text.size() == 1) return std::nullopt;
  }
  int64_t value = 0;
  for (; i < text.size(); ++i) {
    if (text[i] < '0' || text[i] > '9') return std::nullopt;
    value = value * 10 + (text[i] - '0');
  }
  if (negative) value = -value;
  if (value < std::numeric_limits<int32_t>::min() || value > std::numeric_limits<int32_t>::max()) return std::nullopt;
  return static_cast<int32_t>(value);
}

int64_t nanoseconds(std::chrono::seconds seconds) { return static_cast<int64_t>(seconds.count()) * 1'000'000'000; }

// Days from 1970-01-01 to y-m-d in the proleptic Gregorian calendar.
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2 ? 1 : 0;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned shifted_month = m > 2 ? m - 3 : m + 9;
  const unsigned doy = (153 * shifted_month + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

unsigned digits(std::string_view text, size_t at, size_t count) {
  unsigned value = 0;
  for (size_t i = at; i < at + count; ++i) {
    if (text[i] < '0' || text[i] > '9') malformed("time is not RFC 3339");
    value = value * 10 + static_cast<unsigned>(text[i] - '0');
  }
  return value;
}

}  // namespace

const std::vector<std::string>& kept_capabilities() {
  static const std::vector<std::string> kept = {"CHOWN", "DAC_OVERRIDE", "FOWNER", "FSETID", "KILL", "NET_BIND_SERVICE", "SETGID", "SETUID"};
  return kept;
}

std::string create_container_body(const ContainerSpec& spec) {
  if (spec.name.empty()) throw std::invalid_argument("container spec has no name");
  if (spec.image.empty()) throw std::invalid_argument("container spec has no image");
  if (spec.cpu_millis == 0 || spec.memory_bytes == 0 || spec.pids_limit == 0) throw std::invalid_argument("container spec leaves a limit unset");

  json::object body;
  body["Image"] = spec.image;
  if (!spec.entry_point.empty()) body["Entrypoint"] = strings_json(spec.entry_point);
  if (!spec.command.empty()) body["Cmd"] = strings_json(spec.command);
  json::array env;
  for (const auto& [name, value] : spec.environment) env.emplace_back(name + "=" + value);
  body["Env"] = std::move(env);
  if (!spec.working_directory.empty()) body["WorkingDir"] = spec.working_directory;
  body["Labels"] = labels_json(spec.labels);
  if (spec.health_check) {
    json::array test;
    test.emplace_back("CMD");
    for (const auto& argument : spec.health_check->command) test.emplace_back(argument);
    json::object check;
    check["Test"] = std::move(test);
    check["Interval"] = nanoseconds(spec.health_check->interval);
    check["Timeout"] = nanoseconds(spec.health_check->timeout);
    check["Retries"] = spec.health_check->retries;
    check["StartPeriod"] = nanoseconds(spec.health_check->start_period);
    body["Healthcheck"] = std::move(check);
  }
  body["NetworkDisabled"] = true;

  json::object host;
  host["NetworkMode"] = "none";
  host["Privileged"] = false;
  host["ReadonlyRootfs"] = spec.read_only_root;
  host["CapDrop"] = json::array{"ALL"};
  host["CapAdd"] = strings_json(kept_capabilities());
  host["SecurityOpt"] = json::array{"no-new-privileges:true"};
  host["NanoCpus"] = static_cast<int64_t>(spec.cpu_millis) * 1'000'000;
  host["Memory"] = static_cast<int64_t>(spec.memory_bytes);
  host["MemorySwap"] = static_cast<int64_t>(spec.memory_bytes);
  host["PidsLimit"] = static_cast<int64_t>(spec.pids_limit);
  if (spec.storage_bytes != 0) host["StorageOpt"] = json::object{{"size", std::to_string(spec.storage_bytes)}};
  host["Init"] = true;
  host["IpcMode"] = "private";
  host["RestartPolicy"] = json::object{{"Name", "no"}};
  host["AutoRemove"] = false;
  json::array mounts;
  for (const auto& mount : spec.mounts) {
    mounts.emplace_back(json::object{{"Type", "volume"}, {"Source", mount.volume}, {"Target", mount.target}, {"ReadOnly", mount.read_only}});
  }
  host["Mounts"] = std::move(mounts);
  body["HostConfig"] = std::move(host);
  return json::serialize(body);
}

std::string create_volume_body(const VolumeSpec& spec, const std::string& device) {
  if (spec.name.empty()) throw std::invalid_argument("volume spec has no name");
  if (spec.kind == VolumeKind::memory && spec.size_bytes == 0) throw std::invalid_argument("memory volume spec has no size");
  json::object body;
  body["Name"] = spec.name;
  body["Driver"] = "local";
  if (spec.kind == VolumeKind::host || spec.kind == VolumeKind::output) {
    // A bind of a directory the driver has resolved or made: never set-uid,
    // never a device, and a host directory never writable.
    if (device.empty() || device.front() != '/') throw std::invalid_argument("volume spec binds no absolute host directory");
    body["DriverOpts"] =
        json::object{{"type", "none"}, {"device", device}, {"o", spec.kind == VolumeKind::host ? "bind,ro,nosuid,nodev" : "bind,nosuid,nodev"}};
  } else if (spec.kind == VolumeKind::memory) {
    body["DriverOpts"] = json::object{{"type", "tmpfs"}, {"device", "tmpfs"}, {"o", "size=" + std::to_string(spec.size_bytes)}};
  } else if (spec.size_bytes != 0) {
    body["DriverOpts"] = json::object{{"size", std::to_string(spec.size_bytes)}};
  }
  body["Labels"] = labels_json(spec.labels);
  return json::serialize(body);
}

std::string registry_auth(const RegistryCredential& credential, const std::string& domain) {
  json::object auth;
  if (!credential.identity_token.empty()) {
    auth["identitytoken"] = credential.identity_token;
  } else {
    auth["username"] = credential.username;
    auth["password"] = credential.password;
  }
  auth["serveraddress"] = domain;
  const std::string text = json::serialize(auth);

  static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  out.reserve((text.size() + 2) / 3 * 4);
  for (size_t i = 0; i < text.size(); i += 3) {
    const size_t left = text.size() - i;
    uint32_t group = static_cast<uint32_t>(static_cast<unsigned char>(text[i])) << 16;
    if (left > 1) group |= static_cast<uint32_t>(static_cast<unsigned char>(text[i + 1])) << 8;
    if (left > 2) group |= static_cast<uint32_t>(static_cast<unsigned char>(text[i + 2]));
    out.push_back(alphabet[(group >> 18) & 0x3f]);
    out.push_back(alphabet[(group >> 12) & 0x3f]);
    out.push_back(left > 1 ? alphabet[(group >> 6) & 0x3f] : '=');
    out.push_back(left > 2 ? alphabet[group & 0x3f] : '=');
  }
  return out;
}

std::string url_encode(std::string_view text) { return dannan::encoding::percent_encode(text); }

std::string louhi_filter() {
  json::object filters;
  filters["label"] = json::array{std::string(label_task)};
  return "filters=" + url_encode(json::serialize(filters));
}

std::string events_filter() {
  json::object filters;
  filters["type"] = json::array{"container"};
  filters["label"] = json::array{std::string(label_task)};
  return "filters=" + url_encode(json::serialize(filters));
}

std::string parse_created_id(std::string_view body) {
  const json::value value = parse_json(body);
  const std::string id = required_string(as_object(value, "reply"), "Id", 128);
  if (id.empty()) malformed("Id is empty");
  return id;
}

std::vector<std::string> parse_container_ids(std::string_view body) {
  const json::value value = parse_json(body);
  if (!value.is_array()) malformed("container list is not an array");
  const json::array& list = value.get_array();
  if (list.size() > max_containers_listed) malformed("more than " + std::to_string(max_containers_listed) + " containers");
  std::vector<std::string> ids;
  ids.reserve(list.size());
  for (const auto& entry : list) ids.push_back(required_string(as_object(entry, "container"), "Id", 128));
  return ids;
}

ContainerInfo parse_inspect(std::string_view body) {
  const json::value value = parse_json(body);
  const json::object& root = as_object(value, "reply");
  ContainerInfo info;
  info.id = required_string(root, "Id", 128);
  info.name = string_member(root, "Name", 256);
  if (!info.name.empty() && info.name.front() == '/') info.name.erase(0, 1);
  if (const json::object* config = object_member(root, "Config")) {
    info.image = string_member(*config, "Image", 1024);
    info.labels = labels_of(object_member(*config, "Labels"), false);
  }
  const json::object* state = object_member(root, "State");
  if (state == nullptr) malformed("State is missing");
  info.state = state_of(required_string(*state, "Status", 32));
  if (info.state == ContainerState::exited || info.state == ContainerState::dead) {
    const int64_t code = integer_member(*state, "ExitCode");
    if (code < std::numeric_limits<int32_t>::min() || code > std::numeric_limits<int32_t>::max()) malformed("ExitCode is out of range");
    info.exit_code = static_cast<int32_t>(code);
  }
  info.oom_killed = bool_member(*state, "OOMKilled");
  if (const json::object* health = object_member(*state, "Health")) info.health = health_of(string_member(*health, "Status", 32));
  info.started_unix_ms = parse_time(string_member(*state, "StartedAt", 64));
  info.finished_unix_ms = parse_time(string_member(*state, "FinishedAt", 64));
  return info;
}

std::vector<VolumeInfo> parse_volumes(std::string_view body) {
  const json::value value = parse_json(body);
  const json::object& root = as_object(value, "reply");
  std::vector<VolumeInfo> volumes;
  const json::value* list = member(root, "Volumes");
  if (list == nullptr || list->is_null()) return volumes;
  if (!list->is_array()) malformed("Volumes is not an array");
  if (list->get_array().size() > max_containers_listed) malformed("too many volumes");
  for (const auto& entry : list->get_array()) {
    const json::object& volume = as_object(entry, "volume");
    volumes.push_back({required_string(volume, "Name", 256), labels_of(object_member(volume, "Labels"), false)});
  }
  return volumes;
}

RuntimeInfo parse_runtime(std::string_view version_body, std::string_view info_body) {
  const json::value version = parse_json(version_body);
  const json::object& v = as_object(version, "version");
  RuntimeInfo info;
  info.name = "docker";
  info.version = required_string(v, "Version", 64);
  info.api_version = required_string(v, "ApiVersion", 16);

  const json::value system = parse_json(info_body);
  const json::object& s = as_object(system, "info");
  if (const json::value* options = member(s, "SecurityOptions"); options != nullptr && !options->is_null()) {
    if (!options->is_array()) malformed("SecurityOptions is not an array");
    for (const auto& option : options->get_array()) {
      if (!option.is_string()) malformed("a security option is not a string");
      const std::string text = bounded(option.get_string(), "a security option", 1024);
      if (text == "name=userns" || text.rfind("name=userns,", 0) == 0) info.user_namespaces = true;
    }
  }
  const std::string cgroup = string_member(s, "CgroupVersion", 8);
  info.cgroup_version = cgroup == "2" ? 2 : cgroup == "1" ? 1 : 0;
  info.limits_memory = bool_member(s, "MemoryLimit");
  info.limits_swap = bool_member(s, "SwapLimit");
  info.limits_cpu = bool_member(s, "CpuCfsQuota");
  info.limits_pids = bool_member(s, "PidsLimit");
  // DriverStatus is a list of [name, value] pairs.
  if (string_member(s, "Driver", 64) == "overlay2") {
    if (const json::value* status = member(s, "DriverStatus"); status != nullptr && !status->is_null()) {
      if (!status->is_array()) malformed("DriverStatus is not an array");
      if (status->get_array().size() > max_labels) malformed("DriverStatus is too long");
      for (const auto& pair : status->get_array()) {
        if (!pair.is_array() || pair.get_array().size() != 2 || !pair.get_array()[0].is_string() || !pair.get_array()[1].is_string())
          malformed("a DriverStatus entry is not a pair of strings");
        const std::string name = bounded(pair.get_array()[0].get_string(), "a DriverStatus name", 256);
        const std::string value = bounded(pair.get_array()[1].get_string(), "a DriverStatus value", 1024);
        if (name == "Backing Filesystem" && value == "xfs") info.limits_storage = true;
      }
    }
  }
  // With user namespaces the daemon keeps its state under a directory named
  // for the remapped root, ".../100000.100000".
  if (info.user_namespaces) {
    const std::string root = string_member(s, "DockerRootDir", 4096);
    const std::string last = root.substr(root.rfind('/') + 1);
    const auto dot = last.find('.');
    const auto number = [](const std::string& text) -> std::optional<uint32_t> {
      if (text.empty() || text.size() > 10 || text.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
      const uint64_t n = std::stoull(text);
      if (n > std::numeric_limits<uint32_t>::max()) return std::nullopt;
      return static_cast<uint32_t>(n);
    };
    if (dot != std::string::npos) {
      const auto uid = number(last.substr(0, dot));
      const auto gid = number(last.substr(dot + 1));
      if (uid && gid) {
        info.root_uid = *uid;
        info.root_gid = *gid;
      }
    }
  }
  const int64_t cpus = integer_member(s, "NCPU");
  if (cpus < 0 || cpus > 1'000'000) malformed("NCPU is out of range");
  info.cpus = static_cast<uint32_t>(cpus);
  const int64_t memory = integer_member(s, "MemTotal");
  if (memory < 0) malformed("MemTotal is negative");
  info.memory_bytes = static_cast<uint64_t>(memory);
  return info;
}

std::string parse_image_digest(std::string_view inspect_body, std::string_view reference) {
  const model::ImageReference wanted = model::parse_image_reference(reference);
  if (!wanted.digest.empty()) return wanted.digest;
  const std::string name = wanted.domain.empty() ? wanted.path : wanted.domain + "/" + wanted.path;

  const json::value value = parse_json(inspect_body);
  const json::object& root = as_object(value, "image");
  const json::value* digests = member(root, "RepoDigests");
  if (digests == nullptr || digests->is_null()) return {};
  if (!digests->is_array()) malformed("RepoDigests is not an array");
  for (const auto& entry : digests->get_array()) {
    if (!entry.is_string()) malformed("a repo digest is not a string");
    const std::string text = bounded(entry.get_string(), "a repo digest", 1024);
    const size_t at = text.find('@');
    if (at == std::string::npos) continue;
    if (text.compare(0, at, name) == 0 && at == name.size() && model::is_digest(std::string_view(text).substr(at + 1))) return text.substr(at + 1);
  }
  return {};
}

std::string parse_distribution(std::string_view body) {
  const json::value value = parse_json(body);
  const json::object& root = as_object(value, "distribution");
  const json::object* descriptor = object_member(root, "Descriptor");
  if (descriptor == nullptr) malformed("Descriptor is missing");
  const std::string digest = required_string(*descriptor, "digest", 128);
  if (!model::is_digest(digest)) malformed("the descriptor's digest is not sha256 and 64 lowercase hex digits");
  return digest;
}

std::optional<std::string> parse_pull_line(std::string_view line) {
  if (line.size() > max_stream_line) malformed("a stream line is too long");
  const json::value value = parse_json(line);
  const json::object& root = as_object(value, "progress");
  if (member(root, "error") == nullptr && member(root, "errorDetail") == nullptr) return std::nullopt;
  std::string message = string_member(root, "error", max_message);
  if (message.empty()) {
    if (const json::object* detail = object_member(root, "errorDetail")) message = string_member(*detail, "message", max_message);
  }
  return message.empty() ? std::string("pull failed") : message;
}

std::optional<ContainerEvent> parse_event(std::string_view line) {
  if (line.size() > max_stream_line) malformed("a stream line is too long");
  const json::value value = parse_json(line);
  const json::object& root = as_object(value, "event");
  if (string_member(root, "Type", 32) != "container") return std::nullopt;
  const json::object* actor = object_member(root, "Actor");
  if (actor == nullptr) return std::nullopt;
  const json::object* attributes = object_member(*actor, "Attributes");
  ContainerEvent event;
  event.id = required_string(*actor, "ID", 128);
  std::string action = required_string(root, "Action", 256);
  if (const size_t colon = action.find(':'); colon != std::string::npos) action.erase(colon);
  event.action = action;
  event.labels = labels_of(attributes, true);
  if (event.labels.count(std::string(label_task)) == 0) return std::nullopt;
  if (attributes != nullptr) event.exit_code = exit_code_text(string_member(*attributes, "exitCode", 16));
  return event;
}

std::string parse_error_message(std::string_view body) noexcept {
  try {
    if (body.size() > max_reply_bytes) return {};
    boost::system::error_code failed;
    json::parse_options options;
    options.max_depth = max_json_depth;
    const json::value value = json::parse(json::string_view(body.data(), body.size()), failed, json::storage_ptr(), options);
    if (failed || !value.is_object()) return {};
    const json::value* message = value.get_object().if_contains("message");
    if (message == nullptr || !message->is_string()) return {};
    const json::string& text = message->get_string();
    return std::string(text.data(), std::min(text.size(), max_message));
  } catch (...) {
    return {};
  }
}

uint64_t parse_time(std::string_view text) {
  if (text.empty()) return 0;
  // YYYY-MM-DDTHH:MM:SS[.fraction]Z
  if (text.size() < 20 || text.size() > 35 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':' || text.back() != 'Z') {
    malformed("time is not RFC 3339 in UTC");
  }
  const unsigned year = digits(text, 0, 4);
  const unsigned month = digits(text, 5, 2);
  const unsigned day = digits(text, 8, 2);
  const unsigned hour = digits(text, 11, 2);
  const unsigned minute = digits(text, 14, 2);
  const unsigned second = digits(text, 17, 2);
  unsigned millis = 0;
  if (text.size() > 20) {
    if (text[19] != '.' || text.size() < 22) malformed("time fraction is not RFC 3339");
    const size_t fraction = text.size() - 21;
    if (fraction > 9) malformed("time fraction has more than nine digits");
    digits(text, 20, fraction);
    for (size_t i = 0; i < 3; ++i) millis = millis * 10 + (i < fraction ? static_cast<unsigned>(text[20 + i] - '0') : 0);
  }
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) malformed("time field is out of range");
  if (year < 1970) return 0;
  const int64_t days = days_from_civil(year, month, day);
  return static_cast<uint64_t>(((days * 24 + hour) * 60 + minute) * 60 + second) * 1000 + millis;
}

}  // namespace louhi::runtime::docker
