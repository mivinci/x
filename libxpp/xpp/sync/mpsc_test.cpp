/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * mpsc_test.cpp — Tests for xpp::sync::mpsc.
 */
#include <gtest/gtest.h>
#include <xpp/promise.h>
#include <xpp/sync/mpsc.h>

xpp::Promise<void> do_send_recv() {
  auto [tx, rx] = xpp::sync::mpsc::channel<int>(4);
  co_await tx.send(1);
  co_await tx.send(2);
  tx.close();

  auto v1 = co_await rx.recv();
  auto v2 = co_await rx.recv();
  auto v3 = co_await rx.recv();

  EXPECT_TRUE(v1.is_some());
  EXPECT_EQ(v1.unwrap(), 1);
  EXPECT_TRUE(v2.is_some());
  EXPECT_EQ(v2.unwrap(), 2);
  EXPECT_TRUE(v3.is_none());
  co_return;
}

TEST(MpscTest, SendRecv) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_send_recv().wait();
}

xpp::Promise<void> do_multi_producer() {
  auto [tx, rx] = xpp::sync::mpsc::channel<int>(8);
  auto tx2      = tx;

  co_await tx.send(10);
  co_await tx2.send(20);
  tx.close();

  EXPECT_EQ((co_await rx.recv()).unwrap(), 10);
  EXPECT_EQ((co_await rx.recv()).unwrap(), 20);
  co_return;
}

TEST(MpscTest, MultiProducer) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_multi_producer().wait();
}

xpp::Promise<void> do_buffer_full() {
  auto [tx, rx] = xpp::sync::mpsc::channel<int>(2);

  co_await tx.send(1);
  co_await tx.send(2);
  auto send_p = tx.send(3);
  EXPECT_EQ((co_await rx.recv()).unwrap(), 1);
  co_await std::move(send_p);
  tx.close();
  co_return;
}

TEST(MpscTest, BufferFull) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_buffer_full().wait();
}
