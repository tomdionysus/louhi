//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Any bytes the Docker daemon could send: every reply parser either reads
// them or refuses them with MalformedReply, and none crashes or reads out of
// bounds.

#include <span>
#include <string_view>
#include <vector>

#include "dannan/fuzz/fuzz.h"
#include "louhi/runtime/docker/api.h"

namespace {

dannan::net::Bytes bytes_of(std::string_view text) { return dannan::net::Bytes(text.begin(), text.end()); }

const dannan::fuzz::Registration docker_replies(
    "docker_reply",
    [](std::span<const uint8_t> data) {
      using namespace louhi::runtime::docker;
      const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
      const auto attempt = [](auto parse) {
        try {
          parse();
        } catch (const MalformedReply&) {
        }
      };
      attempt([&] { parse_created_id(text); });
      attempt([&] { parse_container_ids(text); });
      attempt([&] { parse_inspect(text); });
      attempt([&] { parse_volumes(text); });
      attempt([&] { parse_runtime(text, text); });
      attempt([&] { parse_image_digest(text, "busybox:1.36"); });
      attempt([&] { parse_distribution(text); });
      attempt([&] { parse_pull_line(text); });
      attempt([&] { parse_event(text); });
      attempt([&] { parse_time(text); });
      attempt([&] { parse_time_ns(text); });
      attempt([&] { parse_logs(text, 400); });
      parse_error_message(text);
    },
    [] {
      return std::vector<dannan::net::Bytes>{
          bytes_of(R"({"Id": "abc123", "Warnings": []})"),
          bytes_of(R"([{"Id": "abc123"}, {"Id": "def456"}])"),
          bytes_of(R"({"Id": "abc", "Name": "/louhi-x", "Config": {"Image": "busybox", "Labels": {"louhi.task": "t"}},
                       "State": {"Status": "exited", "ExitCode": 3, "OOMKilled": false, "StartedAt": "2026-11-15T17:39:37.123456789Z",
                                 "FinishedAt": "0001-01-01T00:00:00Z", "Health": {"Status": "healthy"}}})"),
          bytes_of(R"({"Volumes": [{"Name": "louhi-a", "Labels": {"louhi.task": "t"}}]})"),
          bytes_of(R"({"Version": "26.1.5", "ApiVersion": "1.45", "NCPU": 4, "MemTotal": 8443518976, "CgroupVersion": "2",
                       "SecurityOptions": ["name=userns"], "DockerRootDir": "/var/lib/docker/100000.100000", "Driver": "overlay2",
                       "DriverStatus": [["Backing Filesystem", "xfs"]]})"),
          bytes_of(R"({"Descriptor": {"digest": "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}})"),
          bytes_of(R"({"status": "Pulling", "error": "denied"})"),
          bytes_of(R"({"Type": "container", "Action": "die", "Actor": {"ID": "abc", "Attributes": {"louhi.task": "t", "exitCode": "1"}}})"),
          bytes_of(R"({"message": "no such container"})"),
          bytes_of(std::string_view("\x01\x00\x00\x00\x00\x00\x00\x1d"
                                    "2026-11-15T17:39:37Z a line\xff\n",
                                    37))};
    });

}  // namespace
