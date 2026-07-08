/*
 * bytes_mut_test.cpp — Tests for xpp::bytes::BytesMut.
 */
#include <gtest/gtest.h>
#include <xpp/bytes/bytes.h>
#include <xpp/bytes/bytes_mut.h>

using namespace xpp::bytes;

TEST(BytesMutTest, DefaultEmpty) {
  BytesMut b;
  EXPECT_TRUE(b.empty());
  EXPECT_EQ(b.size(), 0u);
}

TEST(BytesMutTest, PutAppend) {
  auto b = BytesMut::with_capacity(64);
  EXPECT_EQ(b.size(), 0u);

  const uint8_t data[] = {1, 2, 3};
  b.put(data, 3);
  EXPECT_EQ(b.size(), 3u);
  EXPECT_EQ(b[0], 1);
  EXPECT_EQ(b[1], 2);
  EXPECT_EQ(b[2], 3);

  b.put(data, 2);
  EXPECT_EQ(b.size(), 5u);
  EXPECT_EQ(b[3], 1);
  EXPECT_EQ(b[4], 2);
}

TEST(BytesMutTest, PutStringView) {
  auto b = BytesMut::with_capacity(64);
  b.put(std::string_view("hello"));
  EXPECT_EQ(b.size(), 5u);
  EXPECT_EQ(b.to_string(), "hello");
}

TEST(BytesMutTest, Freeze) {
  auto          b      = BytesMut::with_capacity(64);
  const uint8_t data[] = {10, 20, 30};
  b.put(data, 3);

  auto frozen = b.freeze();
  EXPECT_EQ(frozen.size(), 3u);
  EXPECT_EQ(frozen[0], 10);
  EXPECT_EQ(frozen[1], 20);
  EXPECT_EQ(frozen[2], 30);
}

TEST(BytesMutTest, FreezeThenSlice) {
  auto          b      = BytesMut::with_capacity(64);
  const uint8_t data[] = {1, 2, 3, 4};
  b.put(data, 4);

  auto frozen = b.freeze();
  auto s      = frozen.slice(1, 3);
  EXPECT_EQ(s.size(), 2u);
  EXPECT_EQ(s[0], 2);
  EXPECT_EQ(s[1], 3);
}
