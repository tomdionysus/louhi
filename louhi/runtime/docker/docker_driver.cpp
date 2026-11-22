//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/runtime/docker/docker_driver.h"

#include <boost/asio/post.hpp>
#include <chrono>
#include <utility>

#include "louhi/model/image.h"
#include "louhi/runtime/docker/api.h"

namespace louhi::runtime::docker {

namespace asio = boost::asio;
using http_verb = boost::beast::http::verb;

namespace {

using Clock = std::chrono::steady_clock;

template <class T>
void complete(const Executor& executor, Completion<T> done, Result<T> result) {
  asio::post(executor, [done_ = std::move(done), result_ = std::move(result)]() mutable { done_(std::move(result_)); });
}

// What one operation of several requests has left of its deadline.
class Budget {
 public:
  explicit Budget(Deadline deadline) : _end(Clock::now() + deadline) {}
  Deadline remaining() const {
    const auto left = std::chrono::duration_cast<Deadline>(_end - Clock::now());
    return left.count() > 0 ? left : Deadline(1);
  }

 private:
  Clock::time_point _end;
};

// A name or id that may go into a request's path.
bool safe_segment(const std::string& text) {
  if (text.empty() || text.size() > 256) return false;
  for (const char c : text) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
    if (!ok) return false;
  }
  return text != "." && text != "..";
}

Error from_status(const Reply& reply, const std::string& what) {
  std::string message = what + ": the runtime answered " + std::to_string(reply.status);
  const std::string detail = parse_error_message(reply.body);
  if (!detail.empty()) message += ": " + detail;
  if (reply.status == 404) return {Error::Kind::not_found, message};
  if (reply.status >= 400 && reply.status < 500) return {Error::Kind::refused, message};
  if (reply.status >= 500) return {Error::Kind::unavailable, message};
  return {Error::Kind::malformed, message};
}

// Runs `parse` over a reply's body, turning a malformed reply into an error.
template <class T, class Parse>
Result<T> parsed(const Reply& reply, const std::string& what, Parse parse) {
  try {
    return Result<T>(parse(reply.body));
  } catch (const MalformedReply& bad) {
    return Error{Error::Kind::malformed, what + ": " + bad.what()};
  }
}

Result<Done> done_unless(const Result<Reply>& got, const std::string& what, std::initializer_list<unsigned> ok, bool absent_is_ok) {
  if (!got.ok()) return got.error();
  for (const unsigned status : ok) {
    if (got.value().status == status) return Done{};
  }
  if (absent_is_ok && got.value().status == 404) return Done{};
  return from_status(got.value(), what);
}

Error unsafe(const std::string& what) { return {Error::Kind::refused, what + " is not a name the runtime takes"}; }

Error not_allowed(const model::ImageReference& reference) {
  return {Error::Kind::refused, "the registry " + Registries::domain_of(reference) + " is not one this node is allowed to use"};
}

}  // namespace

DockerDriver::DockerDriver(asio::any_io_executor io, std::string socket_path, Registries registries, HostDirectories host)
    : _io(std::move(io)), _http(std::make_shared<LocalHttp>(_io, std::move(socket_path))), _registries(std::move(registries)), _host(std::move(host)) {}

std::string DockerDriver::path(const std::string& rest) const { return "/v" + std::string(api_version) + rest; }

LocalHttp::Headers DockerDriver::auth_for(const model::ImageReference& reference) const {
  const std::string domain = Registries::domain_of(reference);
  const RegistryCredential* credential = _registries.credential_for(domain);
  if (credential == nullptr) return {};
  return {{"X-Registry-Auth", registry_auth(*credential, domain)}};
}

void DockerDriver::resolve(const std::string& image, Deadline deadline, Executor executor, Completion<std::string> done) {
  model::ImageReference reference;
  try {
    reference = model::parse_image_reference(image);
  } catch (const std::invalid_argument&) {
    return complete<std::string>(executor, std::move(done), unsafe("the image reference"));
  }
  if (!_registries.allows(reference)) return complete<std::string>(executor, std::move(done), not_allowed(reference));
  if (!reference.digest.empty()) return complete<std::string>(executor, std::move(done), reference.digest);
  _http->request(http_verb::get, path("/distribution/" + image + "/json"), "", auth_for(reference), deadline, executor,
                 [image, done = std::move(done)](Result<Reply> got) mutable {
                   if (!got.ok()) return done(got.error());
                   // The daemon answers 401 or 403 for a registry that refuses
                   // it, and 404 for an unknown image or tag.
                   if (got.value().status != 200) return done(from_status(got.value(), "resolving " + image));
                   done(parsed<std::string>(got.value(), "resolving " + image, [](const std::string& body) { return parse_distribution(body); }));
                 });
}

