//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "louhi/runtime/driver.h"

// The Docker Engine API, as pure functions: the bodies Louhi sends, and
// parsers for what the daemon sends back. No I/O, so every rule here is tested
// without a daemon, and every parser is fuzzed.
//
// The daemon is local and trusted to run containers, but its replies are still
// bytes from outside the process: each parser is bounded, checks every type it
// reads, and throws MalformedReply for anything it does not understand.
//
// The hardened profile (design, "The runtime") is in create_container_body:
// no network of Docker's (Louhi attaches the task's own), no privilege, every
// capability dropped but a short list, no new privileges, Docker's default
// seccomp profile left in force, an init process, a private IPC namespace, no
// restart policy (the agent restarts), CPU, memory and process limits always
// set, no swap, and no host path ever mounted.
namespace louhi::runtime::docker {

// The API version Louhi speaks: Docker 20.10 and later.
inline constexpr std::string_view api_version = "1.41";

inline constexpr size_t max_reply_bytes = 8 * 1024 * 1024;
inline constexpr size_t max_stream_line = 64 * 1024;
inline constexpr size_t max_json_depth = 32;
inline constexpr size_t max_labels = 256;
inline constexpr size_t max_label_text = 4096;
inline constexpr size_t max_message = 512;
inline constexpr size_t max_containers_listed = 10000;

class MalformedReply : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// The capabilities a container keeps; every other is dropped. Docker's default
// set less NET_RAW, MKNOD, AUDIT_WRITE, SETFCAP, SETPCAP and SYS_CHROOT.
const std::vector<std::string>& kept_capabilities();

// Bodies. Throw std::invalid_argument for a spec with an empty name or image,
// or a limit of zero.
std::string create_container_body(const ContainerSpec& spec);
std::string create_volume_body(const VolumeSpec& spec);

// Query strings.
std::string url_encode(std::string_view text);
// "filters=..." selecting what carries the louhi.task label.
std::string louhi_filter();
// The same for the event stream, containers only.
std::string events_filter();

// Parsers.
std::string parse_created_id(std::string_view body);
std::vector<std::string> parse_container_ids(std::string_view body);
ContainerInfo parse_inspect(std::string_view body);
std::vector<VolumeInfo> parse_volumes(std::string_view body);
RuntimeInfo parse_runtime(std::string_view version_body, std::string_view info_body);
// The digest the daemon recorded for `reference` (from /images/<ref>/json):
// the reference's own digest when it has one, else the RepoDigests entry for
// the same name; empty when the daemon records none for that name.
std::string parse_image_digest(std::string_view inspect_body, std::string_view reference);
// One line of a pull's progress stream: the error it reports, if any.
std::optional<std::string> parse_pull_line(std::string_view line);
// One line of the event stream: an event about one of Louhi's containers, or
// nothing for any other event.
std::optional<ContainerEvent> parse_event(std::string_view line);
// The daemon's {"message": ...} on a failure, bounded; never throws, and gives
// an empty string for anything else.
std::string parse_error_message(std::string_view body) noexcept;
// RFC 3339 in UTC ("2026-11-15T17:39:37.123456789Z") to Unix milliseconds;
// Docker's zero time ("0001-01-01T00:00:00Z") is 0.
uint64_t parse_time(std::string_view text);

}  // namespace louhi::runtime::docker
