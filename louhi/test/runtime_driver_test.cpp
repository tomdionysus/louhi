//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The runtime contract's errors (runtime/driver.h): every kind of failure has
// its own name for a person, as the agent's log line gives it.

#include <gtest/gtest.h>

#include <set>
#include <string>

#include "louhi/runtime/driver.h"

using louhi::runtime::Error;
using louhi::runtime::kind_name;

TEST(RuntimeErrors, EachKindHasItsOwnName) {
  EXPECT_STREQ(kind_name(Error::Kind::unavailable), "unavailable");
  EXPECT_STREQ(kind_name(Error::Kind::refused), "refused");
  EXPECT_STREQ(kind_name(Error::Kind::not_found), "not found");
  EXPECT_STREQ(kind_name(Error::Kind::timed_out), "timed out");
  EXPECT_STREQ(kind_name(Error::Kind::malformed), "malformed");
  const std::set<std::string> names{kind_name(Error::Kind::unavailable), kind_name(Error::Kind::refused), kind_name(Error::Kind::not_found),
                                    kind_name(Error::Kind::timed_out), kind_name(Error::Kind::malformed)};
  EXPECT_EQ(names.size(), 5u);
}
