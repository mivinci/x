/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * watch_test.cpp — Tests for xpp::sync::watch.
 */
#include <gtest/gtest.h>
#include <xpp/promise.h>
#include <xpp/sync/watch.h>

#include <thread>
#include <chrono>

using namespace xpp::sync::watch;

// ── W-2: send returns old value ─────────────────────────────────────

TEST(WatchTest, SendReturnsOld) {
  auto [tx, rx] = channel(10);
  auto old      = tx.send(20);
  EXPECT_TRUE(old.is_ok());
  EXPECT_EQ(old.unwrap(), 10);
  (void)rx;
}

// ── W-8: send fails with no receivers ───────────────────────────────

TEST(WatchTest, SendFailsNoReceivers) {
  auto [tx, rx] = channel(0);
  { auto d = std::move(rx); }
  auto r = tx.send(1);
  EXPECT_TRUE(r.is_err());
  EXPECT_EQ(r.unwrap_err().kind, SendError<int>::NoReceiver);
}

// ── W-9: multi-producer ─────────────────────────────────────────────

TEST(WatchTest, MultiProducer) {
  auto [tx, rx] = channel(0);
  auto tx2      = tx;
  EXPECT_EQ(tx.send(10).unwrap(), 0);
  EXPECT_EQ(tx2.send(20).unwrap(), 10);
  (void)rx;
}

// ── W-4: borrow peek ────────────────────────────────────────────────

TEST(WatchTest, BorrowRead) {
  auto [tx, rx] = channel(42);
  auto r        = tx.borrow();
  EXPECT_EQ(*r, 42);
}

// ── W-3: changed() ready immediately ─────────────────────────────────

xpp::Promise<void> do_changed_ready() {
  auto [tx, rx] = channel(1);
  tx.send(2);
  co_await rx.changed();
  co_return;
}

TEST(WatchTest, ChangedReady) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_changed_ready().await();
}

// ── W-1: initial seen, changed() waits ───────────────────────────────

xpp::Promise<void> do_initial_seen() {
  auto [tx, rx] = channel(42);
  auto p        = rx.changed(); // suspends — 42 already seen
  tx.send(99);
  co_await std::move(p); // resumes after send
  co_return;
}

TEST(WatchTest, InitialSeen) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_initial_seen().await();
}

// ── W-5: borrow_and_update marks seen ───────────────────────────────

TEST(WatchTest, BorrowAndUpdate) {
  auto [tx, rx] = channel(0);
  tx.send(1);
  {
    auto r = rx.borrow_and_update();
    EXPECT_EQ(*r, 1);
  }
  EXPECT_FALSE(rx.has_changed().unwrap()); // seen → nothing new
}

// ── W-6: has_changed ────────────────────────────────────────────────

TEST(WatchTest, HasChanged) {
  auto [tx, rx] = channel(0);
  EXPECT_FALSE(rx.has_changed().unwrap());
  tx.send(1);
  EXPECT_TRUE(rx.has_changed().unwrap());
}

// ── W-7: subscribe ──────────────────────────────────────────────────

xpp::Promise<void> do_subscribe() {
  auto [tx, rx1] = channel(0);
  tx.send(1);
  auto rx2 = tx.subscribe();
  auto p   = rx2.changed();
  tx.send(2);
  co_await std::move(p);
  co_return;
}

TEST(WatchTest, Subscribe) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_subscribe().await();
}

// ── Multi-threaded tests (require -DXPP_MT) ─────────────────────────

#if XPP_MT

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
  recver().await();
  worker.join();
}

#endif // XPP_MT
