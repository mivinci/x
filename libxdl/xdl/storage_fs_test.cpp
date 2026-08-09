/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * storage_fs_test.cpp - Async filesystem storage tests with event loop
 */

#include <gtest/gtest.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fcntl.h>

#include <x/base/event.h>
#include <x/base/test_helper.h>
#include <x/fs/fs.h>

extern "C" {
#include <xdl/storage.h>
#include <xdl/storage_fs.h>
}

#define TMP_FILE "/tmp/xdl_storage_test.bin"

static std::atomic<bool>   g_done{false};
static std::atomic<xErrno> g_err{xErrno_Unknown};
static xdl_storage_file_t  *g_file;

static void cb_open(void *arg, xErrno err, xdl_storage_file_t *f) {
  g_file = f; g_err = err; g_done = true;
  xEventLoopStop(xEventLoopCurrent()); (void)arg;
}

static void cb_write(void *arg, xErrno err, ssize_t written) {
  (void)written; g_err = err; g_done = true;
  xEventLoopStop(xEventLoopCurrent()); (void)arg;
}

static uint8_t  g_read_buf[4096];
static ssize_t  g_read_len;

static void cb_read(void *arg, const uint8_t *data, ssize_t len) {
  if (data && len > 0 && len <= sizeof(g_read_buf)) memcpy(g_read_buf, data, len);
  g_read_len = data ? len : -1;
  g_done = true; xEventLoopStop(xEventLoopCurrent()); (void)arg;
}

static void cb_flush(void *arg, xErrno err) {
  g_err = err; g_done = true;
  xEventLoopStop(xEventLoopCurrent()); (void)arg;
}

static void cb_close(void *arg) {
  g_done = true;
  xEventLoopStop(xEventLoopCurrent()); (void)arg;
}

static xdl_storage_file_t *do_open(xEventLoop loop) {
  g_done = false; g_file = nullptr;
  EXPECT_EQ(xdl_storage_fs_open(TMP_FILE, cb_open, nullptr), 0);
  xEventLoopRun(loop, X_RUN_DEFAULT);
  EXPECT_TRUE(g_done.load());
  EXPECT_EQ(g_err.load(), xErrno_Ok);
  EXPECT_NE(g_file, nullptr);
  return g_file;
}

/* ── Tests ──────────────────────────────────────── */

TEST(StorageFs, OpenAsync) {
  xEventLoop loop = xEventLoopCreate(); xEventLoopEnter(loop);
  xdl_storage_file_t *f = do_open(loop);

  g_done = false;
  EXPECT_EQ(xdl_storage_flush(f, cb_flush, nullptr), 0);
  run_for(loop, 300);
  EXPECT_TRUE(g_done.load());
  EXPECT_EQ(g_err.load(), xErrno_Ok);

  g_done = false;
  xdl_storage_close(f, cb_close, nullptr);
  run_for(loop, 100);
  EXPECT_TRUE(g_done.load());

  xEventLoopLeave(); xEventLoopDestroy(loop);
  remove(TMP_FILE);
}

TEST(StorageFs, WriteAsync) {
  xEventLoop loop = xEventLoopCreate(); xEventLoopEnter(loop);
  xdl_storage_file_t *f = do_open(loop);

  g_done = false;
  EXPECT_EQ(xdl_storage_write(f, 0, reinterpret_cast<const uint8_t *>("hello"), 5,
                              cb_write, nullptr), 0);
  xEventLoopRun(loop, X_RUN_DEFAULT);
  EXPECT_TRUE(g_done.load());
  EXPECT_EQ(g_err.load(), xErrno_Ok);

  /* Verify .part contents */
  xFsReq sr = {}; sr.op = xFsOpOpen; sr.path = TMP_FILE ".part";
  sr.flags = O_RDONLY; xFsReqSubmit(&sr);
  ASSERT_EQ(sr.result, xErrno_Ok);
  uint8_t buf[256] = {}; xFsReq rr = {}; rr.op = xFsOpRead; rr.file = sr.out_file;
  rr.buf = buf; rr.len = 5; xFsReqSubmit(&rr);
  EXPECT_EQ(rr.retval, 5); EXPECT_EQ(memcmp(buf, "hello", 5), 0);
  xFsReq cr = {}; cr.op = xFsOpClose; cr.file = sr.out_file; xFsReqSubmit(&cr);

  g_done = false;
  EXPECT_EQ(xdl_storage_flush(f, cb_flush, nullptr), 0);
  run_for(loop, 300);
  EXPECT_TRUE(g_done.load());

  g_done = false;
  xdl_storage_close(f, cb_close, nullptr);
  run_for(loop, 100);
  EXPECT_TRUE(g_done.load());

  xEventLoopLeave(); xEventLoopDestroy(loop);
  remove(TMP_FILE);
}