void DockerDriver::describe(Deadline deadline, Executor executor, Completion<RuntimeInfo> done) {
  const Budget budget(deadline);
  auto http = _http;
  const std::string info_path = path("/info");
  _http->request(http_verb::get, path("/version"), "", budget.remaining(), executor,
                 [http, info_path, budget, executor, done = std::move(done)](Result<Reply> version) mutable {
                   if (!version.ok()) return done(version.error());
                   if (version.value().status != 200) return done(from_status(version.value(), "describing the runtime"));
                   http->request(http_verb::get, info_path, "", budget.remaining(), executor,
                                 [version = std::move(version), done = std::move(done)](Result<Reply> info) mutable {
                                   if (!info.ok()) return done(info.error());
                                   if (info.value().status != 200) return done(from_status(info.value(), "describing the runtime"));
                                   try {
                                     done(parse_runtime(version.value().body, info.value().body));
                                   } catch (const MalformedReply& bad) {
                                     done(Error{Error::Kind::malformed, std::string("describing the runtime: ") + bad.what()});
                                   }
                                 });
                 });
}

void DockerDriver::pull(const std::string& image, Deadline deadline, Executor executor, Completion<ImageInfo> done) {
  const Budget budget(deadline);
  // The reference goes into a query and a path, so it must be one.
  model::ImageReference reference;
  try {
    reference = model::parse_image_reference(image);
  } catch (const std::invalid_argument&) {
    return complete<ImageInfo>(executor, std::move(done), unsafe("the image reference"));
  }
  if (!_registries.allows(reference)) return complete<ImageInfo>(executor, std::move(done), not_allowed(reference));
  auto http = _http;
  auto failure = std::make_shared<std::optional<std::string>>();
  const std::string inspect_path = path("/images/" + image + "/json");
  _http->stream(
      http_verb::post, path("/images/create?fromImage=" + url_encode(image)), "", auth_for(reference), budget.remaining(), executor,
      [failure](std::string line) {
        if (*failure) return;
        try {
          if (auto error = parse_pull_line(line)) *failure = std::move(*error);
        } catch (const MalformedReply&) {
          *failure = "the runtime's pull progress was not understood";
        }
      },
      [http, image, inspect_path, failure, budget, executor, done = std::move(done)](Result<Reply> pulled) mutable {
        if (!pulled.ok()) return done(pulled.error());
        if (pulled.value().status != 200) return done(from_status(pulled.value(), "pulling " + image));
        if (*failure) return done(Error{Error::Kind::refused, "pulling " + image + ": " + **failure});
        http->request(http_verb::get, inspect_path, "", budget.remaining(), executor, [image, done = std::move(done)](Result<Reply> inspected) mutable {
          if (!inspected.ok()) return done(inspected.error());
          if (inspected.value().status != 200) return done(from_status(inspected.value(), "inspecting " + image));
          done(parsed<ImageInfo>(inspected.value(), "inspecting " + image,
                                 [&image](const std::string& body) { return ImageInfo{image, parse_image_digest(body, image)}; }));
        });
      });
}

