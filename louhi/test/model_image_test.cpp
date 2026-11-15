//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Image references (model/image.h), by the Docker distribution grammar:
// [domain[:port]/]path[:tag][@sha256:hex]. A domain is told from a path
// component by a '.', a ':', "localhost" or an uppercase letter. Nothing is
// added that the caller did not write, and anything outside the grammar is
// refused.

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "louhi/model/image.h"

using namespace louhi::model;

namespace {

const std::string digest = "sha256:" + std::string(64, 'b');

}  // namespace

TEST(ImageReference, BareName) {
  const auto r = parse_image_reference("nginx");
  EXPECT_EQ(r, (ImageReference{"", "nginx", "", ""}));
  EXPECT_EQ(to_string(r), "nginx");
}

TEST(ImageReference, PathTagAndDigest) {
  const auto r = parse_image_reference("library/nginx:1.27-alpine@" + digest);
  EXPECT_EQ(r, (ImageReference{"", "library/nginx", "1.27-alpine", digest}));
  EXPECT_EQ(to_string(r), "library/nginx:1.27-alpine@" + digest);
}

TEST(ImageReference, DomainWithPortIsNotATag) {
  const auto r = parse_image_reference("localhost:5000/team/app");
  EXPECT_EQ(r, (ImageReference{"localhost:5000", "team/app", "", ""}));
  const auto tagged = parse_image_reference("registry.example.net:5000/team/app:v2");
  EXPECT_EQ(tagged, (ImageReference{"registry.example.net:5000", "team/app", "v2", ""}));
}

TEST(ImageReference, DomainIsToldFromAPathComponent) {
  EXPECT_EQ(parse_image_reference("localhost/app").domain, "localhost");
  EXPECT_EQ(parse_image_reference("ghcr.io/owner/app").domain, "ghcr.io");
  EXPECT_EQ(parse_image_reference("Registry/app").domain, "Registry");
  // A first component with none of those is part of the path.
  const auto r = parse_image_reference("team/app");
  EXPECT_EQ(r.domain, "");
  EXPECT_EQ(r.path, "team/app");
}

TEST(ImageReference, DigestOnly) {
  const auto r = parse_image_reference("app@" + digest);
  EXPECT_EQ(r, (ImageReference{"", "app", "", digest}));
}

TEST(ImageReference, PathSeparators) {
  EXPECT_NO_THROW(parse_image_reference("team/a.b_c__d---e"));
  // A first component with a '.' is a domain, and '_' is not in a domain.
  EXPECT_THROW(parse_image_reference("a.b_c/f"), std::invalid_argument);
  EXPECT_THROW(parse_image_reference("a___b"), std::invalid_argument);
  EXPECT_THROW(parse_image_reference("a..b"), std::invalid_argument);
  EXPECT_THROW(parse_image_reference("a-"), std::invalid_argument);
  EXPECT_THROW(parse_image_reference("-a"), std::invalid_argument);
  EXPECT_THROW(parse_image_reference("a//b"), std::invalid_argument);
}

TEST(ImageReference, RefusesWhatIsNotAReference) {
  const std::vector<std::string> refused = {"",
                                            ":tag",
                                            "@" + digest,
                                            "app:",
                                            "app@",
                                            "app@sha256:abc",
                                            "app@sha512:" + std::string(128, 'a'),
                                            "app@sha256:" + std::string(64, 'B'),
                                            "App",
                                            "app:-tag",
                                            "app:" + std::string(129, 'a'),
                                            "host:0/app",
                                            "host:65536/app",
                                            "host:x/app",
                                            "-host.io/app",
                                            "host.io/",
                                            "app tag",
                                            "app\n",
                                            std::string("a\0b", 3)};
  for (const auto& bad : refused) {
    EXPECT_THROW(parse_image_reference(bad), std::invalid_argument) << bad;
  }
}

TEST(ImageReference, Bounds) {
  EXPECT_NO_THROW(parse_image_reference(std::string(255, 'a')));
  EXPECT_THROW(parse_image_reference(std::string(256, 'a')), std::invalid_argument);
  EXPECT_NO_THROW(parse_image_reference("a:" + std::string(128, 't')));
  EXPECT_THROW(parse_image_reference(std::string(513, 'a')), std::invalid_argument);
}

TEST(Digest, IsExactlySha256AndLowercaseHex) {
  EXPECT_TRUE(is_digest(digest));
  EXPECT_FALSE(is_digest("sha256:" + std::string(63, 'b')));
  EXPECT_FALSE(is_digest("sha256:" + std::string(65, 'b')));
  EXPECT_FALSE(is_digest("sha256:" + std::string(64, 'g')));
  EXPECT_FALSE(is_digest("SHA256:" + std::string(64, 'b')));
}
