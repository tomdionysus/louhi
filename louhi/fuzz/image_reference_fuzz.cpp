//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Any text a caller gives as an image: refused, or parsed into parts that
// print back as exactly that text and parse again to the same parts.

#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "dannan/fuzz/fuzz.h"
#include "louhi/model/image.h"

namespace {

dannan::net::Bytes bytes_of(std::string_view text) { return dannan::net::Bytes(text.begin(), text.end()); }

const dannan::fuzz::Registration image_references(
    "image_reference",
    [](std::span<const uint8_t> data) {
      using namespace louhi::model;
      const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
      try {
        const ImageReference reference = parse_image_reference(text);
        const std::string printed = to_string(reference);
        if (printed != text) throw std::logic_error("an image reference does not print back as written");
        if (parse_image_reference(printed) != reference) throw std::logic_error("an image reference printed does not parse to the same parts");
      } catch (const std::invalid_argument&) {
      }
    },
    [] {
      return std::vector<dannan::net::Bytes>{bytes_of("alpine"), bytes_of("alpine:3.20"), bytes_of("registry.example.net:5000/team/frontend:1.4.2"),
                                             bytes_of("busybox@sha256:" + std::string(64, 'a'))};
    });

}  // namespace