void DockerDriver::create_volume(const VolumeSpec& spec, Deadline deadline, Executor executor, Completion<Done> done) {
  if (!safe_segment(spec.name)) return complete<Done>(executor, std::move(done), unsafe("the volume name"));
  // A host directory is resolved, and an output directory made, before the
  // daemon is asked to bind it.
  std::string device;
  try {
    if (spec.kind == VolumeKind::host) {
      const auto source = _host.sources.find(spec.host_directory);
      if (source == _host.sources.end() || _host.files == nullptr)
        return complete<Done>(executor, std::move(done), Error{Error::Kind::refused, "this node lists no host directory " + spec.host_directory});
      device = _host.files->resolve_inside(source->second, spec.host_path);
    } else if (spec.kind == VolumeKind::output) {
      if (_host.output_root.empty() || _host.files == nullptr)
        return complete<Done>(executor, std::move(done), Error{Error::Kind::refused, "this node has no output area"});
      _host.files->make_output(_host.output_root, spec.name, spec.owner_uid, spec.owner_gid, spec.size_bytes);
      device = _host.output_root + "/" + spec.name;
    }
  } catch (const std::invalid_argument& refused) {
    return complete<Done>(executor, std::move(done), Error{Error::Kind::refused, refused.what()});
  } catch (const std::runtime_error& failed) {
    return complete<Done>(executor, std::move(done), Error{Error::Kind::unavailable, failed.what()});
  }
  std::string body;
  try {
    body = create_volume_body(spec, device);
  } catch (const std::invalid_argument& bad) {
    return complete<Done>(executor, std::move(done), Error{Error::Kind::refused, bad.what()});
  }
  _http->request(http_verb::post, path("/volumes/create"), std::move(body), deadline, executor,
                 [done = std::move(done)](Result<Reply> got) { done(done_unless(got, "creating a volume", {200, 201}, false)); });
}

void DockerDriver::remove_volume(const std::string& name, Deadline deadline, Executor executor, Completion<Done> done) {
  if (!safe_segment(name)) return complete<Done>(executor, std::move(done), unsafe("the volume name"));
  _http->request(http_verb::delete_, path("/volumes/" + name), "", deadline, executor, [host = _host, name, done = std::move(done)](Result<Reply> got) {
    auto removed = done_unless(got, "removing a volume", {204}, true);
    // An output volume's directory goes once the daemon has let go of it.
    if (removed.ok() && !host.output_root.empty() && host.files != nullptr) {
      try {
        host.files->remove_output(host.output_root, name);
      } catch (const std::exception& failed) {
        return done(Error{Error::Kind::unavailable, failed.what()});
      }
    }
    done(std::move(removed));
  });
}

void DockerDriver::list_volumes(Deadline deadline, Executor executor, Completion<std::vector<VolumeInfo>> done) {
  _http->request(http_verb::get, path("/volumes?" + louhi_filter()), "", deadline, executor, [done = std::move(done)](Result<Reply> got) {
    if (!got.ok()) return done(got.error());
    if (got.value().status != 200) return done(from_status(got.value(), "listing volumes"));
    done(parsed<std::vector<VolumeInfo>>(got.value(), "listing volumes", [](const std::string& body) { return parse_volumes(body); }));
  });
}

void DockerDriver::create(const ContainerSpec& spec, Deadline deadline, Executor executor, Completion<std::string> done) {
  if (!safe_segment(spec.name)) return complete<std::string>(executor, std::move(done), unsafe("the container name"));
  std::string body;
  try {
    body = create_container_body(spec);
  } catch (const std::invalid_argument& bad) {
    return complete<std::string>(executor, std::move(done), Error{Error::Kind::refused, bad.what()});
  }
  _http->request(http_verb::post, path("/containers/create?name=" + url_encode(spec.name)), std::move(body), deadline, executor,
                 [done = std::move(done)](Result<Reply> got) {
                   if (!got.ok()) return done(got.error());
                   if (got.value().status != 201) return done(from_status(got.value(), "creating a container"));
                   done(parsed<std::string>(got.value(), "creating a container", [](const std::string& reply) { return parse_created_id(reply); }));
                 });
}

void DockerDriver::start(const std::string& id, Deadline deadline, Executor executor, Completion<Done> done) {
  if (!safe_segment(id)) return complete<Done>(executor, std::move(done), unsafe("the container id"));
  _http->request(http_verb::post, path("/containers/" + id + "/start"), "", deadline, executor,
                 [done = std::move(done)](Result<Reply> got) { done(done_unless(got, "starting a container", {204, 304}, false)); });
}

void DockerDriver::stop(const std::string& id, std::chrono::seconds grace, Deadline deadline, Executor executor, Completion<Done> done) {
  if (!safe_segment(id)) return complete<Done>(executor, std::move(done), unsafe("the container id"));
  _http->request(http_verb::post, path("/containers/" + id + "/stop?t=" + std::to_string(grace.count())), "", deadline, executor,
                 [done = std::move(done)](Result<Reply> got) { done(done_unless(got, "stopping a container", {204, 304}, false)); });
}

