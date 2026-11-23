//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Any bytes a caller sends as a request body or target: refused as Invalid,
// or read into a request that is valid, and a definition or a quota read from
// a body reads back the same from what the API shows of it.

#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "dannan/fuzz/fuzz.h"
#include "louhi/api/http.h"
#include "louhi/api/json.h"

namespace {

dannan::net::Bytes bytes_of(std::string_view text) { return dannan::net::Bytes(text.begin(), text.end()); }

const dannan::fuzz::Registration api_bodies(
    "api_body",
    [](std::span<const uint8_t> data) {
      using namespace louhi;
      const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
      try {
        api::parse_target(text);
      } catch (const api::Invalid&) {
      }
      boost::json::object body;
      try {
        body = api::parse_body(text);
      } catch (const api::Invalid&) {
        return;
      }
      try {
        auto definition = api::task_definition_body(body, "fuzz");
        if (api::task_definition_body(api::to_json(definition), "fuzz") != definition) throw std::logic_error("a definition shown does not read back the same");
      } catch (const api::Invalid&) {
      }
      try {
        auto quota = api::quota_body(body, "fuzz");
        if (api::quota_body(api::to_json(quota), "fuzz") != quota) throw std::logic_error("a quota shown does not read back the same");
      } catch (const api::Invalid&) {
      }
      for (const auto& parse : {+[](const boost::json::object& b) { api::run_task_body(b); }, +[](const boost::json::object& b) { api::stop_task_body(b); },
                                +[](const boost::json::object& b) { api::project_body(b, "fuzz"); }}) {
        try {
          parse(body);
        } catch (const api::Invalid&) {
        }
      }
    },
    [] {
      return std::vector<dannan::net::Bytes>{
          bytes_of(
              R"({"family":"f","cpu_millis":250,"memory_mib":128,"containers":[{"name":"app","image":"alpine:3.20","mounts":[{"volume":"src","container_path":"/src","read_only":true}]}],"volumes":[{"name":"src","kind":"host","host_directory":"work","host_path":"web"},{"name":"out","kind":"output","size_mib":64}]})"),
          bytes_of(R"({"max_tasks":4,"max_cpu_millis":1000,"max_memory_mib":1024})"),
          bytes_of(R"({"family":"f","count":2,"client_token":"t"})"),
          bytes_of(R"({"reason":"done"})"),
          bytes_of(R"({"description":"a project"})"),
          bytes_of("/v1/projects/web/tasks?limit=3&next=a%2Bb")};
    });

}  // namespace
