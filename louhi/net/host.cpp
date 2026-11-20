//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/net/host.h"

#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/post.hpp>
#include <memory>
#include <stdexcept>

#include "dannan/identity/crypto.h"

namespace louhi::net {

namespace {

namespace asio = boost::asio;
using Argv = std::vector<std::string>;

constexpr size_t id_digits = 12;

bool is_hex(const std::string& text) {
  for (const char c : text)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

void require_host(const std::string& host) {
  if (!task_of_interface(host)) throw std::invalid_argument("a host end is \"lh\" and twelve hex digits of a task id");
}

void require_vrf(const std::string& vrf) {
  bool ok = !vrf.empty() && vrf.size() <= 15;
  for (const char c : vrf) ok = ok && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-');
  if (!ok) throw std::invalid_argument("a VRF's name is 1 to 15 of lowercase letters, digits and '-'");
}

// "a.b.c.d/n", one spelling, n from 16 to 28. The address itself, not its
// range: host bits are what it is.
std::string require_address(const std::string& text) {
  const auto slash = text.find('/');
  if (slash == std::string::npos) throw std::invalid_argument("a task's address is a.b.c.d/n");
  const std::string address = text.substr(0, slash);
  const std::string length = text.substr(slash + 1);
  boost::system::error_code failed;
  const auto parsed = asio::ip::make_address_v4(address, failed);
  if (failed || parsed.to_string() != address) throw std::invalid_argument("a task's address is not IPv4");
  if (length.empty() || length.size() > 2 || length.find_first_not_of("0123456789") != std::string::npos)
    throw std::invalid_argument("a task's address length is not a number");
  const int bits = std::stoi(length);
  if (bits < 16 || bits > 28 || std::to_string(bits) != length) throw std::invalid_argument("a task's address length is not 16 to 28");
  return address;
}

Argv inside(int pid, Argv command) {
  Argv out{"nsenter", "--target", std::to_string(pid), "--net"};
  out.insert(out.end(), command.begin(), command.end());
  return out;
}

std::string described(const Argv& argv) {
  std::string out;
  for (const auto& a : argv) out += (out.empty() ? "" : " ") + a;
  return out;
}

// Runs a plan's steps one after another; the first that fails ends it.
struct Steps : std::enable_shared_from_this<Steps> {
  Commands& commands;
  std::vector<Argv> plan;
  runtime::Deadline deadline;
  runtime::Executor executor;
  std::function<void(std::optional<std::string>)> finished;
  size_t at = 0;

  Steps(Commands& c, std::vector<Argv> p, runtime::Deadline d, runtime::Executor e, std::function<void(std::optional<std::string>)> f)
      : commands(c), plan(std::move(p)), deadline(d), executor(std::move(e)), finished(std::move(f)) {}

  void next() {
    if (at == plan.size()) return finished(std::nullopt);
    commands.run(Invocation{plan[at], ""}, deadline, executor, [self = shared_from_this()](runtime::Result<CommandResult> got) {
      const Argv& step = self->plan[self->at];
      if (!got.ok()) return self->finished(described(step) + ": " + got.error().message);
      if (got.value().exit_code != 0) return self->finished(described(step) + ": exit " + std::to_string(got.value().exit_code) + ": " + got.value().output);
      ++self->at;
      self->next();
    });
  }
};

void post_result(const runtime::Executor& executor, runtime::Completion<runtime::Done> done, std::optional<runtime::Error> error) {
  asio::post(executor, [done = std::move(done), error = std::move(error)]() mutable {
    if (error) return done(runtime::Result<runtime::Done>(std::move(*error)));
    done(runtime::Result<runtime::Done>(runtime::Done{}));
  });
}

}  // namespace

std::string host_interface(const model::TaskId& task) { return "lh" + dannan::identity::hex(task.bytes).substr(0, id_digits); }

std::optional<std::string> task_of_interface(const std::string& name) {
  if (name.size() != 2 + id_digits || name.rfind("lh", 0) != 0) return std::nullopt;
  std::string digits = name.substr(2);
  if (!is_hex(digits)) return std::nullopt;
  return digits;
}

std::vector<Argv> attach_plan(const TaskLink& link) {
  require_host(link.host);
  if (link.sandbox_pid <= 1) throw std::invalid_argument("a sandbox's process id is not one");
  const std::string address = require_address(link.address);
  if (!link.vrf.empty()) require_vrf(link.vrf);
  const std::string peer = "lp" + link.host.substr(2);
  const int pid = link.sandbox_pid;

  std::vector<Argv> plan{
      {"ip", "link", "add", link.host, "type", "veth", "peer", "name", peer},
      {"ip", "link", "set", peer, "netns", std::to_string(pid)},
      inside(pid, {"ip", "link", "set", peer, "name", "eth0"}),
      inside(pid, {"ip", "address", "add", link.address, "dev", "eth0"}),
      inside(pid, {"ip", "link", "set", "lo", "up"}),
      inside(pid, {"ip", "link", "set", "eth0", "up"}),
      // The task needs no gateway address: the host end answers every ARP.
      inside(pid, {"ip", "route", "add", "default", "dev", "eth0"}),
      {"sysctl", "-w", "net.ipv4.conf." + link.host + ".proxy_arp=1"},
  };
  if (!link.vrf.empty()) plan.push_back({"ip", "link", "set", link.host, "master", link.vrf});
  plan.push_back({"ip", "link", "set", link.host, "up"});
  Argv route{"ip", "route", "add", address + "/32", "dev", link.host};
  if (!link.vrf.empty()) {
    route.push_back("vrf");
    route.push_back(link.vrf);
  }
  plan.push_back(std::move(route));
  return plan;
}

std::vector<Argv> detach_plan(const std::string& host) {
  require_host(host);
  return {{"ip", "link", "delete", host}};
}

void IpHost::attach(TaskLink link, runtime::Deadline deadline, runtime::Executor executor, runtime::Completion<runtime::Done> done) {
  auto plan = attach_plan(link);
  auto undo = detach_plan(link.host).front();
  Commands& commands = _commands;
  auto steps = std::make_shared<Steps>(
      _commands, std::move(plan), deadline, executor, [&commands, undo, deadline, executor, done = std::move(done)](std::optional<std::string> failed) mutable {
        if (!failed) return post_result(executor, std::move(done), std::nullopt);
        // What was made is removed again, whatever that removal says.
        commands.run(Invocation{undo, ""}, deadline, executor,
                     [executor, done = std::move(done), failed = std::move(*failed)](runtime::Result<CommandResult>) mutable {
                       post_result(executor, std::move(done), runtime::Error{runtime::Error::Kind::refused, "attaching the task's interface: " + failed});
                     });
      });
  steps->next();
}

void IpHost::detach(std::string host, runtime::Deadline deadline, runtime::Executor executor, runtime::Completion<runtime::Done> done) {
  auto plan = detach_plan(host);
  _commands.run(Invocation{plan.front(), ""}, deadline, executor, [executor, done = std::move(done)](runtime::Result<CommandResult> got) mutable {
    if (!got.ok()) return post_result(executor, std::move(done), got.error());
    const auto& r = got.value();
    // Gone already is what was wanted.
    if (r.exit_code != 0 && r.output.find("Cannot find device") == std::string::npos)
      return post_result(executor, std::move(done), runtime::Error{runtime::Error::Kind::refused, "removing the task's interface: " + r.output});
    post_result(executor, std::move(done), std::nullopt);
  });
}

}  // namespace louhi::net
