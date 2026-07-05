/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * broadcast_mt_test.cpp — Multi-threaded tests for xpp::sync::broadcast.
 *
 * Requires -DXPP_MT.
 */
#include <gtest/gtest.h>
#include <xpp/promise.h>
#include <xpp/sync/broadcast.h>

#include <thread>

#if !XPP_MT
// Skip tests when XPP_MT is not defined.
#else

using namespace xpp::sync::broadcast;

TEST(BroadcastMtTest, WorkerSendLoopRecv) {
  auto [tx, rx] = channel<int>(16);

  std::thread worker([&tx] {
    for (int i = 0; i < 3; ++i) {
      EXPECT_TRUE(tx.try_send(i).is_ok());
    }
  });

  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto recver = [&]() -> xpp::Promise<void> {
    for (int i = 0; i < 3; ++i) {
      auto v = co_await rx.recv();
      EXPECT_TRUE(v.is_ok());
    }
    co_return;
  };
  recver().wait();
  worker.join();
}

TEST(BroadcastMtTest, MultipleWorkerSenders) {
  auto [tx, rx] = channel<int>(32);

  std::thread t1([tx]() mutable {
    for (int i = 0; i < 5; ++i)
      EXPECT_TRUE(tx.try_send(i).is_ok());
  });
  std::thread t2([tx]() mutable {
    for (int i = 100; i < 105; ++i)
      EXPECT_TRUE(tx.try_send(i).is_ok());
  });

  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto recver = [&]() -> xpp::Promise<void> {
    for (int i = 0; i < 10; ++i) {
      auto v = co_await rx.recv();
      EXPECT_TRUE(v.is_ok());
    }
    co_return;
  };
  recver().wait();
  t1.join();
  t2.join();
}

#endif // XPP_MT
