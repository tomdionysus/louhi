//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The shared grammars (model/names.h): a name is a DNS label that starts with
// a letter, an environment name is a C identifier, a container path is
// absolute with no "." or ".." segment.

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "louhi/model/names.h"

using namespace louhi::model;

TEST(Names, AcceptsLabels) {
  EXPECT_TRUE(is_name("a"));
  EXPECT_TRUE(is_name("web"));
  EXPECT_TRUE(is_name("log-shipper2"));
  EXPECT_TRUE(is_name("a--b"));
  EXPECT_TRUE(is_name(std::string(63, 'a')));
}

TEST(Names, RefusesWhatIsNotALabel) {
  EXPECT_FALSE(is_name(""));
  EXPECT_FALSE(is_name(std::string(64, 'a')));
  EXPECT_FALSE(is_name("2web"));
  EXPECT_FALSE(is_name("-web"));
  EXPECT_FALSE(is_name("web-"));
  EXPECT_FALSE(is_name("Web"));
  EXPECT_FALSE(is_name("web_app"));
  EXPECT_FALSE(is_name("web.app"));
  EXPECT_FALSE(is_name("web/app"));
  EXPECT_FALSE(is_name(std::string("we\0b", 4)));
}

TEST(Names, RequireNameNamesTheFieldNotTheValue) {
  try {
    require_name("service name", "Secret-Value");
    FAIL() << "accepted an invalid name";
  } catch (const std::invalid_argument& refused) {
    const std::string message = refused.what();
    EXPECT_NE(message.find("service name"), std::string::npos);
    EXPECT_EQ(message.find("Secret-Value"), std::string::npos);
  }
  EXPECT_NO_THROW(require_name("service name", "frontend"));
}

TEST(EnvNames, AcceptsIdentifiers) {
  EXPECT_TRUE(is_env_name("PATH"));
  EXPECT_TRUE(is_env_name("_x1"));
  EXPECT_TRUE(is_env_name("lower_Case"));
  EXPECT_TRUE(is_env_name(std::string(255, 'A')));
}

TEST(EnvNames, RefusesOthers) {
  EXPECT_FALSE(is_env_name(""));
  EXPECT_FALSE(is_env_name("1PATH"));
  EXPECT_FALSE(is_env_name("A=B"));
  EXPECT_FALSE(is_env_name("A-B"));
  EXPECT_FALSE(is_env_name(std::string(256, 'A')));
}

TEST(ContainerPaths, AcceptsAbsolutePaths) {
  EXPECT_TRUE(is_container_path("/"));
  EXPECT_TRUE(is_container_path("/srv"));
  EXPECT_TRUE(is_container_path("/srv/app/"));
  EXPECT_TRUE(is_container_path("/a.b/..c/.d"));
}

TEST(ContainerPaths, RefusesRelativeOrClimbingPaths) {
  EXPECT_FALSE(is_container_path(""));
  EXPECT_FALSE(is_container_path("srv"));
  EXPECT_FALSE(is_container_path("//"));
  EXPECT_FALSE(is_container_path("/srv//app"));
  EXPECT_FALSE(is_container_path("/srv/./app"));
  EXPECT_FALSE(is_container_path("/srv/../etc"));
  EXPECT_FALSE(is_container_path("/.."));
  EXPECT_FALSE(is_container_path(std::string("/sr\0v", 5)));
  EXPECT_FALSE(is_container_path("/" + std::string(1024, 'a')));
}
