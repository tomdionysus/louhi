//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/runtime/registries.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace louhi::runtime {

namespace {

constexpr const char* docker_hub = "docker.io";

// A domain as the reference grammar writes one, with its port if any.
bool is_domain(const std::string& text) {
  if (text.empty() || text.size() > model::max_image_name) return false;
  try {
    return model::parse_image_reference(text + "/x").domain == text;
  } catch (const std::invalid_argument&) {
    return false;
  }
}

}  // namespace

Registries::Registries(std::vector<std::string> allowed, std::map<std::string, RegistryCredential> credentials)
    : _allowed(std::move(allowed)), _credentials(std::move(credentials)) {
  for (const auto& domain : _allowed) {
    if (!is_domain(domain)) throw std::invalid_argument("an allowed registry is not a registry's domain");
  }
  for (const auto& [domain, credential] : _credentials) {
    if (std::find(_allowed.begin(), _allowed.end(), domain) == _allowed.end())
      throw std::invalid_argument("there are credentials for " + (is_domain(domain) ? domain : std::string("a registry")) + ", which is not allowed");
    const bool password = !credential.username.empty() && !credential.password.empty() && credential.identity_token.empty();
    const bool token = credential.username.empty() && credential.password.empty() && !credential.identity_token.empty();
    if (!password && !token)
      throw std::invalid_argument("the credentials for " + domain + " are neither a user name with a password nor an identity token alone");
  }
}

std::string Registries::domain_of(const model::ImageReference& reference) { return reference.domain.empty() ? docker_hub : reference.domain; }

bool Registries::allows(const model::ImageReference& reference) const {
  return std::find(_allowed.begin(), _allowed.end(), domain_of(reference)) != _allowed.end();
}

const RegistryCredential* Registries::credential_for(const std::string& domain) const {
  const auto found = _credentials.find(domain);
  return found == _credentials.end() ? nullptr : &found->second;
}

}  // namespace louhi::runtime
