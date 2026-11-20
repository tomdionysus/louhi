//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "louhi/model/ids.h"
#include "louhi/net/command.h"
#include "louhi/runtime/driver.h"

// A task's interface on its node (design, "A task's network namespace"): a
// veth pair whose far end is the task's eth0, in the namespace its sandbox
// container holds, and whose host end carries the route to the task's
// address. What is done is a plan of argument lists, a pure function tested
// as data; IpHost runs it through Commands.
namespace louhi::net {

// The host end's name for a task: "lh" and the first twelve hex digits of its
// id (fourteen characters; the kernel allows fifteen).
std::string host_interface(const model::TaskId& task);
// The task id's first twelve hex digits a host end's name carries; nothing
// for a name that is not a task's.
std::optional<std::string> task_of_interface(const std::string& name);

struct TaskLink {
  // host_interface() of the task.
  std::string host;
  // A process in the sandbox's namespace.
  int sandbox_pid = 0;
  // The task's address with its subnet's length: "10.0.1.5/24".
  std::string address;
  // The VPC's routing domain (a VRF device), none for the default one.
  std::string vrf;
};

// The commands that attach `link`, in order. Throws std::invalid_argument for
// a host end that is not a task's, a process id that is not one, an address
// that is not IPv4 with a length of 16 to 28, or a VRF name that is not one.
std::vector<std::vector<std::string>> attach_plan(const TaskLink& link);
// The commands that remove a task's pair (its far end goes with it).
std::vector<std::vector<std::string>> detach_plan(const std::string& host);

class Host {
 public:
  virtual ~Host() = default;
  // Attaches, step by step; a step that fails stops the rest, the pair is
  // removed again, and the call fails refused, naming the step and the
  // program's words. Completes once, posted to `executor`. Throws as
  // attach_plan() does.
  virtual void attach(TaskLink link, runtime::Deadline deadline, runtime::Executor executor, runtime::Completion<runtime::Done> done) = 0;
  // Removes the pair; one not there is no error.
  virtual void detach(std::string host, runtime::Deadline deadline, runtime::Executor executor, runtime::Completion<runtime::Done> done) = 0;
};

// Host by `ip`, `nsenter` and `sysctl`. `commands` outlives it.
class IpHost final : public Host {
 public:
  explicit IpHost(Commands& commands) : _commands(commands) {}
  void attach(TaskLink link, runtime::Deadline deadline, runtime::Executor executor, runtime::Completion<runtime::Done> done) override;
  void detach(std::string host, runtime::Deadline deadline, runtime::Executor executor, runtime::Completion<runtime::Done> done) override;

 private:
  Commands& _commands;
};

}  // namespace louhi::net
