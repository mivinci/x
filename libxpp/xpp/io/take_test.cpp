/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * take_coroutine_test.cpp — Tests for xpp::io::Take.
 */
#include <gtest/gtest.h>
#include <xpp/io/take.h>
#include <xpp/io/util.h>
#include <xpp/net/tcp.h>
#include <xpp/net/test_helpers.h>
#include <xpp/promise.h>
#include <xpp/promise_combinators.h>

using xpp::io::Take;
using xpp::net::TcpListener;
using xpp::net::TcpStream;

xpp::Promise<void> do_take_within_limit() {
  uint16_t port = get_free_port();
  auto     lr = co_await xpp::net::TcpListener::bind(("127.0.0.1:" + std::to_string(port)).c_str());
  TcpListener listener = std::move(lr).unwrap();

  auto server = listener.accept();
  auto client = xpp::net::TcpStream::connect(("127.0.0.1:" + std::to_string(port)).c_str());

  auto [sp, cr] = co_await xpp::all(std::move(server), std::move(client));
  TcpStream sc  = std::move(sp.first);
  TcpStream cc  = std::move(cr).unwrap();

  co_await sc.write("hello world", 11);
  sc.close();

  Take<TcpStream> limit(std::move(cc), 5);
  auto            data = co_await xpp::io::read_all(limit);

  EXPECT_EQ(data.size(), 5u);
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(data.data()), 5), "hello");
  co_return;
}

TEST(TakeTest, WithinLimit) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_take_within_limit().wait();
}

xpp::Promise<void> do_take_read_zero() {
  uint16_t port = get_free_port();
  auto     lr = co_await xpp::net::TcpListener::bind(("127.0.0.1:" + std::to_string(port)).c_str());
  TcpListener listener = std::move(lr).unwrap();

  auto server = listener.accept();
  auto client = xpp::net::TcpStream::connect(("127.0.0.1:" + std::to_string(port)).c_str());

  auto [sp, cr] = co_await xpp::all(std::move(server), std::move(client));
  TcpStream sc  = std::move(sp.first);
  TcpStream cc  = std::move(cr).unwrap();

  co_await sc.write("hello", 5);
  sc.close();

  Take<TcpStream> limit(std::move(cc), 5);
  char            buf[10];

  ssize_t n1 = co_await limit.read(buf, 5);
  EXPECT_EQ(n1, 5);

  ssize_t n2 = co_await limit.read(buf, 10);
  EXPECT_EQ(n2, 0);
  EXPECT_EQ(limit.remaining(), 0u);
  co_return;
}

TEST(TakeTest, ReadZeroAfterLimit) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_take_read_zero().wait();
}
