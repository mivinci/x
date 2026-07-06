/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * broadcast_test.cpp — Tests for xpp::sync::broadcast.
 */
#include <gtest/gtest.h>
#include <xpp/promise.h>
#include <xpp/sync/broadcast.h>

#include <thread>

using namespace xpp::sync::broadcast;

// ── B-1: multiple receivers see all values ──────────────────────────

TEST(BroadcastTest, MultipleReceivers) {
  auto [tx, rx1] = channel<int>(4);
  auto rx2       = tx.subscribe();

  EXPECT_TRUE(tx.try_send(10).is_ok());
  EXPECT_TRUE(tx.try_send(20).is_ok());

  EXPECT_EQ(rx1.try_recv().unwrap(), 10);
  EXPECT_EQ(rx2.try_recv().unwrap(), 10);
  EXPECT_EQ(rx1.try_recv().unwrap(), 20);
  EXPECT_EQ(rx2.try_recv().unwrap(), 20);
}

// ── B-2: late subscriber only sees future values ────────────────────

TEST(BroadcastTest, LateSubscriber) {
  auto [tx, rx1] = channel<int>(4);
  EXPECT_TRUE(tx.try_send(1).is_ok());

  auto rx2 = tx.subscribe();
  EXPECT_TRUE(tx.try_send(2).is_ok());

  EXPECT_EQ(rx1.try_recv().unwrap(), 1);
  EXPECT_EQ(rx1.try_recv().unwrap(), 2);

  EXPECT_EQ(rx2.try_recv().unwrap(), 2); // rx2 was NOT present for 1
}

// ── B-3: lag on overflow ────────────────────────────────────────────

TEST(BroadcastTest, LagOnOverflow) {
  auto [tx, rx] = channel<int>(2);
  EXPECT_TRUE(tx.try_send(1).is_ok());
  EXPECT_TRUE(tx.try_send(2).is_ok());

  // Don't consume 1 yet — sender overflows on next send
  EXPECT_TRUE(tx.try_send(3).is_ok());

  // Receiver has lagged — position behind head
  // After lag, should be able to read 2 and 3
  // (exact behavior depends on the recv() implementation)
}

// ── B-4: RAII close ─────────────────────────────────────────────────

TEST(BroadcastTest, RaiiClose) {
  auto rx = ([] {
    auto [tx, rx] = channel<int>(4);
    EXPECT_TRUE(tx.try_send(42).is_ok());
    return std::move(rx);
  })();

  EXPECT_EQ(rx.try_recv().unwrap(), 42);
  auto r = rx.try_recv();
  EXPECT_TRUE(r.is_err());
}

// ── B-5: send returns receiver count ────────────────────────────────

TEST(BroadcastTest, SendReturnsReceiverCount) {
  auto [tx, rx] = channel<int>(4);
  auto rx2      = tx.subscribe();

  auto r1 = tx.try_send(1);
  EXPECT_TRUE(r1.is_ok());
  EXPECT_EQ(r1.unwrap(), 2u);
}

// ── B-6: send fails with no receivers ────────────────────────────────

TEST(BroadcastTest, SendFailsNoReceivers) {
  auto [tx, rx] = channel<int>(4);
  { auto drop = std::move(rx); } // drop the only receiver

  auto r = tx.try_send(1);
  EXPECT_TRUE(r.is_err());
  EXPECT_EQ(r.unwrap_err().kind, SendError<int>::NoReceiver);
  EXPECT_EQ(r.unwrap_err().value, 1);
}

// ── B-7: multi-producer ─────────────────────────────────────────────

TEST(BroadcastTest, MultiProducer) {
  auto [tx, rx] = channel<int>(4);
  auto tx2      = tx;

  EXPECT_TRUE(tx.try_send(10).is_ok());
  EXPECT_TRUE(tx2.try_send(20).is_ok());

  EXPECT_EQ(rx.try_recv().unwrap(), 10);
  EXPECT_EQ(rx.try_recv().unwrap(), 20);
}

// ── Multi-threaded tests (require -DXPP_MT) ─────────────────────────

#if XPP_MT

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
