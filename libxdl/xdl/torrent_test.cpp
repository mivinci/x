/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * torrent_test.cpp - Torrent parser tests
 */

#include <gtest/gtest.h>
#include <cstring>
#include <string>

extern "C" {
#include <xdl/torrent.h>
#include <xdl/bencode.h>
}

/* ── Helpers ───────────────────────────────────────────── */

/* Generate valid bencoded torrent data */
static std::string make_torrent(const std::string &name, uint64_t length,
                                 uint32_t block_length, const char *blocks, size_t blocks_len,
                                 const char *announce, const char **urls, int url_count) {
  char buf[256];
  std::string s = "d";
  if (announce) {
    int n = snprintf(buf, sizeof(buf), "8:announce%zu:%s", strlen(announce), announce);
    s += std::string(buf, n);
  }
  s += "4:infod";

  /* name */
  int n = snprintf(buf, sizeof(buf), "4:name%zu:%s", name.size(), name.c_str());
  s += std::string(buf, n);

  /* length */
  n = snprintf(buf, sizeof(buf), "6:lengthi%llue", (unsigned long long)length);
  s += std::string(buf, n);

  /* block length */
  n = snprintf(buf, sizeof(buf), "12:block lengthi%ue", block_length);
  s += std::string(buf, n);

  /* blocks (SHA1 hashes) */
  if (blocks && blocks_len > 0) {
    n = snprintf(buf, sizeof(buf), "6:blocks%zu:", blocks_len);
    s += std::string(buf, n);
    s += std::string(blocks, blocks_len);
  }

  /* url-list */
  if (urls && url_count > 0) {
    s += "8:url-listl";
    for (int i = 0; i < url_count; i++) {
      n = snprintf(buf, sizeof(buf), "%zu:%s", strlen(urls[i]), urls[i]);
      s += std::string(buf, n);
    }
    s += "e";
  }

  s += "ee"; /* close info, close top */
  return s;
}

#define SHA1_20 "01234567890123456789"
#define SHA2_20 "abcdefghijabcdefghij"

TEST(Torrent, ParseMinimal) {
  auto data = make_torrent("test.bin", 1048576, 262144, NULL, 0, NULL, NULL, 0);
  auto *t = xdl_torrent_parse(reinterpret_cast<const uint8_t *>(data.data()), data.size());
  ASSERT_NE(t, nullptr);
  EXPECT_STREQ(t->name, "test.bin");
  EXPECT_EQ(t->length, static_cast<uint64_t>(1048576));
  EXPECT_EQ(t->block_length, static_cast<uint32_t>(262144));
  EXPECT_EQ(t->block_count, static_cast<uint32_t>(4));
  EXPECT_EQ(t->announce, nullptr);
  EXPECT_EQ(t->http_url_count, 0);
  xdl_torrent_destroy(t);
}

TEST(Torrent, ParseWithAnnounce) {
  auto data = make_torrent("x", 100, 256, NULL, 0, "http://t:6969", NULL, 0);
  auto *t = xdl_torrent_parse(reinterpret_cast<const uint8_t *>(data.data()), data.size());
  ASSERT_NE(t, nullptr);
  EXPECT_STREQ(t->announce, "http://t:6969");
  xdl_torrent_destroy(t);
}

TEST(Torrent, ParseWithBlockHashes) {
  std::string hashes = std::string(SHA1_20) + std::string(SHA2_20);
  auto data = make_torrent("x", 524288, 262144, hashes.c_str(), hashes.size(), NULL, NULL, 0);
  auto *t = xdl_torrent_parse(reinterpret_cast<const uint8_t *>(data.data()), data.size());
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->block_count, static_cast<uint32_t>(2));
  ASSERT_NE(t->block_hashes, nullptr);
  EXPECT_EQ(memcmp(t->block_hashes, SHA1_20, 20), 0);
  EXPECT_EQ(memcmp(t->block_hashes + 20, SHA2_20, 20), 0);
  xdl_torrent_destroy(t);
}

TEST(Torrent, ParseWithHttpUrls) {
  const char *urls[] = {"https://cdn1.example.com/file", "https://cdn2.example.com/file"};
  auto data = make_torrent("x", 100, 256, NULL, 0, NULL, urls, 2);
  auto *t = xdl_torrent_parse(reinterpret_cast<const uint8_t *>(data.data()), data.size());
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->http_url_count, 2);
  EXPECT_STREQ(t->http_urls[0], "https://cdn1.example.com/file");
  EXPECT_STREQ(t->http_urls[1], "https://cdn2.example.com/file");
  xdl_torrent_destroy(t);
}

TEST(Torrent, ParseSingleHttpUrlString) {
  std::string data = "d4:infod4:name1:x6:lengthi100e8:url-list28:https://cdn.example.com/fileee";
  auto *t = xdl_torrent_parse(reinterpret_cast<const uint8_t *>(data.data()), data.size());
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->http_url_count, 1);
  EXPECT_STREQ(t->http_urls[0], "https://cdn.example.com/file");
  xdl_torrent_destroy(t);
}

TEST(Torrent, DefaultBlockLength) {
  std::string data = "d4:infod4:name1:x6:lengthi1048576eee";
  auto *t = xdl_torrent_parse(reinterpret_cast<const uint8_t *>(data.data()), data.size());
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->block_length, static_cast<uint32_t>(262144));
  EXPECT_EQ(t->block_count, static_cast<uint32_t>(4));
  xdl_torrent_destroy(t);
}

TEST(Torrent, NoBlockHashes) {
  auto data = make_torrent("x", 300000, 262144, NULL, 0, NULL, NULL, 0);
  auto *t = xdl_torrent_parse(reinterpret_cast<const uint8_t *>(data.data()), data.size());
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->block_count, static_cast<uint32_t>(2));
  EXPECT_EQ(t->block_hashes, nullptr);
  xdl_torrent_destroy(t);
}

TEST(Torrent, ParseNull) { EXPECT_EQ(xdl_torrent_parse(NULL, 0), nullptr); }
TEST(Torrent, ParseEmpty) { EXPECT_EQ(xdl_torrent_parse(reinterpret_cast<const uint8_t *>(""), 0), nullptr); }
TEST(Torrent, ParseInvalid) { EXPECT_EQ(xdl_torrent_parse(reinterpret_cast<const uint8_t *>("not bencoded"), 11), nullptr); }
TEST(Torrent, ParseNotDict) { EXPECT_EQ(xdl_torrent_parse(reinterpret_cast<const uint8_t *>("i42e"), 4), nullptr); }

TEST(Torrent, ParseMissingInfo) {
  EXPECT_EQ(xdl_torrent_parse(reinterpret_cast<const uint8_t *>("d4:name4:teste"), 14), nullptr);
}

TEST(Torrent, DestroyNullSafe) { xdl_torrent_destroy(NULL); }
