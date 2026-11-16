//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "dannan/contract/time_source.h"
#include "louhi/api/client.h"
#include "lugh/client/https.h"

// louhi-ctl: the API from a terminal (design, "API and sign-on"), everything
// but its main(), which only binds it to the real issuer, API, files and
// clocks. It signs in to a Lugh by the device flow (RFC 8628), keeps the
// tokens in a credentials file only its owner can read, refreshes the access
// token when less than a minute of it is left, and prints each answer's JSON.
// Built only with LOUHI_BUILD_LUGH.
namespace louhi::ctl {

// A command line that cannot be acted on: louhi-ctl exits 2.
struct Usage : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// The command line: flags (each --name VALUE, some repeatable) and the rest.
struct Args {
  std::map<std::string, std::vector<std::string>> flags;
  std::vector<std::string> positional;

  // The flag's one value; throws Usage when it is given more than once.
  std::optional<std::string> flag(const std::string& name) const;
  std::vector<std::string> all(const std::string& name) const;
};

// The arguments after the program's name. Throws Usage for a flag with no
// value.
Args parse(const std::vector<std::string>& arguments);

// What login keeps: where to sign in again and the tokens.
struct Credentials {
  std::string issuer;
  std::string client_id;
  std::string endpoint;
  std::vector<std::string> scopes;
  std::string access_token;
  std::string refresh_token;
  uint64_t expires_unix = 0;
  bool operator==(const Credentials&) const = default;
};

std::string encode(const Credentials& credentials);
// The file's text, its own owner's: a field missing or of another type is
// empty. Throws std::runtime_error for text that is not a JSON object.
Credentials decode(std::string_view text);

// Where the credentials are kept. They are bearer credentials: the file is
// readable by its owner alone.
class CredentialFile {
 public:
  virtual ~CredentialFile() = default;
  // The file's text; nothing when there is no file.
  virtual std::optional<std::string> read() = 0;
  // Replaces the file whole, owner-only. Throws std::runtime_error when it
  // cannot.
  virtual void write(std::string_view text) = 0;
  // Removes it; no file is no error.
  virtual void remove() = 0;
};

// The file at a path, replaced by Dannan's durable_replace_file.
class DiskCredentialFile final : public CredentialFile {
 public:
  explicit DiskCredentialFile(std::filesystem::path path) : _path(std::move(path)) {}
  std::optional<std::string> read() override;
  void write(std::string_view text) override;
  void remove() override;

 private:
  const std::filesystem::path _path;
};

// A variable of the environment; nothing when it is unset.
using Getenv = std::function<std::optional<std::string>(const std::string&)>;

// --credentials, else $LOUHI_CREDENTIALS, else
// $HOME/.config/louhi/credentials.json.
std::filesystem::path credentials_path(const Args& args, const Getenv& getenv);

// The request a command makes.
struct Call {
  std::string method;
  std::string target;
  std::string body;
  bool operator==(const Call&) const = default;
};

// The call for args.positional (a command and its operands); `read` reads a
// file an operand names, "-" for stdin. Throws Usage for a command unknown,
// given the wrong operands, or with a number that is not one.
Call command(const Args& args, const std::function<std::string(const std::string&)>& read);

// What louhi-ctl reaches, each made from the command line, so tests hand in
// fakes.
struct Environment {
  // HTTPS to the issuer, chaining to the roots in `ca` (empty: the
  // system's).
  std::function<std::shared_ptr<lugh::client::Https>(const std::string& ca)> issuer;
  // The API at `endpoint`, an https one chaining to `ca`.
  std::function<std::shared_ptr<api::Requester>(const api::Endpoint& endpoint, const std::string& ca)> api;
  std::function<std::unique_ptr<CredentialFile>(const std::filesystem::path&)> credentials;
  // A file a command names; "-" is stdin.
  std::function<std::string(const std::string&)> read;
  Getenv getenv;
  const dannan::contract::WallClock* wall = nullptr;
  // What the device flow's polls wait by.
  const dannan::contract::TimeSource* steady = nullptr;
  // Where every answer is delivered; another thread runs it.
  boost::asio::any_io_executor executor;
  // The answer's JSON.
  std::ostream* out = nullptr;
  // Everything for a person: how to sign in, and why a command failed.
  std::ostream* err = nullptr;
};

// Runs louhi-ctl with `arguments` (those after the program's name): 0 for a
// 2xx answer and for a login or logout done, 1 for any other answer or
// failure, 2 for a usage error. Waits for each answer, at most a minute
// beyond its deadline (a sign-in, until its code expires).
int run(const std::vector<std::string>& arguments, const Environment& environment);

}  // namespace louhi::ctl
