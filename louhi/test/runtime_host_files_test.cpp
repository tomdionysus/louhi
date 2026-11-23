//
// Louhi - The Distributed Compute Layer
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The node's own directories (runtime/host_files.h) on the real file system:
// a path inside a listed directory resolves, every symbolic link followed,
// and one that leads out of it, through ".." or a link, is refused; an output
// directory is made its owner's alone; removing it follows no link a task
// left. Giving it to another owner needs root, as louhi-node runs, and is
// skipped otherwise. The quota needs xfs with project quotas: it runs only
// where LOUHI_TEST_XFS_OUTPUT names a directory on one (node-c), and is
// skipped elsewhere.

#include <fcntl.h>
#include <gtest/gtest.h>
#include <linux/dqblk_xfs.h>
#include <linux/fs.h>
#include <linux/quota.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "dannan/test/fixtures/temp_dir.h"
#include "louhi/runtime/host_files.h"

using namespace louhi::runtime;
namespace fs = std::filesystem;

namespace {

// A project's hard block limit on the file system holding `path`; zero when it
// has none.
uint64_t hard_limit(const std::string& path, uint32_t project) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) return 0;
  struct fs_disk_quota quota{};
  const long got = ::syscall(SYS_quotactl_fd, fd, QCMD(Q_XGETQUOTA, PRJQUOTA), project, &quota);
  ::close(fd);
  return got == 0 ? quota.d_blk_hardlimit : 0;
}

struct HostFilesTest : ::testing::Test {
  dannan::test::TempDir dir;
  LinuxHostFiles files;
  const fs::path work = dir.path() / "work";
  const fs::path outside = dir.path() / "outside";

  HostFilesTest() {
    fs::create_directories(work / "web" / "src");
    fs::create_directories(outside);
    std::ofstream(work / "web" / "readme") << "a file";
  }
};

}  // namespace

TEST_F(HostFilesTest, APathInsideTheDirectoryResolvesToItsRealPath) {
  EXPECT_EQ(files.resolve_inside(work.string(), "web/src"), fs::canonical(work / "web" / "src").string());
  EXPECT_EQ(files.resolve_inside(work.string(), ""), fs::canonical(work).string()) << "the directory itself";
  fs::create_directory_symlink(work / "web" / "src", work / "current");
  EXPECT_EQ(files.resolve_inside(work.string(), "current"), fs::canonical(work / "web" / "src").string()) << "a link that stays inside";
}

TEST_F(HostFilesTest, APathThatLeadsOutIsMissingOrIsNotADirectoryIsRefused) {
  EXPECT_THROW(files.resolve_inside(work.string(), "../outside"), OutsideDirectory);
  fs::create_directory_symlink(outside, work / "away");
  EXPECT_THROW(files.resolve_inside(work.string(), "away"), OutsideDirectory) << "a link out";
  fs::create_directory(dir.path() / "work-two");
  EXPECT_THROW(files.resolve_inside(work.string(), "../work-two"), OutsideDirectory) << "a sibling whose name begins the same";
  EXPECT_THROW(files.resolve_inside(work.string(), "missing"), OutsideDirectory);
  EXPECT_THROW(files.resolve_inside(work.string(), "web/readme"), OutsideDirectory);
  EXPECT_THROW(files.resolve_inside((dir.path() / "none").string(), ""), OutsideDirectory);
}

