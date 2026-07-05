/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * oneshot_test.cpp — Tests for xpp::sync::oneshot.
 */
#include <gtest/gtest.h>
#include <xpp/promise.h>
#include <xpp/sync/oneshot.h>

xpp::Promise<void> do_send_recv() {
  auto [tx, rx] = xpp::sync::oneshot::channel<int>();
  tx.send(42);
  int val = co_await std::move(rx).recv();
  EXPECT_EQ(val, 42);
  co_return;
}

TEST(OneshotTest, SendRecv) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_send_recv().wait();
}

xpp::Promise<void> do_string() {
  auto [tx, rx] = xpp::sync::oneshot::channel<std::string>();
  tx.send(std::string("hello"));
  auto val = co_await std::move(rx).recv();
  EXPECT_EQ(val, "hello");
  co_return;
}

TEST(OneshotTest, String) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_string().wait();
}
