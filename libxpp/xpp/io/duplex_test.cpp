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

/* ── Write a→b, read b ─────────────────────────────────────────────── */

xpp::Promise<void> do_write_then_read() {
  auto [a, b] = xpp::io::duplex(256);

  co_await a.write("hello", 5);
  a.close(); // signal EOF on b

  char    buf[16] = {};
  ssize_t n       = co_await b.read(buf, sizeof(buf));
  EXPECT_EQ(n, 5);
  EXPECT_EQ(std::string(buf, 5), "hello");

  ssize_t n2 = co_await b.read(buf, sizeof(buf));
  EXPECT_EQ(n2, 0);
  co_return;
}

TEST(DuplexTest, WriteThenRead) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_write_then_read().wait();
}

/* ── Bidirectional ─────────────────────────────────────────────────── */

xpp::Promise<void> do_bidirectional() {
  auto [a, b] = xpp::io::duplex(256);

  // a→b
  auto w1      = a.write("ping", 4);
  char buf1[8] = {};
  auto r1      = b.read(buf1, sizeof(buf1));

  // b→a
  auto w2      = b.write("pong", 4);
  char buf2[8] = {};
  auto r2      = a.read(buf2, sizeof(buf2));

  auto [v1, v2, v3, v4] =
    co_await xpp::all(std::move(w1), std::move(r1), std::move(w2), std::move(r2));
  EXPECT_EQ(v1, 4);
  EXPECT_EQ(v2, 4);
  EXPECT_EQ(std::string(buf1, 4), "ping");

  EXPECT_EQ(v3, 4);
  EXPECT_EQ(v4, 4);
  EXPECT_EQ(std::string(buf2, 4), "pong");
  co_return;
}

TEST(DuplexTest, Bidirectional) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_bidirectional().wait();
}

/* ── Buffer full → writer blocks, reader unblocks ──────────────────── */

xpp::Promise<void> do_buffer_full() {
  auto [a, b] = xpp::io::duplex(64);

  // Fill b's read buffer completely by writing from a
  std::string chunk(64, 'A');
  co_await a.write(chunk.data(), 64);

  // Now write 1 more byte — should block until b reads
  auto wp = a.write("B", 1);

  // b reads 1 byte → unblocks a's write
  char c;
  auto rp = b.read(&c, 1);

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

/* ── read_all round-trip ───────────────────────────────────────────── */

xpp::Promise<void> do_read_all_roundtrip() {
  auto [a, b] = xpp::io::duplex(4096);

  co_await a.write("hello world", 11);
  a.close();

  auto data = co_await xpp::io::read_all(b);
  EXPECT_EQ(data.size(), 11u);
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(data.data()), 11), "hello world");
  co_return;
}

TEST(DuplexTest, ReadAllRoundtrip) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_read_all_roundtrip().wait();
}
