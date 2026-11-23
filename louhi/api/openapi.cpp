//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/api/openapi.h"

#include <boost/json.hpp>
#include <string>
#include <string_view>

#include "louhi/api/routes.h"
#include "louhi/version.h"

namespace louhi::api {

namespace json = boost::json;

namespace {

// The components, by hand: each record as api/json.cpp writes it, each body
// as it reads one. A record's body accepts what the record shows, so one
// described can be put back as it is.
constexpr std::string_view components = R"json({
  "securitySchemes": {
    "bearer": {"type": "http", "scheme": "bearer", "bearerFormat": "JWT",
               "description": "An access token from a trusted Lugh issuer, audience louhi, carrying louhi:admin or louhi:<project>:<read|register|run|*>."}
  },
  "parameters": {
    "project": {"name": "project", "in": "path", "required": true, "description": "A project's name.", "schema": {"type": "string"}},
    "family": {"name": "family", "in": "path", "required": true, "description": "A task definition family's name.", "schema": {"type": "string"}},
    "revision": {"name": "revision", "in": "path", "required": true, "schema": {"type": "integer", "minimum": 1, "maximum": 4294967295}},
    "task": {"name": "task", "in": "path", "required": true, "description": "Thirty-two hex digits.", "schema": {"type": "string", "pattern": "^[0-9A-Fa-f]{32}$"}},
    "limit": {"name": "limit", "in": "query", "description": "The most to answer.", "schema": {"type": "integer", "minimum": 1, "maximum": 100, "default": 100}},
    "next": {"name": "next", "in": "query", "description": "The key to start after, from the last page's next.", "schema": {"type": "string", "maxLength": 64}}
  },
  "responses": {
    "Error": {"description": "Refused: 400 invalid, 401 unauthenticated, 403 forbidden, 404 not_found, 405 method_not_allowed, 409 quota or full, 413 too_large, 503 not_written or not_ready.",
              "content": {"application/json": {"schema": {"$ref": "#/components/schemas/Error"}}}}
  },
  "schemas": {
    "Error": {"type": "object", "required": ["error", "message"],
              "properties": {"error": {"type": "string"}, "message": {"type": "string"}}},
    "Project": {"type": "object", "required": ["name", "description", "created_unix_ms"],
                "properties": {"name": {"type": "string"}, "description": {"type": "string"}, "created_unix_ms": {"type": "integer"}}},
    "ProjectBody": {"type": "object", "additionalProperties": false,
                    "properties": {"name": {"type": "string", "description": "The path's, if given."}, "description": {"type": "string"},
                                   "created_unix_ms": {"description": "Ignored."}}},
    "Quota": {"type": "object", "additionalProperties": false,
              "properties": {"project": {"type": "string"}, "max_tasks": {"type": "integer"}, "max_cpu_millis": {"type": "integer"},
                             "max_memory_mib": {"type": "integer"}, "max_addresses": {"type": "integer"}, "max_volumes": {"type": "integer"},
                             "max_load_balancers": {"type": "integer"}, "updated_unix_ms": {"type": "integer"}}},
    "EnvVar": {"type": "object", "required": ["name"], "additionalProperties": false,
               "properties": {"name": {"type": "string"}, "value": {"type": "string"}}},
    "PortMapping": {"type": "object", "required": ["container_port"], "additionalProperties": false,
                    "properties": {"container_port": {"type": "integer", "minimum": 1, "maximum": 65535}, "protocol": {"enum": ["tcp", "udp"]}}},
    "Mount": {"type": "object", "required": ["volume", "container_path"], "additionalProperties": false,
              "properties": {"volume": {"type": "string"}, "container_path": {"type": "string"}, "read_only": {"type": "boolean"}}},
    "HealthCheck": {"type": "object", "additionalProperties": false,
                    "properties": {"command": {"type": "array", "items": {"type": "string"}}, "interval_s": {"type": "integer"},
                                   "timeout_s": {"type": "integer"}, "retries": {"type": "integer"}, "start_period_s": {"type": "integer"}}},
    "Container": {"type": "object", "required": ["name", "image"], "additionalProperties": false,
                  "properties": {"name": {"type": "string"}, "image": {"type": "string"},
                                 "image_digest": {"type": "string", "description": "The system's, resolved at registration."},
                                 "entry_point": {"type": "array", "items": {"type": "string"}}, "command": {"type": "array", "items": {"type": "string"}},
                                 "environment": {"type": "array", "items": {"$ref": "#/components/schemas/EnvVar"}},
                                 "working_directory": {"type": "string"},
                                 "ports": {"type": "array", "items": {"$ref": "#/components/schemas/PortMapping"}},
                                 "mounts": {"type": "array", "items": {"$ref": "#/components/schemas/Mount"}},
                                 "health_check": {"$ref": "#/components/schemas/HealthCheck"},
                                 "cpu_millis": {"type": "integer"}, "memory_mib": {"type": "integer"},
                                 "essential": {"type": "boolean", "default": true}, "read_only_root": {"type": "boolean", "default": false}}},
    "Volume": {"type": "object", "required": ["name"], "additionalProperties": false,
               "properties": {"name": {"type": "string"}, "kind": {"enum": ["memory", "disk", "host", "output"], "default": "memory"},
                              "size_mib": {"type": "integer", "description": "Every kind but host."},
                              "host_directory": {"type": "string", "description": "host: a directory the node's operator names."},
                              "host_path": {"type": "string", "description": "host: a relative path inside it; mounted read-only."}}},
    "TaskDefinition": {"type": "object", "required": ["family", "containers"], "additionalProperties": false,
                       "properties": {"project": {"type": "string"}, "family": {"type": "string"},
                                      "revision": {"type": "integer", "description": "The system's."},
                                      "cpu_millis": {"type": "integer"}, "memory_mib": {"type": "integer"},
                                      "ephemeral_storage_mib": {"type": "integer", "default": 1024},
                                      "containers": {"type": "array", "minItems": 1, "items": {"$ref": "#/components/schemas/Container"}},
                                      "volumes": {"type": "array", "items": {"$ref": "#/components/schemas/Volume"}},
                                      "run_once": {"type": "boolean", "default": false},
                                      "registered_unix_ms": {"type": "integer", "description": "The system's."}}},
    "RunTaskBody": {"type": "object", "required": ["family"], "additionalProperties": false,
                    "properties": {"family": {"type": "string"},
                                   "revision": {"type": "integer", "minimum": 1, "description": "The family's latest when left out."},
                                   "count": {"type": "integer", "minimum": 1, "maximum": 10, "default": 1},
                                   "client_token": {"type": "string", "maxLength": 64,
                                                    "description": "A retry with the same token within an hour answers the tasks the first made."}}},
    "StopTaskBody": {"type": "object", "additionalProperties": false, "properties": {"reason": {"type": "string"}}},
    "Task": {"type": "object", "required": ["id", "project", "family", "revision", "desired", "placement_epoch", "started_by", "created_unix_ms"],
             "properties": {"id": {"type": "string"}, "project": {"type": "string"}, "family": {"type": "string"}, "revision": {"type": "integer"},
                            "service": {"type": "string"}, "desired": {"enum": ["running", "stopped"]}, "stop_reason": {"type": "string"},
                            "node": {"type": "string"}, "placement_epoch": {"type": "integer"}, "started_by": {"type": "string"},
                            "created_unix_ms": {"type": "integer"}}},
    "ContainerStatus": {"type": "object", "required": ["name", "state", "restarts", "health"],
                        "properties": {"name": {"type": "string"}, "state": {"enum": ["created", "running", "exited"]},
                                       "exit_code": {"type": "integer"}, "restarts": {"type": "integer"},
                                       "health": {"enum": ["unknown", "healthy", "unhealthy"]}}},
    "TaskStatus": {"type": "object", "required": ["task", "node", "sequence", "state", "containers", "started_unix_ms", "stopped_unix_ms", "observed_unix_ms"],
                   "properties": {"task": {"type": "string"}, "node": {"type": "string"}, "sequence": {"type": "integer"},
                                  "state": {"enum": ["pending", "pulling", "starting", "running", "stopping", "stopped", "parked"]},
                                  "reason": {"type": "string"},
                                  "containers": {"type": "array", "items": {"$ref": "#/components/schemas/ContainerStatus"}},
                                  "started_unix_ms": {"type": "integer"}, "stopped_unix_ms": {"type": "integer"}, "observed_unix_ms": {"type": "integer"}}},
    "DescribedTask": {"allOf": [{"$ref": "#/components/schemas/Task"},
                                {"type": "object", "required": ["statuses"],
                                 "properties": {"statuses": {"type": "array", "items": {"$ref": "#/components/schemas/TaskStatus"}}}}]},
    "Capacity": {"type": "object", "required": ["cpu_millis", "memory_mib"],
                 "properties": {"cpu_millis": {"type": "integer"}, "memory_mib": {"type": "integer"}}},
    "NodeStatus": {"type": "object",
                   "required": ["node", "sequence", "louhi_version", "runtime", "runtime_version", "total", "allocated", "tasks", "ready", "conditions", "observed_unix_ms"],
                   "properties": {"node": {"type": "string"}, "sequence": {"type": "integer"}, "louhi_version": {"type": "string"},
                                  "runtime": {"type": "string"}, "runtime_version": {"type": "string"},
                                  "total": {"$ref": "#/components/schemas/Capacity"}, "allocated": {"$ref": "#/components/schemas/Capacity"},
                                  "tasks": {"type": "integer"}, "ready": {"type": "boolean"},
                                  "conditions": {"type": "array", "items": {"type": "string"}}, "observed_unix_ms": {"type": "integer"}}},
    "Status": {"type": "object", "required": ["node", "version", "ready", "store"],
               "properties": {"node": {"type": "string", "description": "This node's id."}, "version": {"type": "string"},
                              "ready": {"type": "boolean", "description": "This node's last report said it was ready; false before its first."},
                              "store": {"type": "object", "required": ["ready"], "properties": {"ready": {"type": "boolean"}}},
                              "tasks": {"type": "integer", "description": "louhi:admin only: the tasks its last report counted."},
                              "report": {"description": "louhi:admin only: its last report, null before its first.",
                                         "oneOf": [{"$ref": "#/components/schemas/NodeStatus"}, {"type": "null"}]}}},
    "ProjectList": {"type": "object", "required": ["projects"],
                    "properties": {"projects": {"type": "array", "items": {"$ref": "#/components/schemas/Project"}}, "next": {"type": "string"}}},
    "FamilyList": {"type": "object", "required": ["families"],
                   "properties": {"families": {"type": "array", "items": {"type": "string"}}, "next": {"type": "string"}}},
    "TaskDefinitionList": {"type": "object", "required": ["task_definitions"],
                           "properties": {"task_definitions": {"type": "array", "items": {"$ref": "#/components/schemas/TaskDefinition"}},
                                          "next": {"type": "string"}}},
    "TaskList": {"type": "object", "required": ["tasks"],
                 "properties": {"tasks": {"type": "array", "items": {"$ref": "#/components/schemas/Task"}}, "next": {"type": "string"}}},
    "RunTaskResult": {"type": "object", "required": ["tasks"],
                      "properties": {"tasks": {"type": "array", "items": {"$ref": "#/components/schemas/Task"}}}},
    "NodeList": {"type": "object", "required": ["nodes"],
                 "properties": {"nodes": {"type": "array", "items": {"$ref": "#/components/schemas/NodeStatus"}}, "next": {"type": "string"}}},
    "LogLine": {"type": "object", "required": ["stream", "time_ns", "text"],
                "properties": {"stream": {"enum": ["stdout", "stderr"]}, "time_ns": {"type": "integer", "description": "Unix nanoseconds."},
                               "text": {"type": "string", "description": "Without its newline; bytes that are not UTF-8 are U+FFFD."},
                               "truncated": {"type": "boolean", "description": "Cut at 16 KiB."}}},
    "TaskLogs": {"type": "object", "required": ["container", "lines"],
                 "description": "Query: container (needed when the task has more than one), tail (1 to 400, 100 by default), after (Unix nanoseconds).",
                 "properties": {"container": {"type": "string"}, "lines": {"type": "array", "items": {"$ref": "#/components/schemas/LogLine"}},
                                "next": {"type": "string", "description": "The last line's time: the next call's after."}}},
    "OpenApi": {"type": "object", "description": "This document."}
  }
})json";

