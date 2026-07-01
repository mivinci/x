/*
 * fs_test.cpp - Async filesystem tests
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>
#define XFS_CLOSE(fd) close(fd)
#define XFS_UNLINK(p) unlink(p)
#define XFS_RMDIR(p)  rmdir(p)
#define XFS_TMPDIR    "/tmp"
#else
#include <fcntl.h>
#include <io.h>

#include <sys/stat.h>
#define XFS_CLOSE(fd) _close(fd)
#define XFS_UNLINK(p) _unlink(p)
#define XFS_RMDIR(p)  _rmdir(p)
#define XFS_TMPDIR    "."
#endif

#include <gtest/gtest.h>

#include <x/base/event.h>
#include <x/fs/fs.h>

class FsTest : public ::testing::Test {
protected:
  xEventLoop loop = nullptr;

  void SetUp() override {
    loop = xEventLoopCreate();
    xEventLoopEnter(loop);
  }
  void TearDown() override {
    xEventLoopLeave();
    xEventLoopDestroy(loop);
  }
};

/* ───────────────────── Open / Close ───────────────────── */

TEST_F(FsTest, OpenCloseAsync) {
  xFsReq r = {};
  r.op     = xFsOpOpen;
  r.path   = XFS_TMPDIR "/__xfs_test_open.tmp";
  r.flags  = O_CREAT | O_RDWR | O_TRUNC;
  r.mode   = 0644;
  r.cb     = [](xFsReq *r) {
    EXPECT_EQ(r->result, xErrno_Ok);
    EXPECT_NE(r->out_file, nullptr);
    XFS_CLOSE((int)(intptr_t)r->out_file);
    XFS_UNLINK(XFS_TMPDIR "/__xfs_test_open.tmp");
    xEventLoopStop(xEventLoopCurrent());
  };
  ASSERT_EQ(xFsReqSubmit(&r), xErrno_Pending);
  xEventLoopRun(loop, X_RUN_DEFAULT);
}

TEST_F(FsTest, OpenCloseSync) {
  xFsReq r = {};
  r.op     = xFsOpOpen;
  r.path   = XFS_TMPDIR "/__xfs_test_sync.tmp";
  r.flags  = O_CREAT | O_RDWR | O_TRUNC;
  r.mode   = 0644;
  ASSERT_EQ(xFsReqSubmit(&r), xErrno_Ok);
  ASSERT_NE(r.out_file, nullptr);
  XFS_CLOSE((int)(intptr_t)r.out_file);
  XFS_UNLINK(XFS_TMPDIR "/__xfs_test_sync.tmp");
}

/* ───────────────────── Read / Write ───────────────────── */

TEST_F(FsTest, WriteRead) {
  const char *msg     = "hello async fs!";
  char        buf[64] = {0};

  xFsReq r = {};
  r.op     = xFsOpOpen;
  r.path   = XFS_TMPDIR "/__xfs_test_rw.tmp";
  r.flags  = O_CREAT | O_RDWR | O_TRUNC;
  r.mode   = 0644;
  ASSERT_EQ(xFsReqSubmit(&r), xErrno_Ok);
  ASSERT_NE(r.out_file, nullptr);

  r.op   = xFsOpWrite;
  r.file = r.out_file;
  r.buf  = const_cast<char *>(msg);
  r.len  = strlen(msg);
  r.cb   = [](xFsReq *r) {
    EXPECT_EQ(r->result, xErrno_Ok);
    xEventLoopStop(xEventLoopCurrent());
  };
  ASSERT_EQ(xFsReqSubmit(&r), xErrno_Pending);
  xEventLoopRun(loop, X_RUN_DEFAULT);

  xFile f  = r.file;
  r.op     = xFsOpRead;
  r.buf    = buf;
  r.len    = sizeof(buf);
  r.offset = 0;
  r.cb     = [](xFsReq *r) {
    EXPECT_EQ(r->result, xErrno_Ok);
    EXPECT_EQ(memcmp(r->buf, "hello async fs!", r->retval), 0);
    XFS_CLOSE((int)(intptr_t)r->file);
    XFS_UNLINK(XFS_TMPDIR "/__xfs_test_rw.tmp");
    xEventLoopStop(xEventLoopCurrent());
  };
  ASSERT_EQ(xFsReqSubmit(&r), xErrno_Pending);
  xEventLoopRun(loop, X_RUN_DEFAULT);
}

/* ───────────────────── Stat ───────────────────── */

TEST_F(FsTest, Stat) {
  xFsReq r = {};
  r.op     = xFsOpStat;
#if !defined(_WIN32)
  r.path = "/tmp";
  ASSERT_EQ(xFsReqSubmit(&r), xErrno_Ok);
  EXPECT_TRUE(r.stat.mode & S_IFDIR);
#else
  r.path = ".";
  ASSERT_EQ(xFsReqSubmit(&r), xErrno_Ok);
  EXPECT_TRUE(r.stat.mode & _S_IFDIR);
#endif
}

/* ───────────────────── Mkdir / Unlink ───────────────────── */

TEST_F(FsTest, MkdirAndCleanup) {
  xFsReq r = {};
  r.op     = xFsOpMkdir;
  r.path   = XFS_TMPDIR "/__xfs_test_dir";
  r.mode   = 0755;
  ASSERT_EQ(xFsReqSubmit(&r), xErrno_Ok);
  XFS_RMDIR(XFS_TMPDIR "/__xfs_test_dir");
}

TEST_F(FsTest, UnlinkFile) {
  xFsReq r = {};
  r.op     = xFsOpOpen;
  r.path   = XFS_TMPDIR "/__xfs_test_rm.tmp";
  r.flags  = O_CREAT | O_RDWR;
  r.mode   = 0644;
  ASSERT_EQ(xFsReqSubmit(&r), xErrno_Ok);
  XFS_CLOSE((int)(intptr_t)r.out_file);

  r.op = xFsOpUnlink;
  ASSERT_EQ(xFsReqSubmit(&r), xErrno_Ok);
}

/* ───────────────────── Error paths ───────────────────── */

TEST_F(FsTest, NullReqReturnsInvalidArg) {
  EXPECT_EQ(xFsReqSubmit(nullptr), xErrno_InvalidArg);
}

TEST_F(FsTest, OpenNonexistent) {
  xFsReq r = {};
  r.op     = xFsOpOpen;
  r.path   = XFS_TMPDIR "/__xfs_nonexistent_XXXXXX";
  r.flags  = O_RDONLY;
  ASSERT_NE(xFsReqSubmit(&r), xErrno_Ok);
}
