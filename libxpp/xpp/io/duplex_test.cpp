/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * duplex_test.cpp — Tests for xpp::io::duplex.
 */
#include <gtest/gtest.h>
#include <xpp/io/duplex.h>
#include <xpp/io/util.h>
#include <xpp/promise.h>
#include <xpp/promise_combinators.h>

/* ── Write then read ───────────────────────────────────────────────── */

xpp::Promise<void> do_write_then_read() {
  auto [reader, writer] = xpp::io::duplex(256);

  co_await writer.write("hello", 5);
  writer.close();

  char    buf[16] = {};
  ssize_t n       = co_await reader.read(buf, sizeof(buf));
  EXPECT_EQ(n, 5);
  EXPECT_EQ(std::string(buf, 5), "hello");

  ssize_t n2 = co_await reader.read(buf, sizeof(buf));
  EXPECT_EQ(n2, 0);
  co_return;
}

TEST(DuplexTest, WriteThenRead) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_write_then_read().wait();
}

/* ── Concurrent read + write ───────────────────────────────────────── */

xpp::Promise<void> do_concurrent() {
  auto [reader, writer] = xpp::io::duplex(256);

  auto wp      = writer.write("hello", 5);
  char buf[16] = {};
  auto rp      = reader.read(buf, sizeof(buf));

  auto [w, r] = co_await xpp::all(std::move(wp), std::move(rp));
  EXPECT_EQ(w, 5);
  EXPECT_EQ(r, 5);
  EXPECT_EQ(std::string(buf, 5), "hello");
  co_return;
}

TEST(DuplexTest, ConcurrentReadWrite) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_concurrent().wait();
}

/* ── Buffer full blocks writer, reader unblocks ────────────────────── */

xpp::Promise<void> do_buffer_full() {
  auto [reader, writer] = xpp::io::duplex(64);

  // Fill buffer completely
  std::string chunk(64, 'A');
  co_await writer.write(chunk.data(), 64);

  // Now write 1 more byte — should block until reader drains
  auto wp = writer.write("B", 1);

  // Read 1 byte to unblock writer
  char c;
  auto rp = reader.read(&c, 1);

  auto [w, r] = co_await xpp::all(std::move(wp), std::move(rp));
  EXPECT_EQ(w, 1);
  EXPECT_EQ(r, 1);
  EXPECT_EQ(c, 'A');
  co_return;
}

TEST(DuplexTest, BufferFull) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_buffer_full().wait();
}

/* ── io::read_all round-trip ────────────────────────────────────────── */

xpp::Promise<void> do_read_all_roundtrip() {
  auto [reader, writer] = xpp::io::duplex(4096);

  // Write, then close
  co_await writer.write("hello world", 11);
  writer.close();

  // Read all
  auto data = co_await xpp::io::read_all(reader);
  EXPECT_EQ(data.size(), 11u);
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(data.data()), 11), "hello world");
  co_return;
}

TEST(DuplexTest, ReadAllRoundtrip) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_read_all_roundtrip().wait();
}
