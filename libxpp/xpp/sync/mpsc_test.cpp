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
  do_try_send_mixed().wait();
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
  do_raii_close().wait();
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
  do_raii_close_recv().wait();
}