// What each operation takes and answers, by schema name; empty takes no
// body. A route added without one here does not compile (-Wswitch).
struct Shape {
  std::string_view request;
  std::string_view response;
  bool paged = false;
};

Shape shape_of(Operation operation) {
  switch (operation) {
    case Operation::put_project:
      return {"ProjectBody", "Project", false};
    case Operation::list_projects:
      return {"", "ProjectList", true};
    case Operation::get_project:
      return {"", "Project", false};
    case Operation::put_quota:
      return {"Quota", "Quota", false};
    case Operation::get_quota:
      return {"", "Quota", false};
    case Operation::register_task_definition:
      return {"TaskDefinition", "TaskDefinition", false};
    case Operation::list_families:
      return {"", "FamilyList", true};
    case Operation::list_revisions:
      return {"", "TaskDefinitionList", true};
    case Operation::describe_task_definition:
      return {"", "TaskDefinition", false};
    case Operation::run_task:
      return {"RunTaskBody", "RunTaskResult", false};
    case Operation::list_tasks:
      return {"", "TaskList", true};
    case Operation::describe_task:
      return {"", "DescribedTask", false};
    case Operation::stop_task:
      return {"StopTaskBody", "Task", false};
    case Operation::get_task_logs:
      return {"", "TaskLogs", false};
    case Operation::list_nodes:
      return {"", "NodeList", true};
    case Operation::status:
      return {"", "Status", false};
    case Operation::openapi:
      return {"", "OpenApi", false};
  }
  return {"", "OpenApi", false};
}

