//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The API's JSON (api/json.h). A request body is untrusted input: bounded in
// size and depth, every member one the request defines, every value inside
// its record's bounds, and the result valid; a refusal names the member,
// never its value. What the API shows of a definition can be registered back
// as it is.

#include <gtest/gtest.h>

#include <boost/json.hpp>
#include <string>

#include "louhi/api/json.h"
#include "louhi/test/fixtures/records.h"

using namespace louhi;
namespace json = boost::json;

namespace {

std::string refusal(const std::function<void()>& fn) {
  try {
    fn();
  } catch (const api::Invalid& error) {
    return error.what();
  }
  return "";
}

json::object minimal_definition() {
  return json::parse(R"({"family":"frontend","cpu_millis":250,"memory_mib":128,
                         "containers":[{"name":"app","image":"alpine:3.20"}]})")
      .as_object();
}

}  // namespace

TEST(ApiJson, ABodyMustBeAnObjectOfBoundedSizeAndDepth) {
  EXPECT_TRUE(api::parse_body("").empty()) << "an empty body is an empty object";
  EXPECT_NE(refusal([] { api::parse_body("[1]"); }), "");
  EXPECT_NE(refusal([] { api::parse_body("{"); }), "");
  EXPECT_NE(refusal([] { api::parse_body(std::string(api::max_body + 1, ' ')); }), "");
  std::string deep;
  for (size_t i = 0; i < api::max_depth + 1; ++i) deep += "{\"a\":";
  deep += "1";
  for (size_t i = 0; i < api::max_depth + 1; ++i) deep += "}";
  EXPECT_NE(refusal([&] { api::parse_body(deep); }), "");
}

TEST(ApiJson, AMinimalDefinitionParsesWithTheDefaults) {
  const auto d = api::task_definition_body(minimal_definition(), "web");
  EXPECT_EQ(d.project, "web");
  EXPECT_EQ(d.family, "frontend");
  EXPECT_EQ(d.revision, 0u) << "the revision is the system's";
  ASSERT_EQ(d.containers.size(), 1u);
  EXPECT_TRUE(d.containers[0].essential);
  EXPECT_FALSE(d.run_once);
  EXPECT_EQ(d.ephemeral_storage_mib, api::default_ephemeral_storage_mib);
}

TEST(ApiJson, VolumesAreMemoryUnlessTheySayDisk) {
  auto body = minimal_definition();
  body["volumes"] = json::array{json::object{{"name", "a"}, {"size_mib", 8}}, json::object{{"name", "b"}, {"kind", "memory"}, {"size_mib", 8}},
                                json::object{{"name", "c"}, {"kind", "disk"}, {"size_mib", 4096}}};
  const auto d = api::task_definition_body(body, "web");
  ASSERT_EQ(d.volumes.size(), 3u);
  EXPECT_EQ(d.volumes[0].kind, model::VolumeKind::memory);
  EXPECT_EQ(d.volumes[1].kind, model::VolumeKind::memory);
  EXPECT_EQ(d.volumes[2].kind, model::VolumeKind::disk);
  EXPECT_EQ(api::to_json(d).at("volumes").at(2).at("kind"), "disk");

  body["volumes"] = json::array{json::object{{"name", "a"}, {"kind", "scratch"}, {"size_mib", 8}}};
  EXPECT_NE(refusal([&] { api::task_definition_body(body, "web"); }), "");
  body["volumes"] = json::array{json::object{{"name", "a"}, {"kind", "memory"}, {"size_mib", 129}}};
  EXPECT_NE(refusal([&] { api::task_definition_body(body, "web"); }), "") << "more memory volume than the task's 128 MiB";
}

TEST(ApiJson, EphemeralStorageIsBounded) {
  auto body = minimal_definition();
  body["ephemeral_storage_mib"] = 20480;
  EXPECT_EQ(api::task_definition_body(body, "web").ephemeral_storage_mib, 20480u);
  body["ephemeral_storage_mib"] = 1;
  EXPECT_NE(refusal([&] { api::task_definition_body(body, "web"); }), "");
}

TEST(ApiJson, WhatTheApiShowsOfADefinitionRegistersBackAsItIs) {
  auto shown = api::to_json(test::task_definition());
  auto back = api::task_definition_body(shown, "web");
  auto expected = test::task_definition();
  expected.revision = 0;
  expected.registered_unix_ms = 0;
  for (auto& c : expected.containers) c.image_digest.clear();
  EXPECT_EQ(back, expected) << "the revision, the time and the digest are the system's";
}

TEST(ApiJson, AnUnknownMemberIsRefusedNeverIgnored) {
  auto body = minimal_definition();
  body["cpu"] = 250;
  EXPECT_NE(refusal([&] { api::task_definition_body(body, "web"); }), "");

  body = minimal_definition();
  body["containers"].as_array()[0].as_object()["privileged"] = true;
  const auto why = refusal([&] { api::task_definition_body(body, "web"); });
  EXPECT_NE(why, "");
  EXPECT_EQ(why.find("privileged"), std::string::npos) << "the caller's text is not repeated";
}

