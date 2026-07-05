/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * watch_mt_test.cpp — Multi-threaded tests for xpp::sync::watch.
 *
 * Requires -DXPP_MT.
 */
#include <gtest/gtest.h>
#include <xpp/promise.h>
#include <xpp/sync/watch.h>

#include <thread>
#include <chrono>

#if !XPP_MT
#else

using namespace xpp::sync::watch;

TEST(WatchMtTest, WorkerSendLoopChanged) {
  auto [tx, rx] = channel(0);

  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  std::thread worker([&tx] {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    tx.send(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    tx.send(2);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    tx.send(3);
  });

  auto recver = [&]() -> xpp::Promise<void> {
    co_await rx.changed(); // 1
    co_await rx.changed(); // 2
    co_await rx.changed(); // 3
    co_return;
  };
  recver().wait();
  worker.join();
}

#endif
