/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * mpsc_test.cpp — Tests for xpp::sync::mpsc.
 */
#include <thread>

#include <gtest/gtest.h>
#include <xpp/promise.h>
#include <xpp/spawn.h>
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
  do_send_recv().await();
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
  do_multi_producer().await();
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
  do_buffer_full().await();
}

xpp::Promise<void> do_buffer_full_retry_value_correct() {
  // Verifies that when send() retries after a full buffer,
  // the original value is preserved — not lost to a moved-from state.
  auto [tx, rx] = xpp::sync::mpsc::channel<int>(1);

  co_await tx.send(100);      // fills the single slot
  auto send_p = tx.send(200); // blocks, value must survive retry

  EXPECT_EQ((co_await rx.recv()).unwrap(), 100); // drain
  co_await std::move(send_p);                    // should now succeed
  EXPECT_EQ((co_await rx.recv()).unwrap(), 200); // value must be 200

  tx.close();
  co_return;
}

TEST(MpscTest, BufferFullRetryValueCorrect) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_buffer_full_retry_value_correct().await();
}

TEST(MpscTest, TrySendSuccess) {
  auto [tx, rx] = xpp::sync::mpsc::channel<int>(4);
  auto r        = tx.try_send(42);
  EXPECT_TRUE(r.is_ok());
  auto v = rx.try_recv();
  ASSERT_TRUE(v.is_ok());
  EXPECT_EQ(v.unwrap(), 42);
}

TEST(MpscTest, TrySendFull) {
  auto [tx, rx] = xpp::sync::mpsc::channel<int>(2);
  EXPECT_TRUE(tx.try_send(1).is_ok());
  EXPECT_TRUE(tx.try_send(2).is_ok());
  auto r = tx.try_send(3);
  ASSERT_TRUE(r.is_err());
  EXPECT_EQ(r.unwrap_err().kind, xpp::sync::mpsc::TrySendError<int>::Full);
  EXPECT_EQ(r.unwrap_err().value, 3);
  // drain
  (void)rx;
}

TEST(MpscTest, TrySendClosed) {
  auto [tx, rx] = xpp::sync::mpsc::channel<int>(4);
  tx.close();
  auto r = tx.try_send(1);
  ASSERT_TRUE(r.is_err());
  EXPECT_EQ(r.unwrap_err().kind, xpp::sync::mpsc::TrySendError<int>::Closed);
  (void)rx;
}

TEST(MpscTest, TryRecvEmpty) {
  auto [tx, rx] = xpp::sync::mpsc::channel<int>(4);
  auto r        = rx.try_recv();
  ASSERT_TRUE(r.is_err());
  EXPECT_EQ(r.unwrap_err(), xpp::sync::mpsc::TryRecvError::Empty);
  (void)tx;
}

TEST(MpscTest, TryRecvClosed) {
  auto [tx, rx] = xpp::sync::mpsc::channel<int>(4);
  tx.close();
  auto r = rx.try_recv();
  ASSERT_TRUE(r.is_err());
  EXPECT_EQ(r.unwrap_err(), xpp::sync::mpsc::TryRecvError::Closed);
}

xpp::Promise<void> do_try_send_mixed() {
  auto [tx, rx] = xpp::sync::mpsc::channel<int>(4);

  // try_send first, then async recv
  EXPECT_TRUE(tx.try_send(10).is_ok());
  EXPECT_TRUE(tx.try_send(20).is_ok());

  // async send interleaved
  co_await tx.send(30);

  EXPECT_EQ((co_await rx.recv()).unwrap(), 10);
  EXPECT_EQ((co_await rx.recv()).unwrap(), 20);
  EXPECT_EQ(rx.try_recv().unwrap(), 30);
  tx.close();
  co_return;
}

TEST(MpscTest, TrySendRecvMixed) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_try_send_mixed().await();
}

// ── RAII close ──────────────────────────────────────────────────────

