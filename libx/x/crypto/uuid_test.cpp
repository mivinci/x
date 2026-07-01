/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * uuid_test.cpp - Tests for UUID generation, formatting, and parsing
 */

#include <cstring>

#include <gtest/gtest.h>
#include <x/base/random.h>
#include <x/crypto/uuid.h>

/* ───────────────────── xRandomBytes ───────────────────── */

TEST(Random, GenerateBytes) {
  uint8_t buf[16];
  ASSERT_EQ(xRandomBytes(buf, 16), xErrno_Ok);

  /* Not all zeros */
  bool has_nonzero = false;
  for (int i = 0; i < 16; i++) {
    if (buf[i] != 0) has_nonzero = true;
  }
  EXPECT_TRUE(has_nonzero);
}

TEST(Random, UniqueAcrossCalls) {
  uint8_t a[16], b[16];
  xRandomBytes(a, 16);
  xRandomBytes(b, 16);
  EXPECT_NE(memcmp(a, b, 16), 0);
}

TEST(Random, NullBuffer) {
  EXPECT_EQ(xRandomBytes(NULL, 16), xErrno_InvalidArg);
}

TEST(Random, ZeroLength) {
  uint8_t buf[1];
  EXPECT_EQ(xRandomBytes(buf, 0), xErrno_Ok);
}

/* ───────────────────── UUID v4 ───────────────────── */

TEST(Uuid, V4VersionAndVariant) {
  xUuid u = xUuidV4();
  /* Version nibble (byte 6, high 4 bits) = 4 */
  EXPECT_EQ((u.bytes[6] >> 4) & 0x0F, 4);
  /* Variant (byte 8, top 2 bits) = 10 */
  EXPECT_EQ((u.bytes[8] >> 6) & 0x03, 2);
}

TEST(Uuid, V4Unique) {
  xUuid a = xUuidV4();
  xUuid b = xUuidV4();
  EXPECT_NE(xUuidCompare(a, b), 0);
}

/* ───────────────────── UUID v7 ───────────────────── */

TEST(Uuid, V7Version) {
  xUuid u = xUuidV7();
  EXPECT_EQ((u.bytes[6] >> 4) & 0x0F, 7);
}

TEST(Uuid, V7Sortable) {
  xUuid a = xUuidV7();
  /* Small delay to ensure different timestamp */
  usleep(2000);
  xUuid b = xUuidV7();
  /* b was generated after a, so a < b */
  EXPECT_LT(xUuidCompare(a, b), 0);
}

/* ───────────────────── UUID v5 ───────────────────── */

TEST(Uuid, V5Determinism) {
  xUuid ns = *xUuidNamespaceDns();
  xUuid a  = xUuidV5(ns, "example.com");
  xUuid b  = xUuidV5(ns, "example.com");
  EXPECT_EQ(xUuidCompare(a, b), 0);
}

TEST(Uuid, V5DifferentNames) {
  xUuid ns = *xUuidNamespaceDns();
  xUuid a  = xUuidV5(ns, "example.com");
  xUuid b  = xUuidV5(ns, "example.org");
  EXPECT_NE(xUuidCompare(a, b), 0);
}

TEST(Uuid, V5VersionAndVariant) {
  xUuid u = xUuidV5(*xUuidNamespaceDns(), "test");
  EXPECT_EQ((u.bytes[6] >> 4) & 0x0F, 5);
  EXPECT_EQ((u.bytes[8] >> 6) & 0x03, 2);
}

TEST(Uuid, V5RfcTestVector) {
  /* RFC 4122 Appendix A: v5(DNS, "example.com") */
  xUuid u = xUuidV5(*xUuidNamespaceDns(), "example.com");
  char  buf[37];
  xUuidToString(u, buf);
  /* Known test vector from RFC 4122 */
  EXPECT_STREQ(buf, "cfbff0d1-9375-5685-968c-48ce8b15ae17");
}

/* ───────────────────── String formatting ───────────────────── */

TEST(Uuid, ToStringNil) {
  xUuid nil = {};
  char  buf[37];
  xUuidToString(nil, buf);
  EXPECT_STREQ(buf, "00000000-0000-0000-0000-000000000000");
}

TEST(Uuid, ToStringRoundTrip) {
  xUuid u = xUuidV4();
  char  buf[37];
  xUuidToString(u, buf);

  xUuid parsed;
  ASSERT_EQ(xUuidFromString(buf, &parsed), xErrno_Ok);
  EXPECT_EQ(xUuidCompare(u, parsed), 0);
}

/* ───────────────────── String parsing ───────────────────── */

TEST(Uuid, ParseHyphenated) {
  xUuid u;
  ASSERT_EQ(xUuidFromString("550e8400-e29b-41d4-a716-446655440000", &u), xErrno_Ok);
  EXPECT_EQ(u.bytes[0], 0x55);
  EXPECT_EQ(u.bytes[6], 0x41); /* version nibble 4 */
  EXPECT_EQ(u.bytes[15], 0x00);
}

TEST(Uuid, ParseNonHyphenated) {
  xUuid u;
  ASSERT_EQ(xUuidFromString("550e8400e29b41d4a716446655440000", &u), xErrno_Ok);
  EXPECT_EQ(u.bytes[0], 0x55);
}

TEST(Uuid, ParseUppercase) {
  xUuid u;
  ASSERT_EQ(xUuidFromString("550E8400-E29B-41D4-A716-446655440000", &u), xErrno_Ok);
  EXPECT_EQ(u.bytes[0], 0x55);
}

TEST(Uuid, ParseInvalid) {
  xUuid u;
  EXPECT_EQ(xUuidFromString("not-a-uuid", &u), xErrno_InvalidArg);
  EXPECT_EQ(xUuidFromString("550e8400-e29b-41d4-a716", &u), xErrno_InvalidArg);
  EXPECT_EQ(xUuidFromString(NULL, &u), xErrno_InvalidArg);
  EXPECT_EQ(xUuidFromString("550e8400-e29b-41d4-a716-446655440000", NULL), xErrno_InvalidArg);
}

/* ───────────────────── Comparison ───────────────────── */

TEST(Uuid, CompareEqual) {
  xUuid a = *xUuidNamespaceDns();
  xUuid b = *xUuidNamespaceDns();
  EXPECT_EQ(xUuidCompare(a, b), 0);
}

TEST(Uuid, CompareLessThan) {
  xUuid a     = {};
  a.bytes[15] = 1;
  xUuid b     = {};
  b.bytes[15] = 2;
  EXPECT_LT(xUuidCompare(a, b), 0);
  EXPECT_GT(xUuidCompare(b, a), 0);
}

/* ───────────────────── Nil check ───────────────────── */

TEST(Uuid, IsNil) {
  xUuid nil = {};
  EXPECT_TRUE(xUuidIsNil(nil));

  xUuid u = xUuidV4();
  EXPECT_FALSE(xUuidIsNil(u));
}

/* ───────────────────── Namespace accessors ───────────────────── */

TEST(Uuid, NamespaceDns) {
  xUuid ns = *xUuidNamespaceDns();
  char  buf[37];
  xUuidToString(ns, buf);
  EXPECT_STREQ(buf, "6ba7b810-9dad-11d1-80b4-00c04fd430c8");
}

TEST(Uuid, NamespaceUrl) {
  xUuid ns = *xUuidNamespaceUrl();
  char  buf[37];
  xUuidToString(ns, buf);
  EXPECT_STREQ(buf, "6ba7b811-9dad-11d1-80b4-00c04fd430c8");
}
