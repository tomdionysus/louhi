//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/model/names.h"

#include <stdexcept>
#include <string>

namespace louhi::model {

namespace {

bool lower(char c) noexcept { return c >= 'a' && c <= 'z'; }
bool digit(char c) noexcept { return c >= '0' && c <= '9'; }
bool upper(char c) noexcept { return c >= 'A' && c <= 'Z'; }

}  // namespace

bool is_name(std::string_view text) noexcept {
  if (text.empty() || text.size() > max_name) return false;
  if (!lower(text.front()) || text.back() == '-') return false;
  for (const char c : text) {
    if (!lower(c) && !digit(c) && c != '-') return false;
  }
  return true;
}

bool is_env_name(std::string_view text) noexcept {
  if (text.empty() || text.size() > max_env_name) return false;
  if (digit(text.front())) return false;
  for (const char c : text) {
    if (!lower(c) && !upper(c) && !digit(c) && c != '_') return false;
  }
  return true;
}

bool is_container_path(std::string_view text) noexcept {
  if (text.empty() || text.size() > max_path || text.front() != '/') return false;
  if (text.find('\0') != std::string_view::npos) return false;
  if (text == "/") return true;
  std::string_view rest = text.substr(1);
  if (rest.back() == '/') rest.remove_suffix(1);
  if (rest.empty()) return false;
  for (;;) {
    const size_t slash = rest.find('/');
    const std::string_view segment = rest.substr(0, slash);
    if (segment.empty() || segment == "." || segment == "..") return false;
    if (slash == std::string_view::npos) return true;
    rest.remove_prefix(slash + 1);
  }
}

void require_name(std::string_view what, std::string_view text) {
  if (!is_name(text)) {
    throw std::invalid_argument(std::string(what) + " is not a name (1 to 63 of a-z, 0-9 and '-', starting with a letter, not ending in '-')");
  }
}

}  // namespace louhi::model
