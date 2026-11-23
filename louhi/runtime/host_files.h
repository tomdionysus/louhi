//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

// The node's own directories a task may see (the maintainer, 2026-11-22):
// host directories the operator lists, bound read-only, and an output area
// where each output volume is a directory of its own, owned by the runtime's
// root in a container and bounded by a quota. Behind an interface, so the
// driver's tests use a fake; the real one is Linux's.
//
// Every operation is synchronous and quick but for remove_output, which
// walks what a task wrote. Thread-safe.
namespace louhi::runtime {

// A path refused: outside its directory, missing, or not a directory.
class OutsideDirectory : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

class HostFiles {
 public:
  virtual ~HostFiles() = default;

  // The real path of `relative` inside `root`, every symbolic link followed:
  // refused (OutsideDirectory) when it is not an existing directory at or
  // below the real path of `root`.
  virtual std::string resolve_inside(const std::string& root, const std::string& relative) = 0;

  // Makes root/name, a directory owned by uid:gid that only its owner may
  // use, and bounds what is written below it to `bytes` (none when zero) by a
  // project quota. A directory already there is taken as it is, bound again.
  // Throws std::runtime_error when it cannot, naming why.
  virtual void make_output(const std::string& root, const std::string& name, uint32_t uid, uint32_t gid, uint64_t bytes) = 0;

  // Removes root/name and all below it, following no link, and clears the
  // limit of the project its bound was kept under; no directory is no error.
  // Throws std::runtime_error when it cannot.
  virtual void remove_output(const std::string& root, const std::string& name) = 0;
};

// The real file system. A bound needs xfs mounted with project quotas
// (pquota or prjquota); elsewhere make_output with a bound throws.
class LinuxHostFiles final : public HostFiles {
 public:
  std::string resolve_inside(const std::string& root, const std::string& relative) override;
  void make_output(const std::string& root, const std::string& name, uint32_t uid, uint32_t gid, uint64_t bytes) override;
  void remove_output(const std::string& root, const std::string& name) override;
};

}  // namespace louhi::runtime
