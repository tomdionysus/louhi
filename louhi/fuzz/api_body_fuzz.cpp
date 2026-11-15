//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "louhi/api/http.h"
#include "louhi/api/json.h"

// Any bytes a caller sends as a request body or target: refused as Invalid,
// or read into a request that is valid, and a definition read from a body
// reads back the same from what the API shows of it.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace louhi;
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  try {
    api::parse_target(text);
  } catch (const api::Invalid&) {
  }
  boost::json::object body;
  try {
    body = api::parse_body(text);
  } catch (const api::Invalid&) {
    return 0;
  }
  try {
    auto definition = api::task_definition_body(body, "fuzz");
    if (api::task_definition_body(api::to_json(definition), "fuzz") != definition) std::abort();
  } catch (const api::Invalid&) {
  }
  try {
    auto quota = api::quota_body(body, "fuzz");
    if (api::quota_body(api::to_json(quota), "fuzz") != quota) std::abort();
  } catch (const api::Invalid&) {
  }
  for (const auto& parse : {+[](const boost::json::object& b) { api::run_task_body(b); }, +[](const boost::json::object& b) { api::stop_task_body(b); },
                            +[](const boost::json::object& b) { api::project_body(b, "fuzz"); }}) {
    try {
      parse(body);
    } catch (const api::Invalid&) {
    }
  }
  return 0;
}
