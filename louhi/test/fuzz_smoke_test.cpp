//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Every fuzz target (louhi/fuzz), run through Dannan's harness on every gate:
// its seeds, mutations of them and random bytes, each target reading or
// refusing them with only the error its parser documents (dannan/fuzz/fuzz.h,
// smoke). The libFuzzer runs are the fuzz step's; this is what every build
// runs, with no clang.

#include <gtest/gtest.h>

#include <set>
#include <string>

#include "dannan/fuzz/fuzz.h"

TEST(FuzzSmoke, EveryTargetIsRegistered) {
  std::set<std::string> names;
  for (const auto& target : dannan::fuzz::targets()) names.insert(target.name);
  EXPECT_EQ(names, (std::set<std::string>{"api_body", "docker_reply", "image_reference", "record"}));
}

TEST(FuzzSmoke, EveryTargetTakesItsSeedsMutationsAndNoise) {
  for (const auto& target : dannan::fuzz::targets()) {
    SCOPED_TRACE(target.name);
    EXPECT_GT(dannan::fuzz::smoke(target), 0u);
  }
}
