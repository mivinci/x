/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * magnet_test.cpp - Magnet URI parser tests
 */

#include <gtest/gtest.h>

#include <cstring>

extern "C" {
#include <xdl/magnet.h>
}

static int hex_digit_impl(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int hex_decode_expected(const char *hex, uint8_t *out) {
  for (int i = 0; i < 20; i++) {
    int hi = hex_digit_impl(hex[i * 2]);
    int lo = hex_digit_impl(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return -1;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return 0;
}

#define H40_0 "0000000000000000000000000000000000000000"
#define H40_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define H40_P "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2"
#define H40_X "ABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCD"

TEST(Magnet, ParseMinimal) {
  xdl_magnet_result_t r;
  ASSERT_EQ(xdl_magnet_parse("magnet:?xt=urn:btih:" H40_P, &r), 0);
  uint8_t expected[20];
  ASSERT_EQ(hex_decode_expected(H40_P, expected), 0);
  EXPECT_EQ(memcmp(r.info_hash, expected, 20), 0);
  EXPECT_STREQ(r.name, "");
  EXPECT_EQ(r.tracker_count, 0);
  xdl_magnet_result_free(&r);
}

TEST(Magnet, ParseWithDisplayName) {
  xdl_magnet_result_t r;
  ASSERT_EQ(xdl_magnet_parse("magnet:?xt=urn:btih:" H40_A "&dn=ubuntu.iso", &r), 0);
  EXPECT_STREQ(r.name, "ubuntu.iso");
  xdl_magnet_result_free(&r);
}

TEST(Magnet, ParseWithOneTracker) {
  xdl_magnet_result_t r;
  ASSERT_EQ(xdl_magnet_parse("magnet:?xt=urn:btih:" H40_A "&tr=http://t1:8080", &r), 0);
  EXPECT_EQ(r.tracker_count, 1);
  EXPECT_STREQ(r.trackers[0], "http://t1:8080");
  xdl_magnet_result_free(&r);
}

TEST(Magnet, ParseWithMultipleTrackers) {
  xdl_magnet_result_t r;
  ASSERT_EQ(xdl_magnet_parse("magnet:?xt=urn:btih:" H40_A
                             "&tr=http://t1:8080&tr=http://t2:8080&tr=http://t3:8080", &r), 0);
  EXPECT_EQ(r.tracker_count, 3);
  EXPECT_STREQ(r.trackers[0], "http://t1:8080");
  EXPECT_STREQ(r.trackers[1], "http://t2:8080");
  EXPECT_STREQ(r.trackers[2], "http://t3:8080");
  xdl_magnet_result_free(&r);
}

TEST(Magnet, ParseFullURI) {
  xdl_magnet_result_t r;
  ASSERT_EQ(xdl_magnet_parse("magnet:?xt=urn:btih:" H40_A
                             "&dn=ubuntu.iso&tr=http://t1:8080&tr=http://t2:8080", &r), 0);
  EXPECT_STREQ(r.name, "ubuntu.iso");
  EXPECT_EQ(r.tracker_count, 2);
  xdl_magnet_result_free(&r);
}

TEST(Magnet, ParseURLEncodedName) {
  xdl_magnet_result_t r;
  ASSERT_EQ(xdl_magnet_parse("magnet:?xt=urn:btih:" H40_A "&dn=ubuntu%2024.04.iso", &r), 0);
  EXPECT_STREQ(r.name, "ubuntu 24.04.iso");
  xdl_magnet_result_free(&r);
}

TEST(Magnet, ParsePlusEncodedSpace) {
  xdl_magnet_result_t r;
  ASSERT_EQ(xdl_magnet_parse("magnet:?xt=urn:btih:" H40_A "&dn=ubuntu+24.04.iso", &r), 0);
  EXPECT_STREQ(r.name, "ubuntu 24.04.iso");
  xdl_magnet_result_free(&r);
}

TEST(Magnet, HexCaseInsensitive) {
  xdl_magnet_result_t r;
  ASSERT_EQ(xdl_magnet_parse("magnet:?xt=urn:btih:" H40_X, &r), 0);
  uint8_t expected[20];
  ASSERT_EQ(hex_decode_expected(H40_X, expected), 0);
  EXPECT_EQ(memcmp(r.info_hash, expected, 20), 0);
  xdl_magnet_result_free(&r);
}

TEST(Magnet, ErrorNoXt) {
  xdl_magnet_result_t r;
  EXPECT_NE(xdl_magnet_parse("magnet:?dn=ubuntu.iso", &r), 0);
}

TEST(Magnet, ErrorNoXtPrefix) {
  xdl_magnet_result_t r;
  EXPECT_NE(xdl_magnet_parse("magnet:?xt=" H40_A, &r), 0);
}

TEST(Magnet, ErrorWrongScheme) {
  xdl_magnet_result_t r;
  EXPECT_NE(xdl_magnet_parse("http://example.com", &r), 0);
}

TEST(Magnet, ErrorNoMagnet) {
  xdl_magnet_result_t r;
  EXPECT_NE(xdl_magnet_parse("xt=urn:btih:" H40_A, &r), 0);
}

TEST(Magnet, ErrorInvalidHex) {
  xdl_magnet_result_t r;
  EXPECT_NE(xdl_magnet_parse("magnet:?xt=urn:btih:zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz", &r), 0);
}

TEST(Magnet, ErrorShortHex) {
  xdl_magnet_result_t r;
  EXPECT_NE(xdl_magnet_parse("magnet:?xt=urn:btih:abc", &r), 0);
}

TEST(Magnet, ErrorNullURI) {
  xdl_magnet_result_t r;
  EXPECT_NE(xdl_magnet_parse(NULL, &r), 0);
}

TEST(Magnet, ErrorNullResult) {
  EXPECT_NE(xdl_magnet_parse("magnet:?xt=urn:btih:" H40_A, NULL), 0);
}

TEST(Magnet, ErrorEmptyString) {
  xdl_magnet_result_t r;
  EXPECT_NE(xdl_magnet_parse("", &r), 0);
}

TEST(Magnet, ParseAllZero) {
  xdl_magnet_result_t r;
  ASSERT_EQ(xdl_magnet_parse("magnet:?xt=urn:btih:" H40_0, &r), 0);
  for (int i = 0; i < 20; i++) EXPECT_EQ(r.info_hash[i], 0);
  xdl_magnet_result_free(&r);
}

TEST(Magnet, IgnoreUnknownParams) {
  xdl_magnet_result_t r;
  ASSERT_EQ(xdl_magnet_parse("magnet:?xs=http://example.com&xt=urn:btih:" H40_A "&dn=test", &r), 0);
  EXPECT_STREQ(r.name, "test");
  xdl_magnet_result_free(&r);
}