xpp::Promise<void> do_raii_close() {
  auto [tx, rx] = xpp::sync::mpsc::channel<int>(4);
  co_await tx.send(100);
  // Let a cloned sender go out of scope; the original is still alive.
  {
    auto tx2 = tx;
    co_await tx2.send(200);
  }
  // tx2 dropped → sender_count = 1 → not yet closed
  co_await tx.send(300);
  // tx drops → last sender → auto-close
  co_return;
}

TEST(MpscTest, RaiiClose) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_raii_close().await();
}

xpp::Promise<void> do_raii_close_recv() {
  auto rx = ([]() -> xpp::sync::mpsc::Receiver<int> {
    auto [tx, rx] = xpp::sync::mpsc::channel<int>(4);
    EXPECT_TRUE(tx.try_send(42).is_ok());
    // tx drops here → last sender → auto-close
    return std::move(rx);
  })();

  auto v1 = co_await rx.recv();
  EXPECT_TRUE(v1.is_some());
  EXPECT_EQ(v1.unwrap(), 42);
  auto v2 = co_await rx.recv();
  EXPECT_TRUE(v2.is_none());
  co_return;
}

TEST(MpscTest, RaiiCloseRecv) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_raii_close_recv().await();
}

// ── Multi-threaded tests ──────────────────────────────────────────

TEST(MpscMtTest, CrossThreadSendRecv) {
  auto [tx, rx] = xpp::sync::mpsc::channel<int>(4);

  std::thread ta([&tx] {
    xpp::EventLoop loop;
    xpp::WaitScope scope(loop);

    auto sender = [&]() -> xpp::Promise<void> {
      co_await tx.send(10);
      co_await tx.send(20);
      co_await tx.send(30);
      tx.close();
      co_return;
    };
    sender().await();
  });

  std::thread tb([&rx] {
    xpp::EventLoop loop;
    xpp::WaitScope scope(loop);

    auto receiver = [&]() -> xpp::Promise<void> {
      EXPECT_TRUE((co_await rx.recv()).is_some());
      EXPECT_TRUE((co_await rx.recv()).is_some());
      EXPECT_TRUE((co_await rx.recv()).is_some());
      EXPECT_TRUE((co_await rx.recv()).is_none());
      co_return;
    };
    receiver().await();
  });

  ta.join();
  tb.join();
}

TEST(MpscMtTest, CrossThreadBufferFull) {
  auto [tx, rx] = xpp::sync::mpsc::channel<int>(2);

  std::thread ta([&tx] {
    xpp::EventLoop loop;
    xpp::WaitScope scope(loop);

    auto sender = [&]() -> xpp::Promise<void> {
      co_await tx.send(1);
      co_await tx.send(2);
      co_await tx.send(3);
      tx.close();
      co_return;
    };
    sender().await();
  });

  std::thread tb([&rx] {
    xpp::EventLoop loop;
    xpp::WaitScope scope(loop);

    auto receiver = [&]() -> xpp::Promise<void> {
      EXPECT_EQ((co_await rx.recv()).unwrap(), 1);
      EXPECT_EQ((co_await rx.recv()).unwrap(), 2);
      EXPECT_EQ((co_await rx.recv()).unwrap(), 3);
      EXPECT_TRUE((co_await rx.recv()).is_none());
      co_return;
    };
    receiver().await();
  });

  ta.join();
  tb.join();
}

TEST(MpscMtTest, TrySendRecvWorkerThread) {
  auto [tx, rx] = xpp::sync::mpsc::channel<int>(4);

  std::thread worker([&tx] {
    for (int i = 0; i < 4; ++i) {
      auto r = tx.try_send(i);
      EXPECT_TRUE(r.is_ok()) << "try_send " << i << " failed";
    }
  });

  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto recv_all = [&]() -> xpp::Promise<void> {
    for (int i = 0; i < 4; ++i) {
      auto v = co_await rx.recv();
      EXPECT_TRUE(v.is_some());
      EXPECT_EQ(v.unwrap(), i);
    }
    co_return;
  };
  recv_all().await();
  worker.join();
}