json::object reference_to(std::string_view where, std::string_view name) { return {{"$ref", "#/components/" + std::string(where) + "/" + std::string(name)}}; }

json::object json_content(std::string_view schema) { return {{"application/json", {{"schema", reference_to("schemas", schema)}}}}; }

std::string lowercase(std::string_view method) {
  std::string out(method);
  for (char& c : out) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  return out;
}

json::object operation_of(const Route& route) {
  const Shape shape = shape_of(route.operation);
  const std::string scope = route.scope == Scope::any ? "any signed-in token" : scope_text(route.scope, "{project}");
  json::array parameters;
  std::string_view rest = route.path;
  while (!rest.empty()) {
    const size_t open = rest.find('{');
    if (open == std::string_view::npos) break;
    const size_t close = rest.find('}', open);
    parameters.push_back(reference_to("parameters", rest.substr(open + 1, close - open - 1)));
    rest.remove_prefix(close + 1);
  }
  if (shape.paged) {
    parameters.push_back(reference_to("parameters", "limit"));
    parameters.push_back(reference_to("parameters", "next"));
  }
  json::object out{
      {"operationId", std::string(route.operation_id)},
      {"summary", std::string(route.summary)},
      {"description", "Scope: " + scope + "."},
      {"x-louhi-scope", scope},
      {"responses",
       {{"200", {{"description", std::string(route.summary)}, {"content", json_content(shape.response)}}}, {"default", reference_to("responses", "Error")}}}};
  if (!parameters.empty()) out["parameters"] = std::move(parameters);
  if (!shape.request.empty()) out["requestBody"] = json::object{{"required", true}, {"content", json_content(shape.request)}};
  return out;
}

}  // namespace

json::object openapi_document() {
  json::object paths;
  for (const Route& route : routes()) {
    json::value& item = paths[json::string_view(route.path.data(), route.path.size())];
    if (!item.is_object()) item = json::object();
    item.as_object()[lowercase(route.method)] = operation_of(route);
  }
  return {{"openapi", "3.1.0"},
          {"info",
           {{"title", "Louhi"},
            {"version", kVersion},
            {"description",
             "Louhi's API: distributed container compute. Every request carries Authorization: Bearer <access token>; none is answered without one."}}},
          {"security", json::array{json::object{{"bearer", json::array()}}}},
          {"paths", std::move(paths)},
          {"components", json::parse(json::string_view(components.data(), components.size()))}};
}

}  // namespace louhi::api
