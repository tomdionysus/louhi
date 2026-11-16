//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The registries a node may use (runtime/registries.h; design, "Images in
// detail"; the maintainer, 2026-11-16): the operator lists them by domain, a reference
// with no domain is Docker Hub's and allowed only when docker.io is listed, no
// list allows none, and credentials are one per allowed registry, a user name
// with a password or an identity token alone.

#include <gtest/gtest.h>

#include <stdexcept>

#include "louhi/runtime/registries.h"

using namespace louhi;
using runtime::Registries;
using runtime::RegistryCredential;

namespace {

model::ImageReference image_ref(const std::string& text) { return model::parse_image_reference(text); }

}  // namespace

TEST(Registries, NoListAllowsNone) {
  const Registries none;
  EXPECT_FALSE(none.allows(image_ref("alpine")));
  EXPECT_FALSE(none.allows(image_ref("registry.example.net/team/app:1")));
}

TEST(Registries, AReferenceWithNoDomainIsDockerHubs) {
  EXPECT_EQ(Registries::domain_of(image_ref("alpine:3.20")), "docker.io");
  EXPECT_EQ(Registries::domain_of(image_ref("library/alpine")), "docker.io");
  EXPECT_EQ(Registries::domain_of(image_ref("localhost:5000/app")), "localhost:5000");

  const Registries hub({"docker.io"}, {});
  EXPECT_TRUE(hub.allows(image_ref("alpine")));
  EXPECT_TRUE(hub.allows(image_ref("docker.io/library/alpine")));
  EXPECT_FALSE(hub.allows(image_ref("ghcr.io/team/app")));
}

TEST(Registries, OnlyTheListedAreAllowedAndThePortCounts) {
  const Registries some({"registry.example.net:5000", "ghcr.io"}, {});
  EXPECT_TRUE(some.allows(image_ref("registry.example.net:5000/team/app:1")));
  EXPECT_TRUE(some.allows(image_ref("ghcr.io/team/app@sha256:" + std::string(64, 'a'))));
  EXPECT_FALSE(some.allows(image_ref("registry.example.net/team/app:1"))) << "another port is another registry";
  EXPECT_FALSE(some.allows(image_ref("alpine"))) << "Docker Hub is not listed";
}

TEST(Registries, AnAllowedEntryMustBeADomain) {
  EXPECT_THROW(Registries({""}, {}), std::invalid_argument);
  EXPECT_THROW(Registries({"Not A Domain"}, {}), std::invalid_argument);
  EXPECT_THROW(Registries({"ghcr.io/team"}, {}), std::invalid_argument);
  EXPECT_THROW(Registries({"alpine"}, {}), std::invalid_argument) << "a bare name is a path, not a registry";
  EXPECT_NO_THROW(Registries({"localhost"}, {}));
}

TEST(Registries, CredentialsAreForAllowedRegistriesAndOfOneForm) {
  const Registries with({"ghcr.io", "docker.io"}, {{"ghcr.io", {"robot", "secret", ""}}, {"docker.io", {"", "", "token"}}});
  ASSERT_NE(with.credential_for("ghcr.io"), nullptr);
  EXPECT_EQ(with.credential_for("ghcr.io")->username, "robot");
  EXPECT_EQ(with.credential_for("docker.io")->identity_token, "token");
  EXPECT_EQ(with.credential_for("quay.io"), nullptr);

  EXPECT_THROW(Registries({"ghcr.io"}, {{"quay.io", {"robot", "secret", ""}}}), std::invalid_argument) << "not allowed";
  EXPECT_THROW(Registries({"ghcr.io"}, {{"ghcr.io", {"robot", "", ""}}}), std::invalid_argument) << "no password";
  EXPECT_THROW(Registries({"ghcr.io"}, {{"ghcr.io", {"robot", "secret", "token"}}}), std::invalid_argument) << "both forms";
  EXPECT_THROW(Registries({"ghcr.io"}, {{"ghcr.io", {"", "", ""}}}), std::invalid_argument) << "neither";
}

TEST(Registries, ARefusalNeverQuotesASecret) {
  try {
    Registries({"ghcr.io"}, {{"ghcr.io", {"robot", "s3cret", "t0ken"}}});
    FAIL() << "refused";
  } catch (const std::invalid_argument& refused) {
    EXPECT_EQ(std::string(refused.what()).find("s3cret"), std::string::npos);
    EXPECT_EQ(std::string(refused.what()).find("t0ken"), std::string::npos);
  }
}
