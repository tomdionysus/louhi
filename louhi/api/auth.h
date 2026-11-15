//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "dannan/contract/time_source.h"

// Who a request is from and what it may do (design, "API and sign-on").
// Whether a token is genuine is the verifier's (the Lugh plugin's, through
// lugh_client: verification stays Lugh's); what it grants is Louhi's, read
// from the token's own claims as Lir reads them:
//
//   "louhi" among its audiences;
//   its scopes "louhi:admin" (everything) and "louhi:<project>:<action>",
//   the action read, register, run or * (every action in that project);
//   until its exp;
//   and no binding (cnf) Louhi cannot check: a bound token is refused.
//
// A scope that does not parse, or names another application, is skipped.
namespace louhi::api {

// What a verified access token says, as far as Louhi needs.
struct Claims {
  std::string subject;
  std::vector<std::string> audiences;
  // The scope claim, split on spaces.
  std::vector<std::string> scopes;
  // exp: wall-clock seconds since the Unix epoch.
  uint64_t expires_unix = 0;
  // A cnf claim of either kind: bound to a key or a certificate.
  bool bound = false;
};

// Says whether a token is genuine and current, from what it holds, waiting
// on nothing. Thread-safe.
class TokenVerifier {
 public:
  virtual ~TokenVerifier() = default;
  // The claims, or why the token is not good.
  virtual std::variant<Claims, std::string> verify(std::string_view token) const = 0;
};

enum class Action : uint8_t { read = 1, register_definitions = 2, run = 3 };

const char* action_name(Action action) noexcept;

// What one token grants.
class Grant {
 public:
  Grant() = default;
  Grant(std::string subject, std::span<const std::string> scopes, uint64_t expires_unix);

  const std::string& subject() const noexcept { return _subject; }
  uint64_t expires_unix() const noexcept { return _expires_unix; }
  bool admin() const noexcept { return _admin; }
  bool may(std::string_view project, Action action) const;
  // Whether it holds any scope in `project`.
  bool holds(std::string_view project) const;
  // The projects it holds a scope in, in order; empty for admin, who holds
  // them all.
  std::vector<std::string> projects() const;

 private:
  std::string _subject;
  uint64_t _expires_unix = 0;
  bool _admin = false;
  // Per project, its actions; every action for "*".
  std::map<std::string, std::set<Action>, std::less<>> _projects;
};

// The token in an Authorization header ("Bearer <token>", the scheme in any
// case), checked and turned into a grant, or why not.
class Authenticator {
 public:
  // The verifier and the clock outlive this.
  Authenticator(const TokenVerifier& verifier, const dannan::contract::WallClock& wall, std::string audience = "louhi");
  // Thread-safe; waits on nothing.
  std::variant<Grant, std::string> authenticate(std::string_view authorization) const;

 private:
  const TokenVerifier& _verifier;
  const dannan::contract::WallClock& _wall;
  std::string _audience;
};

}  // namespace louhi::api
