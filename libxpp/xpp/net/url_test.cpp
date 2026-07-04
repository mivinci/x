/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * url_test.cpp — Tests for xpp::net::Url.
 */

#include <string>

#include <gtest/gtest.h>
#include <xpp/net/url.h>

using xpp::net::Url;
using xpp::net::UrlParseError;

TEST(UrlTest, ParseValidUrl) {
  auto r = Url::parse("https://example.com:8080/path?q=1");
  ASSERT_TRUE(r.is_ok());
  auto u = std::move(r).unwrap();
  EXPECT_EQ(u.scheme(), "https");
  EXPECT_EQ(u.host(), "example.com");
  EXPECT_EQ(u.port_num(), 8080);
  EXPECT_EQ(u.path(), "/path");
  EXPECT_EQ(u.query(), "q=1");
}

TEST(UrlTest, ParseInvalidUrl) {
  auto r = Url::parse("not a url");
  EXPECT_TRUE(r.is_err());
  EXPECT_EQ(r.unwrap_err(), UrlParseError::InvalidFormat);
}

TEST(UrlTest, ParseEmpty) {
  auto r = Url::parse("");
  EXPECT_TRUE(r.is_err());
  EXPECT_EQ(r.unwrap_err(), UrlParseError::Empty);
}

TEST(UrlTest, DefaultPort) {
  auto r1 = Url::parse("http://localhost/api");
  ASSERT_TRUE(r1.is_ok());
  EXPECT_EQ(std::move(r1).unwrap().port_num(), 80);

  auto r2 = Url::parse("https://localhost/api");
  ASSERT_TRUE(r2.is_ok());
  EXPECT_EQ(std::move(r2).unwrap().port_num(), 443);
}

TEST(UrlTest, MoveSemantics) {
  auto r = Url::parse("http://example.com/path");
  ASSERT_TRUE(r.is_ok());
  Url u = std::move(r).unwrap();

  Url moved = std::move(u);
  EXPECT_EQ(moved.scheme(), "http");
  EXPECT_EQ(moved.host(), "example.com");
  EXPECT_EQ(moved.path(), "/path");
}
