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

/* ───────────────────────────────────────────────────────────────────
 *  Phase 5: status-carrying errors + to_string()
 * ─────────────────────────────────────────────────────────────────── */

TEST(ErrorStatus, StatusNoneByDefault) {
  Error e(Error::Kind::Timeout, String::from_utf8("timed out").unwrap());
  EXPECT_TRUE(e.status().is_none());
  EXPECT_FALSE(e.is_status_error());
}

TEST(ErrorStatus, StatusSetOnProtocolError) {
  Error e(Error::Kind::Protocol, String::from_utf8("server returned 404").unwrap(),
          StatusCode::NotFound);
  ASSERT_TRUE(e.status().is_some());
  EXPECT_EQ(e.status().unwrap(), StatusCode::NotFound);
  EXPECT_TRUE(e.is_status_error());
  EXPECT_TRUE(e.is_protocol());
}

TEST(ErrorStatus, IsStatusErrorFalseForTransportError) {
  // Even with a status accidentally set, is_status_error reflects status.is_some()
  Error e(Error::Kind::Connect, String::from_utf8("refused").unwrap());
  EXPECT_FALSE(e.is_status_error());
}

TEST(ErrorToString, FormatWithoutStatus) {
  Error e(Error::Kind::Timeout, String::from_utf8("connect timed out").unwrap());
  String s = e.to_string();
  // Format: "timeout: connect timed out"
  EXPECT_EQ(s, String::from_utf8("timeout: connect timed out").unwrap());
}

TEST(ErrorToString, FormatWithStatus) {
  Error e(Error::Kind::Protocol, String::from_utf8("not found").unwrap(),
          StatusCode::NotFound);
  String s = e.to_string();
  // Format: "protocol: not found (status 404)"
  EXPECT_EQ(s, String::from_utf8("protocol: not found (status 404)").unwrap());
}

TEST(ErrorToString, FormatDefaultError) {
  Error e;
  String s = e.to_string();
  // Default kind = Io, empty message
  EXPECT_EQ(s, String::from_utf8("io: ").unwrap());
}

TEST(ErrorToString, AllKindsHaveNames) {
  // Ensure kind_name() covers every enumerator — no "unknown" leak.
  Error::Kind kinds[] = {
      Error::Kind::Connect,    Error::Kind::Dns,         Error::Kind::Timeout,
      Error::Kind::TooManyRedirects, Error::Kind::InvalidUrl, Error::Kind::Io,
      Error::Kind::Protocol,   Error::Kind::Tls,         Error::Kind::Body,
  };
  for (auto k : kinds) {
    Error e(k, String::from_utf8("msg").unwrap());
    String s = e.to_string();
    EXPECT_FALSE(s.empty());
    // Should contain ": msg", not start with "unknown: msg"
    EXPECT_TRUE(s.contains(String::from_utf8(": msg").unwrap()));
    EXPECT_FALSE(s.starts_with(String::from_utf8("unknown").unwrap()));
  }
}
