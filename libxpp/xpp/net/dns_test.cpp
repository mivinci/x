/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * dns_test.cpp — Tests for xpp::net::lookup_host.
 */

#include <gtest/gtest.h>
#include <xpp/net/tcp.h>
#include <xpp/promise.h>

#include <x/base/event.h>

using xpp::net::lookup_host;

TEST(DnsResolveTest, ResolveLocalhost) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto addrs = lookup_host("localhost").await();
  EXPECT_FALSE(addrs.empty());
}

TEST(DnsResolveTest, ResolveInvalidReturnsEmpty) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto addrs = lookup_host("nonexistent-host-xyz123.invalid").await();
  EXPECT_TRUE(addrs.empty());
}
