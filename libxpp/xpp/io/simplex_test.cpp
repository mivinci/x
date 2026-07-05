/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * simplex_test.cpp — Tests for xpp::io::simplex.
 */
#include <gtest/gtest.h>
#include <xpp/io/simplex.h>
#include <xpp/io/util.h>
#include <xpp/promise.h>
#include <xpp/promise_combinators.h>

xpp::Promise<void> do_write_then_read() {
  auto [reader, writer] = xpp::io::simplex(256);

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

TEST(SimplexTest, WriteThenRead) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_write_then_read().wait();
}

xpp::Promise<void> do_buffer_full() {
  auto [reader, writer] = xpp::io::simplex(64);

  co_await writer.write(std::string(64, 'A').data(), 64);

  auto wp = writer.write("B", 1);
  char c;
  auto rp = reader.read(&c, 1);

  auto [w, r] = co_await xpp::all(std::move(wp), std::move(rp));
  EXPECT_EQ(w, 1);
  EXPECT_EQ(r, 1);
  co_return;
}

TEST(SimplexTest, BufferFull) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_buffer_full().wait();
}

xpp::Promise<void> do_read_all_roundtrip() {
  auto [reader, writer] = xpp::io::simplex(4096);

  co_await writer.write("hello world", 11);
  writer.close();

  auto data = co_await xpp::io::read_all(reader);
  EXPECT_EQ(data.size(), 11u);
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(data.data()), 11), "hello world");
  co_return;
}

TEST(SimplexTest, ReadAllRoundtrip) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_read_all_roundtrip().wait();
}
