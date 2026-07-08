/*
 * http_error_test.cpp — Tests for xpp::http::Error.
 */
#include <gtest/gtest.h>
#include <xpp/http/error.h>

TEST(HttpErrorTest, Builder) {
  auto e = xpp::http::Error::builder("bad url");
  EXPECT_EQ(e.kind(), xpp::http::Error::Builder);
  EXPECT_TRUE(e.is_builder());
  EXPECT_FALSE(e.is_request());
  EXPECT_STREQ(e.message().c_str(), "bad url");
}

TEST(HttpErrorTest, Request) {
  auto e = xpp::http::Error::request("connection refused");
  EXPECT_EQ(e.kind(), xpp::http::Error::Request);
  EXPECT_TRUE(e.is_request());
  EXPECT_FALSE(e.is_timeout());
  EXPECT_STREQ(e.message().c_str(), "connection refused");
}

TEST(HttpErrorTest, Timeout) {
  auto e = xpp::http::Error::timeout("timed out after 30s");
  EXPECT_EQ(e.kind(), xpp::http::Error::Timeout);
  EXPECT_TRUE(e.is_timeout());
  EXPECT_STREQ(e.message().c_str(), "timed out after 30s");
}

TEST(HttpErrorTest, StatusCode) {
  auto e = xpp::http::Error::status_code(404);
  EXPECT_EQ(e.kind(), xpp::http::Error::Status);
  EXPECT_TRUE(e.is_status());
  EXPECT_STREQ(e.message().c_str(), "HTTP 404");
}
