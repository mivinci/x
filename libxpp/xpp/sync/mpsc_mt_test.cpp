/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * mpsc_mt_test.cpp — Multi-threaded tests for xpp::sync::mpsc.
 *
 * Requires -DXPP_MT. Each thread runs its own EventLoop; the channel
 * is shared as Arc<Channel> across threads.
 */
#include <gtest/gtest.h>
#include <xpp/promise.h>
#include <xpp/sync/mpsc.h>

#include <thread>

#if !XPP_MT
// Skip all tests when XPP_MT is not defined.
#else

using namespace xpp;
using namespace xpp::sync::mpsc;

// ── Cross-thread send/recv (each thread owns its EventLoop) ─────────

TEST(MpscMtTest, CrossThreadSendRecv) {
  auto [tx, rx] = channel<int>(4);

  std::thread ta([&tx] {
    EventLoop loop;
    WaitScope scope(loop);

    auto sender = [&]() -> Promise<void> {
      co_await tx.send(10);
      co_await tx.send(20);
      co_await tx.send(30);
      tx.close();
      co_return;
    };
    sender().wait();
  });

  std::thread tb([&rx] {
    EventLoop loop;
    WaitScope scope(loop);

    auto receiver = [&]() -> Promise<void> {
      EXPECT_TRUE((co_await rx.recv()).is_some());
      EXPECT_TRUE((co_await rx.recv()).is_some());
      EXPECT_TRUE((co_await rx.recv()).is_some());
      EXPECT_TRUE((co_await rx.recv()).is_none());
      co_return;
    };
    receiver().wait();
  });

  ta.join();
  tb.join();
}

TEST(MpscMtTest, CrossThreadBufferFull) {
  auto [tx, rx] = channel<int>(2);

  std::thread ta([&tx] {
    EventLoop loop;
    WaitScope scope(loop);

    auto sender = [&]() -> Promise<void> {
      // fill buffer (capacity 2)
      co_await tx.send(1);
      co_await tx.send(2);
      // this will suspend until receiver consumes a slot
      co_await tx.send(3);
      tx.close();
      co_return;
    };
    sender().wait();
  });

  std::thread tb([&rx] {
    EventLoop loop;
    WaitScope scope(loop);

    auto receiver = [&]() -> Promise<void> {
      EXPECT_EQ((co_await rx.recv()).unwrap(), 1);
      EXPECT_EQ((co_await rx.recv()).unwrap(), 2);
      EXPECT_EQ((co_await rx.recv()).unwrap(), 3);
      EXPECT_TRUE((co_await rx.recv()).is_none());
      co_return;
    };
    receiver().wait();
  });

  ta.join();
  tb.join();
}

// ── try_send / try_recv from a non-loop worker thread ───────────────

TEST(MpscMtTest, TrySendRecvWorkerThread) {
  auto [tx, rx] = channel<int>(4);

  std::thread worker([&tx] {
    for (int i = 0; i < 4; ++i) {
      auto r = tx.try_send(i);
      EXPECT_TRUE(r.is_ok()) << "try_send " << i << " failed";
    }
  });

  EventLoop loop;
  WaitScope scope(loop);

  auto recv_all = [&]() -> Promise<void> {
    for (int i = 0; i < 4; ++i) {
      auto v = co_await rx.recv();
      EXPECT_TRUE(v.is_some());
      EXPECT_EQ(v.unwrap(), i);
    }
    co_return;
  };
  recv_all().wait();

  worker.join();
}

// ── Multiple producers (worker threads), single consumer (coroutine) ─

TEST(MpscMtTest, MultiProducerThreads) {
  auto [tx, rx] = channel<int>(32);

  std::thread t1([tx]() mutable {
    for (int i = 0; i < 10; ++i) {
      EXPECT_TRUE(tx.try_send(i).is_ok());
    }
  });
  std::thread t2([tx]() mutable {
    for (int i = 100; i < 110; ++i) {
      EXPECT_TRUE(tx.try_send(i).is_ok());
    }
  });
  std::thread t3([tx]() mutable {
    for (int i = 200; i < 210; ++i) {
      EXPECT_TRUE(tx.try_send(i).is_ok());
    }
  });

  EventLoop loop;
  WaitScope scope(loop);

  auto consumer = [&]() -> Promise<void> {
    int sum = 0;
    for (int i = 0; i < 30; ++i) {
      auto v = co_await rx.recv();
      EXPECT_TRUE(v.is_some());
      sum += v.unwrap();
    }
    // 0..9=45, 100..109=1045, 200..209=2045 → total=3135
    EXPECT_EQ(sum, 3135);
    co_return;
  };
  consumer().wait();

  t1.join();
  t2.join();
  t3.join();
}

#endif // XPP_MT
