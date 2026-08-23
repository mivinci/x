/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * oneshot_test.cpp — Tests for xpp::sync::oneshot.
 */
#include <thread>

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
  do_send_recv().await();
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
  do_string().await();
}

// ── Multi-threaded tests ──────────────────────────────────────────

TEST(OneshotMtTest, SendFromWorkerRecvOnLoop) {
  auto [tx, rx] = xpp::sync::oneshot::channel<int>();

  std::thread worker([&tx] { tx.send(42); });

  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto recver = [&]() -> xpp::Promise<void> {
    int val = co_await std::move(rx).recv();
    EXPECT_EQ(val, 42);
    co_return;
  };
  recver().await();
  worker.join();
}

TEST(OneshotMtTest, SendOnLoopRecvInWorker) {
  auto [tx, rx] = xpp::sync::oneshot::channel<int>();

  std::thread worker([&rx]() mutable {
    xpp::EventLoop loop;
    xpp::WaitScope scope(loop);

    auto recver = [&]() -> xpp::Promise<void> {
      int val = co_await std::move(rx).recv();
      EXPECT_EQ(val, 99);
      co_return;
    };
    recver().await();
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  tx.send(99);
  worker.join();
}

TEST(OneshotMtTest, ResolveBeforeAwait) {
  auto [tx, rx] = xpp::sync::oneshot::channel<int>();
  tx.send(77);

  std::thread worker([&rx]() mutable {
    xpp::EventLoop loop;
    xpp::WaitScope scope(loop);

    auto recver = [&]() -> xpp::Promise<void> {
      int val = co_await std::move(rx).recv();
      EXPECT_EQ(val, 77);
      co_return;
    };
    recver().await();
  });

  worker.join();
}

TEST(OneshotMtTest, MoveOnlyType) {
  auto [tx, rx] = xpp::sync::oneshot::channel<std::unique_ptr<int>>();

  std::thread worker([&tx] { tx.send(std::make_unique<int>(42)); });

  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto recver = [&]() -> xpp::Promise<void> {
    auto val = co_await std::move(rx).recv();
    EXPECT_NE(val, nullptr);
    EXPECT_EQ(*val, 42);
    co_return;
  };
  recver().await();
  worker.join();
}
