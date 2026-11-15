//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <string_view>

// The small grammars every record shares. A name is what a caller types for a
// project, a task definition's family, a service, a container or a volume:
// one to 63 of lowercase letters, digits and hyphens, starting with a letter
// and not ending in a hyphen, so it is also a DNS label and a path segment
// with no escaping anywhere it appears.
namespace louhi::model {

inline constexpr size_t max_name = 63;
inline constexpr size_t max_env_name = 255;
inline constexpr size_t max_path = 1024;

bool is_name(std::string_view text) noexcept;

// An environment variable's name: a letter or underscore, then letters,
// digits and underscores, at most max_env_name.
bool is_env_name(std::string_view text) noexcept;

// An absolute path inside a container: starts with '/', at most max_path, no
// NUL, no "." or ".." segment, no empty segment but a trailing one.
bool is_container_path(std::string_view text) noexcept;

// Throws std::invalid_argument naming `what` when `text` is not a name. The
// message carries the field, never the value.
void require_name(std::string_view what, std::string_view text);

}  // namespace louhi::model