void DockerDriver::remove(const std::string& id, Deadline deadline, Executor executor, Completion<Done> done) {
  if (!safe_segment(id)) return complete<Done>(executor, std::move(done), unsafe("the container id"));
  _http->request(http_verb::delete_, path("/containers/" + id + "?force=true&v=true"), "", deadline, executor,
                 [done = std::move(done)](Result<Reply> got) { done(done_unless(got, "removing a container", {204}, true)); });
}

void DockerDriver::list(Deadline deadline, Executor executor, Completion<std::vector<ContainerInfo>> done) {
  // The list names Louhi's containers; each is then inspected for its state,
  // one at a time. One gone in between is left out, as it is no longer here.
  struct Walk {
    std::shared_ptr<LocalHttp> http;
    std::string prefix;
    Budget budget;
    Executor executor;
    Completion<std::vector<ContainerInfo>> done;
    std::vector<std::string> ids;
    size_t next = 0;
    std::vector<ContainerInfo> found;

    static void step(const std::shared_ptr<Walk>& walk) {
      if (walk->next == walk->ids.size()) return walk->done(std::move(walk->found));
      const std::string id = walk->ids[walk->next++];
      if (!safe_segment(id)) return walk->done(Error{Error::Kind::malformed, "listing containers: the runtime named an id that is not one"});
      walk->http->request(
          http_verb::get, walk->prefix + "/containers/" + id + "/json", "", walk->budget.remaining(), walk->executor, [walk](Result<Reply> got) {
            if (!got.ok()) return walk->done(got.error());
            if (got.value().status == 404) return step(walk);
            if (got.value().status != 200) return walk->done(from_status(got.value(), "inspecting a container"));
            auto info = parsed<ContainerInfo>(got.value(), "inspecting a container", [](const std::string& body) { return parse_inspect(body); });
            if (!info.ok()) return walk->done(info.error());
            walk->found.push_back(std::move(info.value()));
            step(walk);
          });
    }
  };
  auto walk = std::make_shared<Walk>(Walk{_http, path(""), Budget(deadline), executor, std::move(done), {}, 0, {}});
  _http->request(http_verb::get, path("/containers/json?all=true&" + louhi_filter()), "", walk->budget.remaining(), executor, [walk](Result<Reply> got) {
    if (!got.ok()) return walk->done(got.error());
    if (got.value().status != 200) return walk->done(from_status(got.value(), "listing containers"));
    auto ids = parsed<std::vector<std::string>>(got.value(), "listing containers", [](const std::string& body) { return parse_container_ids(body); });
    if (!ids.ok()) return walk->done(ids.error());
    walk->ids = std::move(ids.value());
    Walk::step(walk);
  });
}

uint64_t DockerDriver::watch(Executor executor, std::function<void(ContainerEvent)> event, std::function<void(Error)> lost) {
  uint64_t id = 0;
  {
    dannan::contract::Lock lock(_mutex);
    id = _next_watch++;
  }
  // Set by unwatch(), so a caller that stopped watching hears nothing more,
  // the stream's end included.
  auto stopped = std::make_shared<std::atomic<bool>>(false);
  auto stream = _http->stream(
      http_verb::get, path("/events?" + events_filter()), "", Deadline(0), executor,
      [stopped, event = std::move(event)](std::string line) {
        if (*stopped) return;
        try {
          if (auto parsed_event = parse_event(line)) event(std::move(*parsed_event));
        } catch (const MalformedReply&) {
          // One event not understood is skipped: events only prompt a re-list.
        }
      },
      [stopped, lost = std::move(lost)](Result<Reply> ended) {
        if (*stopped) return;
        if (!ended.ok()) return lost(ended.error());
        if (ended.value().status != 200) return lost(from_status(ended.value(), "watching events"));
        lost(Error{Error::Kind::unavailable, "the runtime ended its event stream"});
      });
  dannan::contract::Lock lock(_mutex);
  _watches.emplace(id, Watch{std::move(stream), std::move(stopped)});
  return id;
}

void DockerDriver::unwatch(uint64_t id) {
  Watch watch;
  {
    dannan::contract::Lock lock(_mutex);
    const auto found = _watches.find(id);
    if (found == _watches.end()) return;
    watch = std::move(found->second);
    _watches.erase(found);
  }
  *watch.stopped = true;
  watch.stream->cancel();
}

}  // namespace louhi::runtime::docker
