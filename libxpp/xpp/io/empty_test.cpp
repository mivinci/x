/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * empty_test.cpp — Tests for xpp::io::Empty.
 */
#include <gtest/gtest.h>
#include <xpp/io/empty.h>
#include <xpp/promise.h>

TEST(EmptyTest, ReadReturnsZero) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  xpp::io::Empty e;
  ssize_t        n = e.read(nullptr, 10).wait();
  EXPECT_EQ(n, 0);
}
