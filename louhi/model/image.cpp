//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/model/image.h"

#include <stdexcept>

namespace louhi::model {

namespace {

bool lower(char c) noexcept { return c >= 'a' && c <= 'z'; }
bool upper(char c) noexcept { return c >= 'A' && c <= 'Z'; }
bool digit(char c) noexcept { return c >= '0' && c <= '9'; }
bool lower_hex(char c) noexcept { return digit(c) || (c >= 'a' && c <= 'f'); }
bool word(char c) noexcept { return lower(c) || upper(c) || digit(c) || c == '_'; }

[[noreturn]] void refuse(const char* why) { throw std::invalid_argument(std::string("image reference: ") + why); }

// A path component: runs of [a-z0-9] joined by one separator, which is '.',
// '_', "__" or one or more '-'.
bool is_path_component(std::string_view text) noexcept {
  if (text.empty()) return false;
  size_t i = 0;
  for (;;) {
    const size_t start = i;
    while (i < text.size() && (lower(text[i]) || digit(text[i]))) ++i;
    if (i == start) return false;
    if (i == text.size()) return true;
    if (text[i] == '.') {
      ++i;
    } else if (text[i] == '_') {
      ++i;
      if (i < text.size() && text[i] == '_') ++i;
    } else if (text[i] == '-') {
      while (i < text.size() && text[i] == '-') ++i;
    } else {
      return false;
    }
  }
}

// A domain component: alphanumerics, hyphens inside but not at either end.
bool is_domain_component(std::string_view text) noexcept {
  if (text.empty() || text.front() == '-' || text.back() == '-') return false;
  for (const char c : text) {
    if (!lower(c) && !upper(c) && !digit(c) && c != '-') return false;
  }
  return true;
}

bool is_domain(std::string_view text) noexcept {
  std::string_view host = text;
  const size_t colon = text.rfind(':');
  if (colon != std::string_view::npos) {
    const std::string_view port = text.substr(colon + 1);
    if (port.empty() || port.size() > 5) return false;
    unsigned value = 0;
    for (const char c : port) {
      if (!digit(c)) return false;
      value = value * 10 + static_cast<unsigned>(c - '0');
    }
    if (value == 0 || value > 65535) return false;
    host = text.substr(0, colon);
  }
  if (host.empty()) return false;
  for (;;) {
    const size_t dot = host.find('.');
    if (!is_domain_component(host.substr(0, dot))) return false;
    if (dot == std::string_view::npos) return true;
    host.remove_prefix(dot + 1);
  }
}

// What the distribution library takes as a domain rather than the first path
// component: one with a '.' or a ':', "localhost", or any uppercase letter.
bool looks_like_domain(std::string_view first) noexcept {
  if (first == "localhost") return true;
  for (const char c : first) {
    if (c == '.' || c == ':' || upper(c)) return true;
  }
  return false;
}

bool is_tag(std::string_view text) noexcept {
  if (text.empty() || text.size() > max_image_tag || !word(text.front())) return false;
  for (const char c : text) {
    if (!word(c) && c != '.' && c != '-') return false;
  }
  return true;
}

}  // namespace

bool is_digest(std::string_view text) noexcept {
  constexpr std::string_view prefix = "sha256:";
  if (text.size() != prefix.size() + 64 || text.substr(0, prefix.size()) != prefix) return false;
  for (const char c : text.substr(prefix.size())) {
    if (!lower_hex(c)) return false;
  }
  return true;
}

ImageReference parse_image_reference(std::string_view text) {
  if (text.empty()) refuse("empty");
  if (text.size() > max_image_reference) refuse("too long");
  ImageReference out;

  std::string_view rest = text;
  const size_t at = rest.find('@');
  if (at != std::string_view::npos) {
    const std::string_view digest = rest.substr(at + 1);
    if (!is_digest(digest)) refuse("digest is not sha256 and 64 lowercase hex digits");
    out.digest = std::string(digest);
    rest = rest.substr(0, at);
  }

  // A tag is after the last ':' that follows the last '/'; a ':' before it is
  // a domain's port.
  const size_t slash = rest.rfind('/');
  const size_t colon = rest.rfind(':');
  if (colon != std::string_view::npos && (slash == std::string_view::npos || colon > slash)) {
    const std::string_view tag = rest.substr(colon + 1);
    if (!is_tag(tag)) refuse("tag is not 1 to 128 of letters, digits, '_', '.' and '-', starting with a letter, digit or '_'");
    out.tag = std::string(tag);
    rest = rest.substr(0, colon);
  }

  if (rest.empty()) refuse("no name");
  if (rest.size() > max_image_name) refuse("name too long");

  std::string_view path = rest;
  const size_t first_slash = rest.find('/');
  if (first_slash != std::string_view::npos && looks_like_domain(rest.substr(0, first_slash))) {
    const std::string_view domain = rest.substr(0, first_slash);
    if (!is_domain(domain)) refuse("domain is not a host name with an optional port");
    out.domain = std::string(domain);
    path = rest.substr(first_slash + 1);
  }

  if (path.empty()) refuse("no path");
  std::string_view components = path;
  for (;;) {
    const size_t next = components.find('/');
    if (!is_path_component(components.substr(0, next))) refuse("path component is not lowercase letters and digits joined by '.', '_', '__' or '-'");
    if (next == std::string_view::npos) break;
    components.remove_prefix(next + 1);
  }
  out.path = std::string(path);
  return out;
}

std::string to_string(const ImageReference& reference) {
  std::string out;
  if (!reference.domain.empty()) out += reference.domain + "/";
  out += reference.path;
  if (!reference.tag.empty()) out += ":" + reference.tag;
  if (!reference.digest.empty()) out += "@" + reference.digest;
  return out;
}

}  // namespace louhi::model
