//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "dannan/contract/thread_safety.h"
#include "louhi/runtime/docker/local_http.h"
#include "louhi/runtime/driver.h"
#include "louhi/runtime/host_files.h"
#include "louhi/runtime/registries.h"

// The Driver over the Docker Engine API (design, "The runtime"; the maintainer,
// 2026-11-15), spoken over the daemon's local socket. Every request goes to
// the API version Louhi was written against (api.h), so a newer daemon answers
// as that version. Containers are made with the hardened profile (api.h).
//
// A name or id that goes into a request's path is checked to hold only
// letters, digits, '_', '.' and '-' first, so nothing a caller passes can
// change which endpoint is asked.
namespace louhi::runtime::docker {

// The node's directories a task may see (host_files.h): the host directories
// the operator lists, by the name a task definition uses, and the output
// area. `files` outlives the driver; with none, host and output volumes are
// refused.
struct HostDirectories {
  std::map<std::string, std::string> sources;
  std::string output_root;
  HostFiles* files = nullptr;
};

class DockerDriver final : public Driver {
 public:
  // `io` runs the connections. `socket_path` is the daemon's socket,
  // /var/run/docker.sock on a node. `registries` says which registries images
  // come from and how to sign in to each; a reference to any other is refused
  // before the daemon is asked.
  DockerDriver(boost::asio::any_io_executor io, std::string socket_path, Registries registries, HostDirectories host = {});

  void describe(Deadline deadline, Executor executor, Completion<RuntimeInfo> done) override;
  void resolve(const std::string& image, Deadline deadline, Executor executor, Completion<std::string> done) override;
  void pull(const std::string& image, Deadline deadline, Executor executor, Completion<ImageInfo> done) override;
  void create_volume(const VolumeSpec& spec, Deadline deadline, Executor executor, Completion<Done> done) override;
  void remove_volume(const std::string& name, Deadline deadline, Executor executor, Completion<Done> done) override;
  void list_volumes(Deadline deadline, Executor executor, Completion<std::vector<VolumeInfo>> done) override;
  void create(const ContainerSpec& spec, Deadline deadline, Executor executor, Completion<std::string> done) override;
  void start(const std::string& id, Deadline deadline, Executor executor, Completion<Done> done) override;
  void stop(const std::string& id, std::chrono::seconds grace, Deadline deadline, Executor executor, Completion<Done> done) override;
  void remove(const std::string& id, Deadline deadline, Executor executor, Completion<Done> done) override;
  void list(Deadline deadline, Executor executor, Completion<std::vector<ContainerInfo>> done) override;
  void logs(const std::string& id, const LogQuery& query, Deadline deadline, Executor executor, Completion<std::vector<LogLine>> done) override;
  uint64_t watch(Executor executor, std::function<void(ContainerEvent)> event, std::function<void(Error)> lost) override;
  void unwatch(uint64_t id) override;

 private:
  struct Watch {
    std::shared_ptr<LocalHttp::Stream> stream;
    std::shared_ptr<std::atomic<bool>> stopped;
  };

  std::string path(const std::string& rest) const;
  // The X-Registry-Auth header for the reference's registry, when the
  // operator gave credentials for it.
  LocalHttp::Headers auth_for(const model::ImageReference& reference) const;

  boost::asio::any_io_executor _io;
  std::shared_ptr<LocalHttp> _http;
  const Registries _registries;
  const HostDirectories _host;
  dannan::contract::Mutex _mutex;
  uint64_t _next_watch DANNAN_GUARDED_BY(_mutex) = 1;
  std::map<uint64_t, Watch> _watches DANNAN_GUARDED_BY(_mutex);
};

}  // namespace louhi::runtime::docker
