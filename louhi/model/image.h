//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// A container image reference, as the Docker distribution grammar writes it:
// [domain[:port]/]path[:tag][@digest]. Parsed strictly, because a reference
// reaches the runtime and a registry from a caller: anything outside the
// grammar is refused, never repaired. Nothing is added that the caller did not
// write (no "docker.io", no "library/", no "latest"), so a reference prints
// back exactly as given.
namespace louhi::model {

// The whole reference, and the name part (domain and path), as the
// distribution grammar bounds them.
inline constexpr size_t max_image_reference = 512;
inline constexpr size_t max_image_name = 255;
inline constexpr size_t max_image_tag = 128;

struct ImageReference {
  // Empty when the reference names none.
  std::string domain;
  // One or more components separated by '/', as written.
  std::string path;
  // Empty when none.
  std::string tag;
  // "sha256:" and 64 lowercase hex digits; empty when none.
  std::string digest;
  bool operator==(const ImageReference&) const = default;
};

// Throws std::invalid_argument for anything that is not a reference within
// the bounds above. Only sha256 digests are accepted.
ImageReference parse_image_reference(std::string_view text);

std::string to_string(const ImageReference& reference);

// "sha256:" and exactly 64 lowercase hex digits.
bool is_digest(std::string_view text) noexcept;

}  // namespace louhi::model