TEST_F(HostFilesTest, AnOutputDirectoryIsMadeItsOwnersAloneAndRemovedFollowingNoLink) {
  const fs::path out = dir.path() / "out";
  fs::create_directory(out);
  const uint32_t uid = ::geteuid() == 0 ? 100000 : ::geteuid();
  const uint32_t gid = ::geteuid() == 0 ? 100000 : ::getegid();
  files.make_output(out.string(), "louhi-a", uid, gid, 0);
  struct stat info{};
  ASSERT_EQ(::stat((out / "louhi-a").c_str(), &info), 0);
  EXPECT_TRUE(S_ISDIR(info.st_mode));
  EXPECT_EQ(info.st_mode & 0777, 0700u);
  EXPECT_EQ(info.st_uid, uid);
  EXPECT_EQ(info.st_gid, gid);
  EXPECT_NO_THROW(files.make_output(out.string(), "louhi-a", uid, gid, 0)) << "made again, taken as it is";

  // What a task leaves: a file, and a link to something outside.
  std::ofstream(out / "louhi-a" / "result") << "ok";
  std::ofstream(outside / "precious") << "keep";
  fs::create_directory_symlink(outside, out / "louhi-a" / "link");
  files.remove_output(out.string(), "louhi-a");
  EXPECT_FALSE(fs::exists(out / "louhi-a"));
  EXPECT_TRUE(fs::exists(outside / "precious")) << "the link was removed, not what it named";
  EXPECT_NO_THROW(files.remove_output(out.string(), "louhi-a")) << "no directory is no error";

  EXPECT_THROW(files.make_output(out.string(), "../escape", uid, gid, 0), std::invalid_argument);
  EXPECT_THROW(files.make_output(out.string(), "a/b", uid, gid, 0), std::invalid_argument);
  EXPECT_THROW(files.remove_output(out.string(), ".."), std::invalid_argument);
  EXPECT_THROW(files.make_output((dir.path() / "none").string(), "louhi-a", uid, gid, 0), std::runtime_error);
}

TEST_F(HostFilesTest, AnOutputDirectoryInThePlaceOfALinkIsRefused) {
  const fs::path out = dir.path() / "out";
  fs::create_directory(out);
  fs::create_directory_symlink(outside, out / "louhi-a");
  EXPECT_THROW(files.make_output(out.string(), "louhi-a", ::geteuid(), ::getegid(), 0), std::runtime_error);
}

TEST_F(HostFilesTest, ABoundNeedsProjectQuotas) {
  const char* xfs = std::getenv("LOUHI_TEST_XFS_OUTPUT");
  if (xfs == nullptr) {
    // Here (no xfs with project quotas) a bound cannot be kept: refused, not
    // silently dropped.
    const fs::path out = dir.path() / "out";
    fs::create_directory(out);
    EXPECT_THROW(files.make_output(out.string(), "louhi-b", ::geteuid(), ::getegid(), 1 << 20), std::runtime_error);
    GTEST_SKIP() << "LOUHI_TEST_XFS_OUTPUT is not set: the bound itself is not tried";
  }
  const std::string name = "louhi-test-" + std::to_string(::getpid());
  files.make_output(xfs, name, ::geteuid(), ::getegid(), 1 << 20);
  // Two MiB into a one MiB bound fails.
  std::ofstream big(fs::path(xfs) / name / "big", std::ios::binary);
  const std::string block(64 * 1024, 'x');
  for (int i = 0; i < 32 && big; ++i) big.write(block.data(), static_cast<std::streamsize>(block.size()));
  big.flush();
  EXPECT_FALSE(big.good()) << "the quota stops the write";
  big.close();

  // The directory's project, and its limit, which goes with the directory:
  // nothing of the bound is left on the file system.
  const int fd = ::open((fs::path(xfs) / name).c_str(), O_RDONLY | O_DIRECTORY);
  ASSERT_GE(fd, 0);
  struct fsxattr attributes{};
  ASSERT_EQ(::ioctl(fd, FS_IOC_FSGETXATTR, &attributes), 0);
  ::close(fd);
  const uint32_t project = attributes.fsx_projid;
  EXPECT_NE(project, 0u);
  EXPECT_GT(hard_limit(xfs, project), 0u);

  files.remove_output(xfs, name);
  EXPECT_FALSE(fs::exists(fs::path(xfs) / name));
  EXPECT_EQ(hard_limit(xfs, project), 0u) << "the project's limit is cleared with its directory";
}
