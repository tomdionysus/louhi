//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/json/value.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "louhi/model/records.h"

// The API's JSON (design, "API and sign-on"): each record as the API shows
// it, and each request body as a caller writes it. Members are the records'
// own field names.
//
// Every request body is untrusted input: at most max_body bytes and
// max_depth deep, every member one the request defines (an unknown member is
// refused, never ignored), every string, list and number inside its record's
// bounds, and the result validated. Anything else throws Invalid, whose
// message names the member at fault and never its value.
namespace louhi::api {

inline constexpr size_t max_body = 1u << 20;
inline constexpr size_t max_depth = 16;
// RunTask's count, as ECS's.
inline constexpr uint32_t max_run_count = 10;
inline constexpr size_t max_client_token = 64;
// A definition that names no ephemeral storage gets this much.
inline constexpr uint32_t default_ephemeral_storage_mib = 1024;

class Invalid : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

// The body of a request as JSON: an object, bounded as above.
boost::json::object parse_body(std::string_view text);

// Request bodies. The project, and where there is one the family, come from
// the path; a body naming them differently is refused.
struct ProjectBody {
  std::string description;
};
ProjectBody project_body(const boost::json::object& body, std::string_view project);

// The limits; the project is the path's.
model::Quota quota_body(const boost::json::object& body, std::string_view project);

// A definition to register; the revision, the time and the image digests are
// the system's, left empty here. A volume's kind is "memory" (the default) or
// "disk".
model::TaskDefinition task_definition_body(const boost::json::object& body, std::string_view project);

struct RunTaskBody {
  std::string family;
  // Absent: the family's latest.
  std::optional<uint32_t> revision;
  uint32_t count = 1;
  // A retry carrying the same token answers what the first made.
  std::string client_token;
};
RunTaskBody run_task_body(const boost::json::object& body);

struct StopTaskBody {
  std::string reason;
};
StopTaskBody stop_task_body(const boost::json::object& body);

// The records as the API shows them.
boost::json::object to_json(const model::Project& record);
boost::json::object to_json(const model::Quota& record);
boost::json::object to_json(const model::TaskDefinition& record);
boost::json::object to_json(const model::Task& record);
boost::json::object to_json(const model::TaskStatus& record);
boost::json::object to_json(const model::NodeStatus& record);

}  // namespace louhi::api
