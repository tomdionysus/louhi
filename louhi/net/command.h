//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/io_context.hpp>
#include <cstddef>
#include <string>
#include <vector>

#include "louhi/runtime/driver.h"

// Running a program on this node (design, "A task's network namespace"): how
// Louhi drives `ip` and `nft`, behind an interface so tests use fakes.
// Interim: Louhi and Macha both need one, so it moves into Dannan (the
// stack's plan, Dannan's item 23); Louhi moves onto Dannan's when it ships. The
// argument list is passed as it is, never through a shell, so no argument is
// ever parsed; the caller checks every name it puts in one.
namespace louhi::net {

// A program and its arguments (argv[0] found on a fixed PATH of the system's
// own directories), and what is written to its stdin.
struct Invocation {
  std::vector<std::string> argv;
  std::string input;
};

// How it ended: its exit code (128 + the signal for one killed), and its
// stdout and stderr together, cut at the runner's bound.
struct CommandResult {
  int exit_code = 0;
  std::string output;
  bool truncated = false;
};

class Commands {
 public:
  virtual ~Commands() = default;
  // Runs `invocation` until it exits or `deadline`, when it is killed and the
  // call fails timed_out. A program that cannot be started fails
  // unavailable, naming it. Completes once, posted to `executor`, never
  // inline. Throws std::invalid_argument for an empty argument list.
  // Thread-safe.
  virtual void run(Invocation invocation, runtime::Deadline deadline, runtime::Executor executor, runtime::Completion<CommandResult> done) = 0;
};

// Commands as child processes (posix_spawn), their pipes and deadline on the
// io context, which outlives this.
class ProcessCommands final : public Commands {
 public:
  // The most output kept of one run.
  static constexpr size_t max_output = 64 * 1024;

  explicit ProcessCommands(boost::asio::io_context& io) : _io(io) {}
  void run(Invocation invocation, runtime::Deadline deadline, runtime::Executor executor, runtime::Completion<CommandResult> done) override;

 private:
  boost::asio::io_context& _io;
};

}  // namespace louhi::net
