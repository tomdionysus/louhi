//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/version.h"

#include <gtest/gtest.h>

#include <string>

TEST(Version, LibraryMatchesHeaders) { EXPECT_EQ(std::string(louhi::library_version()), std::string(louhi::kVersion)); }

TEST(Version, IsSemver) {
  EXPECT_EQ(std::string(louhi::kVersion),
            std::to_string(louhi::kVersionMajor) + "." + std::to_string(louhi::kVersionMinor) + "." + std::to_string(louhi::kVersionPatch));
}
