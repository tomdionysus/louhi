//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include "louhi/model/records.h"

// One valid record of each kind, every optional part filled, for tests to
// start from and break one field at a time.
namespace louhi::test {

inline model::NodeId node_id(uint8_t fill) {
  model::NodeId id;
  id.bytes.fill(fill);
  return id;
}

inline model::TaskId task_id(uint8_t fill) {
  model::TaskId id;
  id.bytes.fill(fill);
  return id;
}

inline model::Project project() { return {"web", "The public site", 1'700'000'000'000}; }

inline model::Quota quota() {
  model::Quota q;
  q.project = "web";
  q.max_tasks = 50;
  q.max_cpu_millis = 16'000;
  q.max_memory_mib = 32'768;
  q.max_addresses = 64;
  q.max_volumes = 8;
  q.max_load_balancers = 2;
  q.updated_unix_ms = 1'700'000'000'001;
  return q;
}

inline model::TaskDefinition task_definition() {
  model::TaskDefinition d;
  d.project = "web";
  d.family = "frontend";
  d.revision = 3;
  d.cpu_millis = 1000;
  d.memory_mib = 512;
  d.volumes = {{"scratch", model::VolumeKind::scratch, 256}};

  model::ContainerDefinition app;
  app.name = "app";
  app.image = "registry.example.net:5000/team/frontend:1.4.2";
  app.image_digest = "sha256:" + std::string(64, 'a');
  app.entry_point = {"/bin/server"};
  app.command = {"--port", "8080"};
  app.environment = {{"MODE", "production"}, {"_HOME", "/srv"}};
  app.working_directory = "/srv";
  app.ports = {{8080, model::Protocol::tcp}, {8125, model::Protocol::udp}};
  app.mounts = {{"scratch", "/tmp/work", false}};
  app.health_check = model::HealthCheck{{"/bin/check", "--quick"}, 10, 3, 2, 15};
  app.cpu_millis = 750;
  app.memory_mib = 384;
  app.essential = true;
  app.read_only_root = true;

  model::ContainerDefinition sidecar;
  sidecar.name = "log-shipper";
  sidecar.image = "alpine:3.20";
  sidecar.mounts = {{"scratch", "/var/log/app", true}};
  sidecar.essential = false;

  d.containers = {app, sidecar};
  d.run_once = false;
  d.registered_unix_ms = 1'700'000'000'002;
  return d;
}

inline model::Service service() {
  model::Service s;
  s.project = "web";
  s.name = "frontend";
  s.family = "frontend";
  s.revision = 3;
  s.desired_count = 4;
  s.deployment = {50, 150};
  s.spread_failure_domains = true;
  s.updated_unix_ms = 1'700'000'000'003;
  return s;
}

inline model::Task task() {
  model::Task t;
  t.id = task_id(0x11);
  t.project = "web";
  t.family = "frontend";
  t.revision = 3;
  t.service = "frontend";
  t.desired = model::DesiredState::running;
  t.node = node_id(0x22);
  t.placement_epoch = 2;
  t.started_by = "service:frontend";
  t.created_unix_ms = 1'700'000'000'004;
  return t;
}

inline model::TaskStatus task_status() {
  model::TaskStatus s;
  s.task = task_id(0x11);
  s.node = node_id(0x22);
  s.sequence = 9;
  s.state = model::TaskState::running;
  s.reason = "";
  s.containers = {{"app", model::ContainerState::running, std::nullopt, 1, model::Health::healthy},
                  {"log-shipper", model::ContainerState::exited, -9, 0, model::Health::unknown}};
  s.started_unix_ms = 1'700'000'000'005;
  s.observed_unix_ms = 1'700'000'000'006;
  return s;
}

inline model::NodeStatus node_status() {
  model::NodeStatus s;
  s.node = node_id(0x22);
  s.sequence = 40;
  s.louhi_version = "0.1.0";
  s.runtime = "docker";
  s.runtime_version = "26.1.5";
  s.total = {4000, 8192};
  s.allocated = {1000, 512};
  s.tasks = 1;
  s.ready = true;
  s.conditions = {"disk 80% full"};
  s.observed_unix_ms = 1'700'000'000'007;
  return s;
}

}  // namespace louhi::test
