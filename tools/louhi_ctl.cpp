//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// louhi-ctl: the API from a terminal (design, "API and sign-on"). It signs in
// to a Lugh by the device flow (RFC 8628), keeps the settings it signed in
// with in its configuration and the tokens in a credentials file only its
// owner can read, both YAML, refreshes the access token when less than a
// minute of it is left, and prints each answer's JSON on stdout. It exits 0
// for a 2xx answer, 1 for any other answer or failure, 2 for a usage error.
//
//   louhi-ctl login --issuer URL --client-id ID --endpoint URL... [--scope S]... [--ca FILE]
//   louhi-ctl logout
//   louhi-ctl list-projects
//   louhi-ctl put-project PROJECT [--description TEXT]
//   louhi-ctl describe-project PROJECT
//   louhi-ctl put-quota PROJECT FILE            a JSON quota; - reads stdin
//   louhi-ctl describe-quota PROJECT
//   louhi-ctl register-task-definition PROJECT FILE
//   louhi-ctl list-task-definition-families PROJECT
//   louhi-ctl list-task-definitions PROJECT FAMILY
//   louhi-ctl describe-task-definition PROJECT FAMILY REVISION
//   louhi-ctl run-task PROJECT FAMILY [--revision N] [--count N] [--client-token T]
//   louhi-ctl list-tasks PROJECT [--limit N] [--next ID]
//   louhi-ctl describe-task PROJECT TASK
//   louhi-ctl stop-task PROJECT TASK [--reason TEXT]
//   louhi-ctl list-nodes
//   louhi-ctl --version
//
// Before any command: --endpoint URL, repeatable (else $LOUHI_ENDPOINT, a
// list, else the configuration's), each node's API tried in turn until one
// answers (ctl.h, run()), --ca FILE (the roots an https endpoint and the issuer
// chain to; the system's when absent), --credentials FILE (else
// $LOUHI_CREDENTIALS, else ~/.config/louhi/credentials.yaml), --config FILE
// (else $LOUHI_CONFIG, else ~/.config/louhi/config.yaml when it exists).
//
// Settings are read by the stack's one reader (dannan/config/config.h): the
// configuration file, then the environment, then the flags. Its keys are
// issuer, client_id, endpoints (a list), scopes (a list), ca and credentials;
// login writes the first four, and ca when given, replacing the file whole.
//
// Everything but binding it to the world is louhi/ctl/ctl.h, where it is
// tested.

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dannan/contract/time_source.h"
#include "dannan/listen/connector.h"
#include "louhi/api/client.h"
#include "louhi/ctl/ctl.h"
#include "lugh/client/https.h"

namespace {

namespace asio = boost::asio;

// One io context run on a thread of its own while a command waits on it.
struct Io {
  asio::io_context io;
  asio::executor_work_guard<asio::io_context::executor_type> work = asio::make_work_guard(io);
  std::thread thread{[this] { io.run(); }};
  ~Io() {
    work.reset();
    io.stop();
    thread.join();
  }
};

std::string read_file(const std::string& path) {
  if (path == "-") return std::string(std::istreambuf_iterator<char>(std::cin), {});
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read " + path);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

}  // namespace

int main(int argc, char** argv) {
  Io io;
  louhi::ctl::Environment env;
  env.issuer = [&io](const std::string& ca) { return std::make_shared<lugh::client::HttpsClient>(io.io, ca); };
  env.api = [&io](const louhi::api::Endpoint& endpoint, const std::string& ca) {
    std::shared_ptr<const dannan::listen::ClientTrust> trust;
    if (endpoint.tls) trust = std::make_shared<dannan::listen::ClientTrust>(ca);
    return std::make_shared<louhi::api::Client>(io.io, endpoint, trust);
  };
  env.file = [](const std::filesystem::path& path) { return std::make_unique<louhi::ctl::DiskCredentialFile>(path); };
  env.read = read_file;
  env.getenv = [](const std::string& name) -> std::optional<std::string> {
    const char* value = std::getenv(name.c_str());
    if (value == nullptr) return std::nullopt;
    return std::string(value);
  };
  env.wall = &dannan::contract::system_wall_clock();
  env.steady = &dannan::contract::steady_time_source();
  env.executor = io.io.get_executor();
  env.out = &std::cout;
  env.err = &std::cerr;
  return louhi::ctl::run(std::vector<std::string>(argv + 1, argv + argc), env);
}
