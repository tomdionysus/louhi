//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <map>
#include <string>
#include <vector>

#include "louhi/model/image.h"

// The registries a node may pull from, and how it signs in to each (design,
// "Images in detail"; the maintainer, 2026-11-16). The operator lists them by domain;
// a reference that names no domain is Docker Hub's, "docker.io", which must be
// listed like any other. No list allows none. Credentials are the operator's,
// one per registry; a project's own wait for a secret store.
//
// Immutable once made, so it is shared between threads without a lock.
namespace louhi::runtime {

struct RegistryCredential {
  // A user name and password, or an identity token in their place.
  std::string username;
  std::string password;
  std::string identity_token;
  bool operator==(const RegistryCredential&) const = default;
};

class Registries {
 public:
  // Allows none.
  Registries() = default;
  // Throws std::invalid_argument for a domain that is not one, a credential
  // for a registry not allowed, or a credential that is neither a user name
  // with a password nor an identity token alone.
  Registries(std::vector<std::string> allowed, std::map<std::string, RegistryCredential> credentials);

  // The registry a reference is pulled from.
  static std::string domain_of(const model::ImageReference& reference);

  bool allows(const model::ImageReference& reference) const;
  // Null when the operator gave none for that registry.
  const RegistryCredential* credential_for(const std::string& domain) const;
  const std::vector<std::string>& allowed() const { return _allowed; }

 private:
  std::vector<std::string> _allowed;
  std::map<std::string, RegistryCredential> _credentials;
};

}  // namespace louhi::runtime