TEST(ApiJson, ADefinitionForAnotherProjectIsRefused) {
  auto body = minimal_definition();
  body["project"] = "batch";
  EXPECT_NE(refusal([&] { api::task_definition_body(body, "web"); }), "");
  body["project"] = "web";
  EXPECT_EQ(refusal([&] { api::task_definition_body(body, "web"); }), "");
}

TEST(ApiJson, AnInvalidDefinitionIsRefusedNamingTheFieldNotTheValue) {
  auto body = minimal_definition();
  body["memory_mib"] = 1;
  EXPECT_NE(refusal([&] { api::task_definition_body(body, "web"); }), "");

  body = minimal_definition();
  body["containers"].as_array()[0].as_object()["environment"] = json::array{json::object{{"name", "1BAD"}, {"value", "s3cret"}}};
  const auto why = refusal([&] { api::task_definition_body(body, "web"); });
  EXPECT_NE(why, "");
  EXPECT_EQ(why.find("s3cret"), std::string::npos);

  body = minimal_definition();
  body["containers"] = json::array{};
  EXPECT_NE(refusal([&] { api::task_definition_body(body, "web"); }), "") << "no containers";

  body = minimal_definition();
  body["containers"].as_array()[0].as_object()["image"] = "Not An Image";
  EXPECT_NE(refusal([&] { api::task_definition_body(body, "web"); }), "");
}

TEST(ApiJson, NumbersMustBeWholeAndInRange) {
  for (const json::value& bad : {json::value(-1), json::value(1.5), json::value("250"), json::value(1ull << 40)}) {
    auto body = minimal_definition();
    body["cpu_millis"] = bad;
    EXPECT_NE(refusal([&] { api::task_definition_body(body, "web"); }), "") << json::serialize(bad);
  }
}

TEST(ApiJson, ListsAreBounded) {
  auto body = minimal_definition();
  json::array containers;
  for (size_t i = 0; i <= model::max_containers; ++i) containers.push_back(json::object{{"name", "c" + std::to_string(i)}, {"image", "alpine"}});
  body["containers"] = containers;
  EXPECT_NE(refusal([&] { api::task_definition_body(body, "web"); }), "");
}

TEST(ApiJson, RunTaskTakesAFamilyAndDefaultsTheRest) {
  const auto r = api::run_task_body(json::parse(R"({"family":"frontend"})").as_object());
  EXPECT_EQ(r.family, "frontend");
  EXPECT_FALSE(r.revision);
  EXPECT_EQ(r.count, 1u);
  EXPECT_EQ(r.client_token, "");
  const auto s = api::run_task_body(json::parse(R"({"family":"frontend","revision":3,"count":10,"client_token":"abc"})").as_object());
  EXPECT_EQ(s.revision, 3u);
  EXPECT_EQ(s.count, 10u);
  EXPECT_EQ(s.client_token, "abc");
}

TEST(ApiJson, RunTaskRefusesWhatItDoesNotDefine) {
  for (const char* bad : {R"({})", R"({"family":"Frontend"})", R"({"family":"f","count":0})", R"({"family":"f","count":11})", R"({"family":"f","revision":0})",
                          R"({"family":"f","node":"x"})"}) {
    EXPECT_NE(refusal([&] { api::run_task_body(api::parse_body(bad)); }), "") << bad << ": a caller never names a node";
  }
  EXPECT_NE(refusal([] { api::run_task_body(api::parse_body(R"({"family":"f","client_token":")" + std::string(65, 'x') + "\"}")); }), "");
}

TEST(ApiJson, AQuotaIsTheOperatorsLimits) {
  const auto q = api::quota_body(json::parse(R"({"max_tasks":5,"max_cpu_millis":2000,"max_memory_mib":1024})").as_object(), "web");
  EXPECT_EQ(q.project, "web");
  EXPECT_EQ(q.max_tasks, 5u);
  EXPECT_EQ(q.max_volumes, 0u) << "a limit left out allows none";
  EXPECT_NE(refusal([] { api::quota_body(json::parse(R"({"project":"batch"})").as_object(), "web"); }), "");
  EXPECT_NE(refusal([] { api::quota_body(json::parse(R"({"max_tasks":-1})").as_object(), "web"); }), "");
  auto shown = api::to_json(test::quota());
  auto back = api::quota_body(shown, "web");
  auto expected = test::quota();
  expected.updated_unix_ms = 0;
  EXPECT_EQ(back, expected);
}

TEST(ApiJson, AProjectBodyHoldsItsDescription) {
  EXPECT_EQ(api::project_body(json::parse(R"({"description":"The public site"})").as_object(), "web").description, "The public site");
  EXPECT_NE(refusal([] { api::project_body(json::parse(R"({"name":"batch"})").as_object(), "web"); }), "");
  EXPECT_NE(refusal([] { api::project_body(json::parse(R"({"owner":"x"})").as_object(), "web"); }), "");
}

TEST(ApiJson, StatusesShowTheirStatesByName) {
  const auto shown = api::to_json(test::task_status());
  EXPECT_EQ(shown.at("state"), "running");
  EXPECT_EQ(shown.at("containers").as_array()[1].as_object().at("exit_code"), -9);
  EXPECT_EQ(api::to_json(test::task()).at("desired"), "running");
  EXPECT_EQ(api::to_json(test::node_status()).at("ready"), true);
}