TEST(StorageFs, WriteAtOffset) {
  xEventLoop loop = xEventLoopCreate(); xEventLoopEnter(loop);
  xdl_storage_file_t *f = do_open(loop);

  g_done = false;
  xdl_storage_write(f, 0, reinterpret_cast<const uint8_t *>("hel"), 3, cb_write, nullptr);
  xEventLoopRun(loop, X_RUN_DEFAULT);
  g_done = false;
  xdl_storage_write(f, 3, reinterpret_cast<const uint8_t *>("lo!"), 3, cb_write, nullptr);
  xEventLoopRun(loop, X_RUN_DEFAULT);
  EXPECT_TRUE(g_done.load());

  xFsReq sr = {}; sr.op = xFsOpOpen; sr.path = TMP_FILE ".part";
  sr.flags = O_RDONLY; xFsReqSubmit(&sr);
  uint8_t buf[256] = {}; xFsReq rr = {}; rr.op = xFsOpRead; rr.file = sr.out_file;
  rr.buf = buf; rr.len = 6; xFsReqSubmit(&rr);
  EXPECT_EQ(rr.retval, 6); EXPECT_EQ(memcmp(buf, "hello!", 6), 0);
  xFsReq cr = {}; cr.op = xFsOpClose; cr.file = sr.out_file; xFsReqSubmit(&cr);

  g_done = false;
  xdl_storage_flush(f, cb_flush, nullptr);
  run_for(loop, 300);
  g_done = false;
  xdl_storage_close(f, cb_close, nullptr);
  run_for(loop, 100);

  xEventLoopLeave(); xEventLoopDestroy(loop);
  remove(TMP_FILE);
}

TEST(StorageFs, ReadAsync) {
  xEventLoop loop = xEventLoopCreate(); xEventLoopEnter(loop);
  xdl_storage_file_t *f = do_open(loop);

  g_done = false;
  xdl_storage_write(f, 0, reinterpret_cast<const uint8_t *>("hello"), 5, cb_write, nullptr);
  xEventLoopRun(loop, X_RUN_DEFAULT);

  g_done = false; g_read_len = 0; memset(g_read_buf, 0, 5);
  uint8_t rbuf[256];
  EXPECT_EQ(xdl_storage_read(f, 0, rbuf, 5, cb_read, nullptr), 0);
  xEventLoopRun(loop, X_RUN_DEFAULT);
  EXPECT_EQ(g_read_len, 5);
  EXPECT_EQ(memcmp(g_read_buf, "hello", 5), 0);

  g_done = false;
  xdl_storage_flush(f, cb_flush, nullptr);
  run_for(loop, 300);
  g_done = false;
  xdl_storage_close(f, cb_close, nullptr);
  run_for(loop, 100);

  xEventLoopLeave(); xEventLoopDestroy(loop);
  remove(TMP_FILE);
}

TEST(StorageFs, CloseWithoutFlushDeletes) {
  xEventLoop loop = xEventLoopCreate(); xEventLoopEnter(loop);
  xdl_storage_file_t *f = do_open(loop);

  g_done = false;
  xdl_storage_write(f, 0, reinterpret_cast<const uint8_t *>("x"), 1, cb_write, nullptr);
  xEventLoopRun(loop, X_RUN_DEFAULT);

  g_done = false;
  xdl_storage_close(f, cb_close, nullptr);
  run_for(loop, 300);
  EXPECT_TRUE(g_done.load());

  xEventLoopLeave(); xEventLoopDestroy(loop);

  char part[1100]; snprintf(part, sizeof(part), "%s.part", TMP_FILE);
  EXPECT_EQ(fopen(part, "rb"), nullptr);
}

TEST(StorageFs, CloseNullSafe) {
  xdl_storage_close(nullptr, cb_close, nullptr);
}
