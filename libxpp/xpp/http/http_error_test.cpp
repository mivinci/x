/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * error_test.cpp — Tests for xpp::http::Error and http::Result<T>.
 */

#include <gtest/gtest.h>
#include <xpp/http/error.h>

using namespace xpp;
using namespace xpp::http;

/* ───────────────────────────────────────────────────────────────────
 *  Error construction
 * ─────────────────────────────────────────────────────────────────── */

TEST(ErrorTest, DefaultConstructorIsIo) {
  Error e;
  EXPECT_EQ(e.kind(), Error::Kind::Io);
  EXPECT_TRUE(e.message().empty());
}

TEST(ErrorTest, ConstructWithKindAndMessage) {
  Error e(Error::Kind::Timeout, String::from_utf8("connect timed out").unwrap());
  EXPECT_EQ(e.kind(), Error::Kind::Timeout);
  EXPECT_EQ(e.message(), String::from_utf8("connect timed out").unwrap());
}

/* ───────────────────────────────────────────────────────────────────
 *  Predicates
 * ─────────────────────────────────────────────────────────────────── */

TEST(ErrorPredicates, IsConnect) {
  Error e(Error::Kind::Connect, String());
  EXPECT_TRUE(e.is_connect());
  EXPECT_FALSE(e.is_timeout());
}

TEST(ErrorPredicates, IsTimeout) {
  Error e(Error::Kind::Timeout, String());
  EXPECT_TRUE(e.is_timeout());
  EXPECT_FALSE(e.is_connect());
}

TEST(ErrorPredicates, IsDns) {
  Error e(Error::Kind::Dns, String());
  EXPECT_TRUE(e.is_dns());
}

TEST(ErrorPredicates, IsRedirect) {
  Error e(Error::Kind::TooManyRedirects, String());
  EXPECT_TRUE(e.is_redirect());
}

TEST(ErrorPredicates, IsInvalidUrl) {
  Error e(Error::Kind::InvalidUrl, String());
  EXPECT_TRUE(e.is_invalid_url());
}

TEST(ErrorPredicates, IsIo) {
  Error e(Error::Kind::Io, String());
  EXPECT_TRUE(e.is_io());
}

TEST(ErrorPredicates, IsProtocol) {
  Error e(Error::Kind::Protocol, String());
  EXPECT_TRUE(e.is_protocol());
}

TEST(ErrorPredicates, IsTls) {
  Error e(Error::Kind::Tls, String());
  EXPECT_TRUE(e.is_tls());
}

TEST(ErrorPredicates, IsBody) {
  Error e(Error::Kind::Body, String());
  EXPECT_TRUE(e.is_body());
}

/* ───────────────────────────────────────────────────────────────────
 *  http::Result<T> alias
 * ─────────────────────────────────────────────────────────────────── */

TEST(HttpResultAlias, OkCarriesValue) {
  http::Result<int> r(xpp::ok, 42);
  EXPECT_TRUE(r.is_ok());
  EXPECT_EQ(r.unwrap(), 42);
}

TEST(HttpResultAlias, ErrCarriesError) {
  http::Result<int> r(xpp::err, Error(Error::Kind::Timeout, String()));
  EXPECT_TRUE(r.is_err());
  EXPECT_EQ(r.unwrap_err().kind(), Error::Kind::Timeout);
}

TEST(HttpResultAlias, DefaultErrorIsHttpError) {
  // Verify the alias uses http::Error, not some other error type.
  static_assert(std::is_same<http::Result<int>::error_type, Error>::value,
                "http::Result<T> must use http::Error as the error type");
}
