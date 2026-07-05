/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * notify_test.cpp — Tests for xpp::sync::Notify.
 */
#include <gtest/gtest.h>
#include <xpp/promise.h>
#include <xpp/sync/notify.h>

// ── N-1: basic notify_one → notified resolves ──────────────────────

TEST(NotifyTest, NotifyOneBasic) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  xpp::sync::Notify n;

  auto p = n.notified();
  n.notify_one();
  p.wait(); // should complete
}

// ── N-2: notify_waiters wakes all ───────────────────────────────────

TEST(NotifyTest, NotifyWaiters) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  xpp::sync::Notify n;

  auto p1 = n.notified();
  auto p2 = n.notified();
  n.notify_waiters();
  p1.wait();
  p2.wait();
}

// ── N-3: reusable ───────────────────────────────────────────────────

TEST(NotifyTest, Reusable) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  xpp::sync::Notify n;

  auto p1 = n.notified();
  n.notify_one();
  p1.wait();

  auto p2 = n.notified();
  n.notify_one();
  p2.wait();
}

// ── N-4: notify before notified is no-op ────────────────────────────

TEST(NotifyTest, NotifyBeforeNotified) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  xpp::sync::Notify n;

  n.notify_one(); // no waiters — no-op

  auto p = n.notified();
  n.notify_one();
  p.wait();
}
