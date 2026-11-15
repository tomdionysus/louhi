//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/api/auth.h"

#include <algorithm>
#include <cctype>
#include <optional>

#include "louhi/model/names.h"

namespace louhi::api {

namespace {

constexpr std::string_view prefix = "louhi:";
constexpr size_t max_token = 16 * 1024;

std::optional<Action> action_of(std::string_view text) {
  if (text == "read") return Action::read;
  if (text == "register") return Action::register_definitions;
  if (text == "run") return Action::run;
  return std::nullopt;
}

bool same_ignoring_case(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
         });
}

}  // namespace

const char* action_name(Action action) noexcept {
  switch (action) {
    case Action::read:
      return "read";
    case Action::register_definitions:
      return "register";
    case Action::run:
      return "run";
  }
  return "unknown";
}

Grant::Grant(std::string subject, std::span<const std::string> scopes, uint64_t expires_unix) : _subject(std::move(subject)), _expires_unix(expires_unix) {
  for (const auto& scope : scopes) {
    const std::string_view text(scope);
    if (text.substr(0, prefix.size()) != prefix) continue;
    const std::string_view rest = text.substr(prefix.size());
    if (rest == "admin") {
      _admin = true;
      continue;
    }
    const auto colon = rest.find(':');
    if (colon == std::string_view::npos) continue;
    const std::string_view project = rest.substr(0, colon);
    const std::string_view action = rest.substr(colon + 1);
    if (!model::is_name(project)) continue;
    if (action == "*") {
      _projects[std::string(project)].insert({Action::read, Action::register_definitions, Action::run});
    } else if (const auto a = action_of(action)) {
      _projects[std::string(project)].insert(*a);
    }
  }
}

bool Grant::may(std::string_view project, Action action) const {
  if (_admin) return true;
  const auto it = _projects.find(project);
  return it != _projects.end() && it->second.count(action) == 1;
}

bool Grant::holds(std::string_view project) const { return _admin || _projects.find(project) != _projects.end(); }

std::vector<std::string> Grant::projects() const {
  std::vector<std::string> out;
  for (const auto& [project, actions] : _projects) out.push_back(project);
  return out;
}

Authenticator::Authenticator(const TokenVerifier& verifier, const dannan::contract::WallClock& wall, std::string audience)
    : _verifier(verifier), _wall(wall), _audience(std::move(audience)) {}

std::variant<Grant, std::string> Authenticator::authenticate(std::string_view authorization) const {
  if (authorization.empty()) return std::string("no Authorization header");
  const auto space = authorization.find(' ');
  if (space == std::string_view::npos || !same_ignoring_case(authorization.substr(0, space), "bearer"))
    return std::string("the Authorization header is not a Bearer token");
  std::string_view token = authorization.substr(space + 1);
  while (!token.empty() && token.front() == ' ') token.remove_prefix(1);
  while (!token.empty() && token.back() == ' ') token.remove_suffix(1);
  if (token.empty()) return std::string("the Bearer token is empty");
  if (token.size() > max_token) return std::string("the Bearer token is too long");

  auto verified = _verifier.verify(token);
  if (auto* why = std::get_if<std::string>(&verified)) return std::move(*why);
  auto& claims = std::get<Claims>(verified);
  if (std::find(claims.audiences.begin(), claims.audiences.end(), _audience) == claims.audiences.end()) return "the token is not for " + _audience;
  if (claims.bound) return std::string("the token is bound to a key or certificate, which Louhi cannot check");
  if (claims.expires_unix * 1000 <= _wall.unix_ms()) return std::string("the token has expired");
  return Grant(std::move(claims.subject), claims.scopes, claims.expires_unix);
}

}  // namespace louhi::api
