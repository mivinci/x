/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * sink_coroutine_test.cpp — Tests for xpp::io::Sink.
 */
#include <gtest/gtest.h>
#include <xpp/io/sink.h>
#include <xpp/io/utils.h>
#include <xpp/net/tcp.h>
#include <xpp/net/test_helpers.h>
#include <xpp/promise.h>
#include <xpp/promise_combinators.h>

using xpp::net::TcpListener;
using xpp::net::TcpStream;

TEST(SinkTest, WriteReturnsLen) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  xpp::io::Sink  s;
  ssize_t        n = s.write("hello", 5).await();
  EXPECT_EQ(n, 5);
}

xpp::Promise<void> do_copy_to_sink() {
  uint16_t port = get_free_port();
  auto     lr = co_await xpp::net::TcpListener::bind(("127.0.0.1:" + std::to_string(port)).c_str());
  TcpListener listener = std::move(lr).unwrap();

  auto server = listener.accept();
  auto client = xpp::net::TcpStream::connect(("127.0.0.1:" + std::to_string(port)).c_str());

  auto [sp, cr] = co_await xpp::all(std::move(server), std::move(client));
  TcpStream cc  = std::move(cr).unwrap();

  co_await cc.write("hello", 5);
  cc.close();

  TcpStream sc = std::move(sp.first);
  auto      s  = xpp::io::sink();
  co_await xpp::io::copy(sc, s);
  co_return;
}

TEST(SinkTest, CopyToSink) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_copy_to_sink().await();
}
