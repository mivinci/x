/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * file_test.cpp — Tests for xpp::fs::File.
 */
#include <unistd.h>

#include <cstring>
#include <fstream>

#include <gtest/gtest.h>
#include <xpp/fs/file.h>
#include <xpp/promise.h>

/* ── Test fixture: create temp file ────────────────────────────────── */

class FsFileTest : public ::testing::Test {
protected:
  std::string m_path;

  void SetUp() override {
    char tmpl[] = "/tmp/xpp_fs_test_XXXXXX";
    int  fd     = mkstemp(tmpl);
    ASSERT_GE(fd, 0);
    close(fd);
    m_path = tmpl;
  }

  void TearDown() override {
    unlink(m_path.c_str());
  }

  void write_file(const std::string &content) {
    std::ofstream f(m_path, std::ios::binary | std::ios::trunc);
    f << content;
  }
};

/* ── Open / read ───────────────────────────────────────────────────── */

TEST_F(FsFileTest, OpenAndRead) {
  write_file("hello world");
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto file = xpp::fs::File::open(m_path.c_str()).await();
  ASSERT_TRUE(file.is_open());

  char    buf[64] = {};
  ssize_t n       = file.read(buf, sizeof(buf), 0).await();
  EXPECT_EQ(n, 11);
  buf[n] = '\0';
  EXPECT_STREQ(buf, "hello world");
}

TEST_F(FsFileTest, OpenNonExistent) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto file = xpp::fs::File::open("/nonexistent/path/file.txt").await();
  EXPECT_FALSE(file.is_open());
}

TEST_F(FsFileTest, ReadAtEOF) {
  write_file("short");
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto    file    = xpp::fs::File::open(m_path.c_str()).await();
  char    buf[64] = {};
  ssize_t n       = file.read(buf, sizeof(buf), 5).await(); // at EOF
  EXPECT_EQ(n, 0);
}

TEST_F(FsFileTest, ReadWithSpan) {
  write_file("data");
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto    file    = xpp::fs::File::open(m_path.c_str()).await();
  uint8_t buf[16] = {};
  ssize_t n       = file.read(xpp::Span<uint8_t>(buf, sizeof(buf)), 0).await();
  EXPECT_EQ(n, 4);
  EXPECT_EQ(memcmp(buf, "data", 4), 0);
}

/* ── Write ─────────────────────────────────────────────────────────── */

TEST_F(FsFileTest, CreateAndWrite) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto file = xpp::fs::File::create(m_path.c_str()).await();
  ASSERT_TRUE(file.is_open());

  ssize_t n = file.write("hello", 5, 0).await();
  EXPECT_EQ(n, 5);

  // Verify
  file.close().await();
  std::ifstream f(m_path);
  std::string   content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  EXPECT_EQ(content, "hello");
}

TEST_F(FsFileTest, WriteString) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto    file = xpp::fs::File::create(m_path.c_str()).await();
  ssize_t n    = file.write(std::string("hello string"), 0).await();
  EXPECT_EQ(n, 12);
}

TEST_F(FsFileTest, WriteAll) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto file = xpp::fs::File::create(m_path.c_str()).await();
  file.write_all("write all test", 14).await();

  std::ifstream f(m_path);
  std::string   content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  EXPECT_EQ(content, "write all test");
}

/* ── RAII ──────────────────────────────────────────────────────────── */

TEST_F(FsFileTest, RAIIDestructorCloses) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  write_file("content");
  {
    auto file = xpp::fs::File::open(m_path.c_str()).await();
    EXPECT_TRUE(file.is_open());
    // ~File() closes synchronously
  }
  // File should be closed — can reopen
  auto file2 = xpp::fs::File::open(m_path.c_str()).await();
  EXPECT_TRUE(file2.is_open());
}

TEST_F(FsFileTest, CloseThenDestroy) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto file = xpp::fs::File::open(m_path.c_str()).await();
  file.close().await();
  EXPECT_FALSE(file.is_open());
  // ~File() should be no-op (already closed)
}

/* ── Stat ──────────────────────────────────────────────────────────── */

TEST_F(FsFileTest, StatOnOpenFile) {
  write_file("12345");
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto file = xpp::fs::File::open(m_path.c_str()).await();
  auto st   = file.stat().await();
  EXPECT_EQ(st.size, 5);
}

TEST_F(FsFileTest, StatByPath) {
  write_file("1234567890");
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto st = xpp::fs::stat(m_path.c_str()).await();
  EXPECT_EQ(st.size, 10);
}

