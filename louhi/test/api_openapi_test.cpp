//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The route table and the OpenAPI document written from it (dashboard
// design, "The API the dashboard reads"): every route is in the document
// with its method, path and scope, and every path in the document is a
// route, so the two cannot drift; the document is OpenAPI 3.1, titled Louhi,
// at this build's version, and every reference in it resolves.

#include <gtest/gtest.h>

#include <boost/json.hpp>
#include <set>
#include <string>
#include <utility>

#include "louhi/api/openapi.h"
#include "louhi/api/routes.h"
#include "louhi/version.h"

using namespace louhi;
namespace json = boost::json;

namespace {

std::string lowercase(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  return out;
}

// Every "$ref" anywhere in `value`.
void references(const json::value& value, std::set<std::string>& out) {
  if (value.is_object()) {
    for (const auto& [key, member] : value.as_object()) {
      if (key == "$ref" && member.is_string()) out.emplace(member.as_string());
      references(member, out);
    }
  } else if (value.is_array()) {
    for (const auto& element : value.as_array()) references(element, out);
  }
}

}  // namespace

TEST(OpenApi, TheDocumentIsOpenApi31ForLouhiAtThisVersion) {
  const json::value document = json::parse(json::serialize(api::openapi_document()));
  EXPECT_EQ(document.at("openapi"), "3.1.0");
  EXPECT_EQ(document.at("info").at("title"), "Louhi");
  EXPECT_EQ(document.at("info").at("version"), louhi::kVersion);
  EXPECT_EQ(document.at("components").at("securitySchemes").at("bearer").at("scheme"), "bearer");
  EXPECT_EQ(document.at("security"), json::parse(R"([{"bearer":[]}])")) << "every route needs a token";
  const auto& error = document.at("components").at("schemas").at("Error");
  EXPECT_EQ(error.at("required"), json::parse(R"(["error","message"])"));
}

TEST(OpenApi, EveryRouteIsInTheDocumentAndEveryPathIsARoute) {
  const json::object document = api::openapi_document();
  const json::object& paths = document.at("paths").as_object();
  std::set<std::pair<std::string, std::string>> routed;
  for (const api::Route& route : api::routes()) {
    routed.emplace(std::string(route.path), lowercase(route.method));
    const json::value* item = paths.if_contains(json::string_view(route.path.data(), route.path.size()));
    ASSERT_NE(item, nullptr) << route.path << " is not in the document";
    const json::value* operation = item->as_object().if_contains(lowercase(route.method));
    ASSERT_NE(operation, nullptr) << route.method << " " << route.path << " is not in the document";
    EXPECT_EQ(std::string(operation->at("operationId").as_string()), std::string(route.operation_id));
    const std::string scope = route.scope == api::Scope::any ? "any signed-in token" : api::scope_text(route.scope, "{project}");
    EXPECT_EQ(std::string(operation->at("x-louhi-scope").as_string()), scope) << route.path;
  }
  size_t operations = 0;
  for (const auto& [path, item] : paths) {
    for (const auto& [method, operation] : item.as_object()) {
      EXPECT_TRUE(operation.is_object());
      ++operations;
      EXPECT_TRUE(routed.count({std::string(path), std::string(method)})) << method << " " << path << " is in the document but is no route";
    }
  }
  EXPECT_EQ(operations, api::routes().size());
}

TEST(OpenApi, EveryReferenceResolves) {
  const json::value document = api::openapi_document();
  std::set<std::string> refs;
  references(document, refs);
  EXPECT_FALSE(refs.empty());
  for (const auto& ref : refs) {
    ASSERT_EQ(ref.rfind("#/components/", 0), 0u) << ref;
    const auto where = ref.substr(13);
    const auto slash = where.find('/');
    ASSERT_NE(slash, std::string::npos) << ref;
    const auto& kind = document.at("components").at(where.substr(0, slash)).as_object();
    EXPECT_TRUE(kind.contains(where.substr(slash + 1))) << ref << " resolves to nothing";
  }
}

TEST(Routes, EachIsUnderV1OnceAndNamesOnlyKnownParameters) {
  const std::set<std::string> known{"{project}", "{family}", "{revision}", "{task}"};
  std::set<std::pair<std::string_view, std::string_view>> seen;
  std::set<std::string_view> ids;
  for (const api::Route& route : api::routes()) {
    EXPECT_EQ(route.path.rfind("/v1/", 0), 0u) << route.path;
    EXPECT_TRUE(seen.emplace(route.method, route.path).second) << route.method << " " << route.path << " twice";
    EXPECT_TRUE(ids.insert(route.operation_id).second) << route.operation_id << " twice";
    EXPECT_FALSE(route.summary.empty());
    std::string_view rest = route.path;
    while (true) {
      const auto open = rest.find('{');
      if (open == std::string_view::npos) break;
      const auto close = rest.find('}', open);
      ASSERT_NE(close, std::string_view::npos) << route.path;
      EXPECT_TRUE(known.count(std::string(rest.substr(open, close - open + 1)))) << route.path;
      rest.remove_prefix(close + 1);
    }
    const bool in_project = route.scope == api::Scope::read || route.scope == api::Scope::register_definitions || route.scope == api::Scope::run;
    if (in_project) {
      EXPECT_NE(route.path.find("{project}"), std::string_view::npos) << "a project's scope needs a project in the path: " << route.path;
    }
  }
}

TEST(Routes, ScopesReadAsATokenCarriesThem) {
  EXPECT_EQ(api::scope_text(api::Scope::any, "web"), "");
  EXPECT_EQ(api::scope_text(api::Scope::admin, "web"), "louhi:admin");
  EXPECT_EQ(api::scope_text(api::Scope::read, "web"), "louhi:web:read");
  EXPECT_EQ(api::scope_text(api::Scope::register_definitions, "web"), "louhi:web:register");
  EXPECT_EQ(api::scope_text(api::Scope::run, "web"), "louhi:web:run");
}
