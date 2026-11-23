//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "louhi/runtime/host_files.h"

#include <fcntl.h>
#include <linux/dqblk_xfs.h>
#include <linux/fs.h>
#include <linux/quota.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <functional>
#include <system_error>

namespace louhi::runtime {

namespace fs = std::filesystem;

namespace {

std::runtime_error failure(const std::string& what) { return std::runtime_error(what + ": " + std::strerror(errno)); }

// A name the driver made (labels.h, volume_name): one path segment.
void require_segment(const std::string& name) {
  if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos || name.find('\0') != std::string::npos)
    throw std::invalid_argument("an output directory's name is not one path segment");
}

class Descriptor {
 public:
  explicit Descriptor(int fd) : _fd(fd) {}
  ~Descriptor() {
    if (_fd >= 0) ::close(_fd);
  }
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
  int get() const { return _fd; }

 private:
  int _fd;
};

// The project a directory's quota is kept under: its own, from its path, in
// a range of its own (the upper half of what xfs allows less the top bit), so
// it is not Docker's, which counts up from the project of its root. Two
// directories that hash alike share one bound: never more room, only less.
uint32_t project_of(const std::string& path) { return 0x40000000u | static_cast<uint32_t>(std::hash<std::string>{}(path) & 0x3fffffffu); }

void bound(const std::string& path, uint64_t bytes) {
  const Descriptor dir(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (dir.get() < 0) throw failure("opening " + path);
  const uint32_t project = project_of(path);
  struct fsxattr attributes{};
  if (::ioctl(dir.get(), FS_IOC_FSGETXATTR, &attributes) != 0) throw failure("reading the attributes of " + path);
  attributes.fsx_projid = project;
  attributes.fsx_xflags |= FS_XFLAG_PROJINHERIT;
  if (::ioctl(dir.get(), FS_IOC_FSSETXATTR, &attributes) != 0) throw failure("setting the project of " + path);

  struct fs_disk_quota quota{};
  quota.d_version = FS_DQUOT_VERSION;
  quota.d_flags = FS_PROJ_QUOTA;
  quota.d_id = project;
  quota.d_fieldmask = FS_DQ_BHARD | FS_DQ_BSOFT;
  // In basic blocks of 512 bytes, rounded up.
  quota.d_blk_hardlimit = (bytes + 511) / 512;
  quota.d_blk_softlimit = quota.d_blk_hardlimit;
#ifdef SYS_quotactl_fd
  if (::syscall(SYS_quotactl_fd, dir.get(), QCMD(Q_XSETQLIM, PRJQUOTA), project, &quota) != 0)
    throw failure("bounding " + path + " (it needs xfs mounted with project quotas)");
#else
  errno = ENOSYS;
  throw failure("bounding " + path);
#endif
}

// Clears a project's limits on the file system holding `root`.
void unbound(const std::string& root, uint32_t project) {
  const Descriptor dir(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (dir.get() < 0) throw failure("opening " + root);
  struct fs_disk_quota quota{};
  quota.d_version = FS_DQUOT_VERSION;
  quota.d_flags = FS_PROJ_QUOTA;
  quota.d_id = project;
  quota.d_fieldmask = FS_DQ_BHARD | FS_DQ_BSOFT;
#ifdef SYS_quotactl_fd
  if (::syscall(SYS_quotactl_fd, dir.get(), QCMD(Q_XSETQLIM, PRJQUOTA), project, &quota) != 0) throw failure("clearing the bound under " + root);
#endif
}

}  // namespace

std::string LinuxHostFiles::resolve_inside(const std::string& root, const std::string& relative) {
  std::error_code ec;
  const fs::path real_root = fs::canonical(root, ec);
  if (ec) throw OutsideDirectory("the host directory " + root + " cannot be resolved: " + ec.message());
  const fs::path real = fs::canonical(real_root / relative, ec);
  if (ec) throw OutsideDirectory("the path " + relative + " in " + root + " cannot be resolved: " + ec.message());
  const auto [ends, at] = std::mismatch(real_root.begin(), real_root.end(), real.begin(), real.end());
  if (ends != real_root.end()) throw OutsideDirectory("the path " + relative + " leads out of " + root);
  if (!fs::is_directory(real, ec)) throw OutsideDirectory("the path " + relative + " in " + root + " is not a directory");
  return real.string();
}

void LinuxHostFiles::make_output(const std::string& root, const std::string& name, uint32_t uid, uint32_t gid, uint64_t bytes) {
  require_segment(name);
  if (!fs::is_directory(root)) throw std::runtime_error("the output area " + root + " is not a directory");
  const std::string path = (fs::path(root) / name).string();
  if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) throw failure("making " + path);
  // Never through a link a task might have left in its place.
  struct stat info{};
  if (::lstat(path.c_str(), &info) != 0) throw failure("reading " + path);
  if (!S_ISDIR(info.st_mode)) throw std::runtime_error(path + " is not a directory");
  // Bounded before it is given away, so the node opens a directory it owns.
  if (bytes != 0) bound(path, bytes);
  if (::lchown(path.c_str(), uid, gid) != 0) throw failure("giving " + path + " to its owner");
  if (::chmod(path.c_str(), 0700) != 0) throw failure("making " + path + " its owner's alone");
}

void LinuxHostFiles::remove_output(const std::string& root, const std::string& name) {
  require_segment(name);
  const std::string path = (fs::path(root) / name).string();
  // The project its bound was kept under, read before the directory goes.
  uint32_t project = 0;
  {
    const Descriptor dir(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct fsxattr attributes{};
    if (dir.get() >= 0 && ::ioctl(dir.get(), FS_IOC_FSGETXATTR, &attributes) == 0) project = attributes.fsx_projid;
  }
  std::error_code ec;
  // remove_all removes a link, never what it points to.
  fs::remove_all(path, ec);
  if (ec) throw std::runtime_error("removing " + path + ": " + ec.message());
  // Its limit goes with it, so nothing of the bound stays on the file
  // system; only a project in Louhi's range (project_of), never Docker's.
  if (project >= 0x40000000u) unbound(root, project);
}

}  // namespace louhi::runtime
