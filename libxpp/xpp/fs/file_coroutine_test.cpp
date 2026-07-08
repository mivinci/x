/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * file_coroutine_test.cpp — C++20 coroutine tests for xpp::fs::File.
 */
#include <unistd.h>

#include <cstring>
#include <fstream>

#include <gtest/gtest.h>
#include <xpp/fs/file.h>
#include <xpp/promise.h>

/* ── Test fixture ──────────────────────────────────────────────────── */

class FsCoroutineTest : public ::testing::Test {
protected:
  std::string m_path;

  void SetUp() override {
    char tmpl[] = "/tmp/xpp_fs_coro_XXXXXX";
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

/* ── Coroutine: open + read ────────────────────────────────────────── */

static xpp::Promise<std::string> coro_read_file(const char *path) {
  auto file = co_await xpp::fs::File::open(path);
  co_return co_await file.read_to_string();
}

TEST_F(FsCoroutineTest, OpenAndRead) {
  write_file("coroutine content");
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto content = coro_read_file(m_path.c_str()).await();
  EXPECT_EQ(content, "coroutine content");
}

/* ── Coroutine: create + write + close ─────────────────────────────── */

static xpp::Promise<void> coro_write_file(const char *path, const std::string &data) {
  auto file = co_await xpp::fs::File::create(path);
  co_await file.write(data.data(), data.size(), 0);
  co_await file.close();
}

TEST_F(FsCoroutineTest, CreateAndWrite) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  coro_write_file(m_path.c_str(), "written via coroutine").await();

  std::ifstream f(m_path);
  std::string   content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  EXPECT_EQ(content, "written via coroutine");
}

/* ── Coroutine: multiple awaits (read → transform → write) ─────────── */

static xpp::Promise<void> coro_copy_uppercase(const char *src, const char *dst) {
  auto file = co_await xpp::fs::File::open(src);
  auto data = co_await file.read_all();

  std::string upper;
  upper.reserve(data.size());
  for (auto c : data) {
    upper.push_back(static_cast<char>(toupper(c)));
  }

  auto out = co_await xpp::fs::File::create(dst);
  co_await out.write(upper, 0);
}

TEST_F(FsCoroutineTest, ReadTransformWrite) {
  write_file("hello world");
  std::string out_path = m_path + "_upper";

  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  coro_copy_uppercase(m_path.c_str(), out_path.c_str()).await();

  auto result = xpp::fs::read(out_path.c_str()).await();
  ASSERT_EQ(result.size(), 11u);
  EXPECT_EQ(memcmp(result.data(), "HELLO WORLD", 11), 0);

  unlink(out_path.c_str());
}

/* ── Coroutine: stat + exists ──────────────────────────────────────── */

static xpp::Promise<bool> coro_check_and_stat(const char *path, xpp::fs::Stat *out_stat) {
  bool ex = co_await xpp::fs::exists(path);
  if (!ex) co_return false;
  *out_stat = co_await xpp::fs::stat(path);
  co_return true;
}

TEST_F(FsCoroutineTest, StatViaCoroutine) {
  write_file("stat me");
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  xpp::fs::Stat st{};
  bool          found = coro_check_and_stat(m_path.c_str(), &st).await();
  EXPECT_TRUE(found);
  EXPECT_EQ(st.size, 7);
}

/* ── Coroutine: directory operations ───────────────────────────────── */

static xpp::Promise<void> coro_mkdir_rmdir(const char *path) {
  co_await xpp::fs::create_dir(path);
  bool ex = co_await xpp::fs::exists(path);
  if (!ex) co_return; // shouldn't happen, but don't leak
  co_await xpp::fs::remove_dir(path);
}

TEST_F(FsCoroutineTest, CreateAndRemoveDir) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  std::string dir = m_path + "_coro_dir";
  coro_mkdir_rmdir(dir.c_str()).await();

  struct stat st;
  EXPECT_NE(::stat(dir.c_str(), &st), 0);
}
