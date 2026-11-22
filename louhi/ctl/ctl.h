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

#include "dannan/config/config.h"
#include "dannan/contract/time_source.h"
#include "louhi/api/client.h"
#include "lugh/client/https.h"

// louhi-ctl: the API from a terminal (design, "API and sign-on"), everything
// but its main(), which only binds it to the real issuer, API, files and
// clocks. It signs in to a Lugh by the device flow (RFC 8628), or, given a
// client secret file, as a service principal by the client credentials grant;
// keeps the settings it signed in with in a YAML configuration file and the
// tokens in a YAML credentials file only its owner can read; renews the
// access token when less than a minute of it is left (by its refresh token,
// else by the client credentials grant again); and prints each answer's JSON.
// The client secret stays in its own file: louhi-ctl keeps the file's path,
// never the secret.
// Built only with LOUHI_BUILD_LUGH.
namespace louhi::ctl {

// A command line that cannot be acted on: louhi-ctl exits 2.
struct Usage : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// A variable of the environment; nothing when it is unset.
using Getenv = std::function<std::optional<std::string>(const std::string&)>;

// The settings, layered by the stack's one reader (dannan/config/config.h):
// the configuration file (--config PATH, else $LOUHI_CONFIG, else
// $HOME/.config/louhi/config.yaml when it exists), then the environment,
// then the flags. Keys: issuer, client_id, client_secret_file, endpoints and
// scopes (lists), ca, credentials, and command.* for the flags one command
// takes.
struct Args {
  dannan::config::Settings settings;
  // The command and its operands, in order.
  std::vector<std::string> positional;

  // The setting's one value, nothing when no layer gave it.
  std::optional<std::string> flag(const std::string& key) const;
  std::vector<std::string> all(const std::string& key) const;
};

// The arguments after the program's name, with the environment. Throws Usage
// for an unknown flag, a flag with no value, a single-valued flag given twice,
// and a configuration file named but missing or that cannot be read. A
// variable set but empty counts as unset.
Args read_args(const std::vector<std::string>& arguments, const Getenv& getenv);

// What login keeps beside the configuration: the tokens, and the issuer and
// client they were issued by and to.
struct Credentials {
  std::string issuer;
  std::string client_id;
  std::string access_token;
  std::string refresh_token;
  uint64_t expires_unix = 0;
  bool operator==(const Credentials&) const = default;
};

// YAML, a map of the fields above.
std::string encode(const Credentials& credentials);
// The file's text, its own owner's: a field missing or of another type is
// empty. Throws std::runtime_error for text that is not a YAML map.
Credentials decode(std::string_view text);

// A file louhi-ctl keeps: its configuration or its credentials. The
// credentials are bearer credentials: the file is readable by its owner
// alone.
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
  // Whether no one but its owner may read it. A client secret in a file others
  // may read is refused, as ssh refuses such a key.
  virtual bool owner_only() = 0;
};

// The file at a path, replaced by Dannan's durable_replace_file and then made
// owner-only (0600). Read with a bound of 64 KiB.
class DiskCredentialFile final : public CredentialFile {
 public:
  explicit DiskCredentialFile(std::filesystem::path path) : _path(std::move(path)) {}
  std::optional<std::string> read() override;
  void write(std::string_view text) override;
  void remove() override;
  bool owner_only() override;

 private:
  const std::filesystem::path _path;
};

// --credentials, else $LOUHI_CREDENTIALS, else the configuration's
// credentials, else $HOME/.config/louhi/credentials.yaml.
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
  // The configuration login writes, and the credentials.
  std::function<std::unique_ptr<CredentialFile>(const std::filesystem::path&)> file;
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
// failure, 2 for a usage error, a configuration that cannot be used
// included. Waits for each answer, at most a minute beyond its deadline (a
// sign-in, until its code expires).
//
// Login writes the issuer, client id, endpoints and scopes it was given (and
// the ca, when one was) to the configuration file in force, replacing it
// whole, and the tokens to the credentials file. Credentials kept by an older
// louhi-ctl, credentials.json beside the default credentials.yaml, are moved
// to the YAML files at the first call, so no one signs in again.
//
// Any node serves the API, so a call is made to each endpoint in turn
// (--endpoint, repeatable; else $LOUHI_ENDPOINT, a list separated by commas
// or spaces; else the configuration's) until one answers: past one that
// cannot be connected to, where nothing was sent, and for a GET past one that
// gave no answer too. A write that may have reached a node is never sent to
// another; the person is told it may have happened. Any HTTP answer, an
// error's included, is the answer.
int run(const std::vector<std::string>& arguments, const Environment& environment);

}  // namespace louhi::ctl
