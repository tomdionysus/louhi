//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>

#include "louhi/runtime/docker/api.h"

// Any bytes the Docker daemon could send: every reply parser either reads
// them or refuses them with MalformedReply, and none crashes or reads out of
// bounds.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace louhi::runtime::docker;
  const std::string_view text(reinterpret_cast<const char*>(data), size);
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
  parse_error_message(text);
  return 0;
}