/* ── Sync ──────────────────────────────────────────────────────────── */

TEST_F(FsFileTest, SyncAll) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto file = xpp::fs::File::create(m_path.c_str()).await();
  file.write("data", 4, 0).await();
  file.sync_all().await();
  SUCCEED();
}

/* ── Convenience ───────────────────────────────────────────────────── */

TEST_F(FsFileTest, ReadAll) {
  write_file("read all content");
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto file = xpp::fs::File::open(m_path.c_str()).await();
  auto data = file.read_all().await();
  ASSERT_EQ(data.len(), 16u);
  EXPECT_EQ(memcmp(data.data(), "read all content", 16), 0);
}

TEST_F(FsFileTest, ReadToString) {
  write_file("text content");
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto file = xpp::fs::File::open(m_path.c_str()).await();
  auto s    = file.read_to_string().await();
  EXPECT_EQ(s, "text content");
}

/* ── Free functions ────────────────────────────────────────────────── */

TEST_F(FsFileTest, FreeReadByPath) {
  write_file("free read");
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto data = xpp::fs::read(m_path.c_str()).await();
  ASSERT_EQ(data.len(), 9u);
  EXPECT_EQ(memcmp(data.data(), "free read", 9), 0);
}

TEST_F(FsFileTest, FreeWriteByPath) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  xpp::fs::write(m_path.c_str(), "free write", 10).await();

  std::ifstream f(m_path);
  std::string   content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  EXPECT_EQ(content, "free write");
}

/* ── Raw fd ────────────────────────────────────────────────────────── */

TEST_F(FsFileTest, FromRawFd) {
  write_file("raw fd test");
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  int fd = ::open(m_path.c_str(), O_RDONLY);
  ASSERT_GE(fd, 0);

  auto file = xpp::fs::File::from_raw_fd(fd);
  EXPECT_TRUE(file.is_open());
  EXPECT_GE(file.raw_fd(), 0);

  char    buf[64] = {};
  ssize_t n       = file.read(buf, sizeof(buf), 0).await();
  EXPECT_EQ(n, 11);
  buf[n] = '\0';
  EXPECT_STREQ(buf, "raw fd test");
}

/* ── Directory operations ──────────────────────────────────────────── */

TEST_F(FsFileTest, CreateAndRemoveDir) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  std::string dir = m_path + "_dir";
  xpp::fs::create_dir(dir.c_str()).await();

  struct stat st;
  EXPECT_EQ(::stat(dir.c_str(), &st), 0);
  EXPECT_TRUE(S_ISDIR(st.st_mode));

  xpp::fs::remove_dir(dir.c_str()).await();
  EXPECT_NE(::stat(dir.c_str(), &st), 0);
}

TEST_F(FsFileTest, RemoveFile) {
  write_file("temp file");
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  xpp::fs::remove_file(m_path.c_str()).await();

  struct stat st;
  EXPECT_NE(::stat(m_path.c_str(), &st), 0);
}

TEST_F(FsFileTest, RenameFile) {
  write_file("original content");
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  std::string new_path = m_path + "_renamed";
  xpp::fs::rename(m_path.c_str(), new_path.c_str()).await();

  // Old path should not exist
  struct stat st;
  EXPECT_NE(::stat(m_path.c_str(), &st), 0);
  // New path should exist with correct content
  EXPECT_EQ(::stat(new_path.c_str(), &st), 0);
  auto data = xpp::fs::read(new_path.c_str()).await();
  ASSERT_EQ(data.len(), 16u);
  EXPECT_EQ(memcmp(data.data(), "original content", 16), 0);

  unlink(new_path.c_str());
}

/* ── exists ────────────────────────────────────────────────────────── */

TEST_F(FsFileTest, ExistsOnPresentFile) {
  write_file("exists test");
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  EXPECT_TRUE(xpp::fs::exists(m_path.c_str()).await());
}

TEST_F(FsFileTest, ExistsOnMissingFile) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  EXPECT_FALSE(xpp::fs::exists("/nonexistent/path/file.txt").await());
}

TEST_F(FsFileTest, ExistsOnDirectory) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  std::string dir = m_path + "_exists_dir";
  xpp::fs::create_dir(dir.c_str()).await();
  EXPECT_TRUE(xpp::fs::exists(dir.c_str()).await());
  xpp::fs::remove_dir(dir.c_str()).await();
  EXPECT_FALSE(xpp::fs::exists(dir.c_str()).await());
}
