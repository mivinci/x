/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event_test.cpp - Tests for xpp::EventLoop and xpp::WaitScope
 */

#include <gtest/gtest.h>
#include <xpp/event.h>

#include <x/base/event.h>

/* ───────────────────── Create / Destroy ───────────────────── */

TEST(EventLoopTest, CreateAndDestroy) {
  xpp::EventLoop loop;
  EXPECT_TRUE(loop);
}

TEST(EventLoopTest, CreateDoesNotEnter) {
  /* Without WaitScope, xEventLoopCurrent() should be NULL. */
  xpp::EventLoop loop;
  ASSERT_TRUE(loop);
  EXPECT_EQ(xEventLoopCurrent(), nullptr);
}

/* ───────────────────── WaitScope ───────────────────── */

TEST(WaitScopeTest, EnterAndLeave) {
  xpp::EventLoop loop;
  ASSERT_TRUE(loop);

  {
    xpp::WaitScope scope(loop);
    EXPECT_EQ(xEventLoopCurrent(), loop.handle());
  }

  /* After scope, loop should be left */
  EXPECT_EQ(xEventLoopCurrent(), nullptr);
}

TEST(WaitScopeTest, NestedScopes) {
  xpp::EventLoop loop;
  ASSERT_TRUE(loop);

  {
    xpp::WaitScope outer(loop);
    EXPECT_EQ(xEventLoopCurrent(), loop.handle());

    {
      xpp::WaitScope inner(loop);
      EXPECT_EQ(xEventLoopCurrent(), loop.handle());
    }

    /* Inner scope left, outer still active */
    EXPECT_EQ(xEventLoopCurrent(), loop.handle());
  }

  EXPECT_EQ(xEventLoopCurrent(), nullptr);
}

/* ───────────────────── Move ───────────────────── */

TEST(EventLoopTest, MoveTransfersOwnership) {
  xpp::EventLoop loop1;
  ASSERT_TRUE(loop1);
  xEventLoop raw = loop1.handle();

  xpp::EventLoop loop2 = std::move(loop1);
  EXPECT_FALSE(loop1);
  EXPECT_TRUE(loop2);
  EXPECT_EQ(loop2.handle(), raw);
}

TEST(EventLoopTest, MoveAssignReleasesOld) {
  xpp::EventLoop loop1;
  xpp::EventLoop loop2;
  ASSERT_TRUE(loop1);
  ASSERT_TRUE(loop2);

  loop2 = std::move(loop1);
  EXPECT_TRUE(loop2);
  EXPECT_FALSE(loop1);
}

/* ───────────────────── Run / Stop ───────────────────── */

TEST(EventLoopTest, StopReturnsFromRun) {
  xpp::EventLoop loop;
  ASSERT_TRUE(loop);

  xpp::WaitScope scope(loop);

  xTimer t = xTimerStart(
    [](void *arg) {
      auto *l = static_cast<xpp::EventLoop *>(arg);
      l->stop();
    },
    &loop, 50, 0);

  loop.run();
  if (t) xTimerStop(t);
}

TEST(EventLoopTest, RunWithRunModeDefault) {
  xpp::EventLoop loop;
  ASSERT_TRUE(loop);

  xpp::WaitScope scope(loop);

  xTimer t =
    xTimerStart([](void *arg) { static_cast<xpp::EventLoop *>(arg)->stop(); }, &loop, 30, 0);

  loop.run(xpp::RunMode::Default);
  if (t) xTimerStop(t);
}

TEST(EventLoopTest, RunNoWaitDoesNotBlock) {
  xpp::EventLoop loop;
  ASSERT_TRUE(loop);

  xpp::WaitScope scope(loop);

  loop.run(xpp::RunMode::NoWait);
  SUCCEED();
}

/* ───────────────────── handle interop ───────────────────── */

TEST(EventLoopTest, HandleInteropWithC) {
  xpp::EventLoop loop;
  ASSERT_TRUE(loop);

  EXPECT_EQ(xEventLoopFd(loop.handle()), xEventLoopFd(loop.handle()));
}

TEST(EventLoopTest, WakeIsSafeNoOp) {
  xpp::EventLoop loop;
  ASSERT_TRUE(loop);

  loop.wake();
  SUCCEED();
}