/* ───────────────────────────────────────────────────────────────────
 *  Lost-wakeup stress regression (issues/mpsc-single-slot-waiter-race.md).
 *
 *  Each round races a worker thread's try_send against the loop
 *  thread's recv-park. The old single-slot PromiseResolver waiter lost
 *  the wakeup in the check-then-act window (recv: pop-empty →
 *  register vs send: push → check-waiter) and hung within ~500-1000
 *  rounds of this exact shape.
 * ─────────────────────────────────────────────────────────────────── */
TEST(MpscMtTest, StressLostWakeupRegression) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  for (int r = 0; r < 2000; ++r) {
    auto [tx, rx] = xpp::sync::mpsc::channel<int>(4);

    std::thread worker([&tx] {
      for (int i = 0; i < 4; ++i) {
        auto res = tx.try_send(i);
        EXPECT_TRUE(res.is_ok());
      }
    });

    for (int i = 0; i < 4; ++i) {
      auto v = rx.recv().await();
      ASSERT_TRUE(v.is_some());
    }
    worker.join();
  }
}

/* ───────────────────────────────────────────────────────────────────
 *  Multiple producers suspended on a full buffer (cap=1): the writer
 *  FIFO must park them all in order (the old single write-waiter slot
 *  overwrote earlier waiters — a parked sender would hang forever) and
 *  drain them FIFO, one wake per pop.
 * ─────────────────────────────────────────────────────────────────── */
static xpp::Promise<void> co_send_one(xpp::sync::mpsc::Sender<int> tx, int v) {
  co_await tx.send(v);
  co_return;
}

TEST(MpscMtTest, MultipleSuspendedSenders) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto [tx, rx] = xpp::sync::mpsc::channel<int>(1);
  ASSERT_TRUE(tx.try_send(0).is_ok()); // fill the single slot

  // Three senders park on the full buffer, in FIFO order.
  xpp::spawn(co_send_one(tx, 1));
  xpp::spawn(co_send_one(tx, 2));
  xpp::spawn(co_send_one(tx, 3));

  // Drain: values arrive 0,1,2,3 — one parked sender per pop.
  for (int expect = 0; expect < 4; ++expect) {
    auto v = rx.recv().await();
    ASSERT_TRUE(v.is_some());
    EXPECT_EQ(v.unwrap(), expect);
  }
}

TEST(MpscMtTest, MultiProducerThreads) {
  auto [tx, rx] = xpp::sync::mpsc::channel<int>(64);

  std::thread t1([tx]() mutable {
    for (int i = 0; i < 10; ++i)
      EXPECT_TRUE(tx.try_send(i).is_ok());
  });
  std::thread t2([tx]() mutable {
    for (int i = 100; i < 110; ++i)
      EXPECT_TRUE(tx.try_send(i).is_ok());
  });
  std::thread t3([tx]() mutable {
    for (int i = 200; i < 210; ++i)
      EXPECT_TRUE(tx.try_send(i).is_ok());
  });

  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto consumer = [&]() -> xpp::Promise<void> {
    int sum = 0;
    for (int i = 0; i < 30; ++i) {
      auto v = co_await rx.recv();
      EXPECT_TRUE(v.is_some());
      sum += v.unwrap();
    }
    EXPECT_EQ(sum, 3135);
    co_return;
  };
  consumer().await();

  t1.join();
  t2.join();
  t3.join();
}

TEST(MpscMtTest, UnboundedCrossThread) {
  auto [tx, rx] = xpp::sync::mpsc::channel<int>();

  std::thread worker([&tx] {
    for (int i = 1; i <= 5; ++i)
      EXPECT_TRUE(tx.try_send(i));
    tx.close();
  });

  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto recver = [&]() -> xpp::Promise<void> {
    for (int i = 1; i <= 5; ++i) {
      auto v = co_await rx.recv();
      EXPECT_TRUE(v.is_some());
      EXPECT_EQ(v.unwrap(), i);
    }
    EXPECT_TRUE((co_await rx.recv()).is_none());
    co_return;
  };
  recver().await();
  worker.join();
}
