/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * error_test.cpp — Tests for xpp::io::Error.
 */
#include <cerrno>

#include <gtest/gtest.h>
#include <xpp/io/error.h>

/* ── Size ──────────────────────────────────────────────────────────── */

TEST(ErrorTest, Size) {
  EXPECT_EQ(sizeof(xpp::io::Error), sizeof(int32_t));
}

/* ── from_errno ────────────────────────────────────────────────────── */

TEST(ErrorTest, FromErrno) {
  auto err = xpp::io::Error::from_errno(EADDRINUSE);
  EXPECT_EQ(err.kind(), xpp::io::ErrorKind::AddrInUse);
  EXPECT_EQ(err.raw_os_error(), EADDRINUSE);
  EXPECT_EQ(err.raw_xerrno(), xErrno_Ok);
}

TEST(ErrorTest, FromErrnoConnectionRefused) {
  auto err = xpp::io::Error::from_errno(ECONNREFUSED);
  EXPECT_EQ(err.kind(), xpp::io::ErrorKind::ConnectionRefused);
  EXPECT_EQ(err.raw_os_error(), ECONNREFUSED);
}

/* ── from_xerrno ───────────────────────────────────────────────────── */

TEST(ErrorTest, FromXErrno) {
  auto err = xpp::io::Error::from_xerrno(xErrno_DnsNotFound);
  EXPECT_EQ(err.kind(), xpp::io::ErrorKind::HostNotFound);
  EXPECT_EQ(err.raw_xerrno(), xErrno_DnsNotFound);
  EXPECT_EQ(err.raw_os_error(), 0);
}

TEST(ErrorTest, FromXErrnoTimeout) {
  auto err = xpp::io::Error::from_xerrno(xErrno_Timeout);
  EXPECT_EQ(err.kind(), xpp::io::ErrorKind::TimedOut);
  EXPECT_EQ(err.raw_xerrno(), xErrno_Timeout);
}

/* ── from_kind ─────────────────────────────────────────────────────── */

TEST(ErrorTest, FromKind) {
  auto err = xpp::io::Error::from_kind(xpp::io::ErrorKind::InvalidInput);
  EXPECT_EQ(err.kind(), xpp::io::ErrorKind::InvalidInput);
  EXPECT_EQ(err.raw_os_error(), 0);
  EXPECT_EQ(err.raw_xerrno(), xErrno_Ok);
}

/* ── from_kind + os_errno ──────────────────────────────────────────── */

TEST(ErrorTest, FromKindWithErrno) {
  xpp::io::Error err(xpp::io::ErrorKind::AddrInUse, EADDRINUSE);
  EXPECT_EQ(err.kind(), xpp::io::ErrorKind::AddrInUse);
  EXPECT_EQ(err.raw_os_error(), EADDRINUSE);
}

/* ── Equality ──────────────────────────────────────────────────────── */

TEST(ErrorTest, Equality) {
  auto e1 = xpp::io::Error::from_errno(EADDRINUSE);
  auto e2 = xpp::io::Error::from_errno(EADDRINUSE);
  auto e3 = xpp::io::Error::from_kind(xpp::io::ErrorKind::InvalidInput);
  EXPECT_EQ(e1, e2);
  EXPECT_NE(e1, e3);
}

/* ── Distinct sources don't collide ────────────────────────────────── */

TEST(ErrorTest, ErrnoVsXErrnoDistinct) {
  auto e1 = xpp::io::Error::from_errno(48); // EADDRINUSE on macOS
  auto e2 = xpp::io::Error::from_xerrno(xErrno_DnsNotFound);
  // Both might map to AddrInUse/HostNotFound but the raw code differs
  EXPECT_NE(e1, e2);
}
