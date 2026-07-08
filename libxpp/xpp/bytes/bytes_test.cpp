/*
 * bytes_test.cpp — Tests for xpp::bytes::Bytes.
 */
#include <gtest/gtest.h>
#include <xpp/bytes/bytes.h>

using namespace xpp::bytes;

TEST(BytesTest, DefaultEmpty) {
  Bytes b;
  EXPECT_TRUE(b.empty());
  EXPECT_EQ(b.size(), 0u);
  EXPECT_EQ(b.data(), nullptr);
}

TEST(BytesTest, FromVector) {
  std::vector<uint8_t> v = {0x01, 0x02, 0x03, 0x04};
  auto                 b = Bytes::from(std::move(v));
  EXPECT_EQ(b.size(), 4u);
  EXPECT_FALSE(b.empty());
  EXPECT_EQ(b[0], 0x01);
  EXPECT_EQ(b[1], 0x02);
  EXPECT_EQ(b[2], 0x03);
  EXPECT_EQ(b[3], 0x04);
}

TEST(BytesTest, SliceNormal) {
  std::vector<uint8_t> v = {10, 20, 30, 40, 50};
  auto                 b = Bytes::from(std::move(v));
  auto                 s = b.slice(1, 4);
  EXPECT_EQ(s.size(), 3u);
  EXPECT_EQ(s[0], 20);
  EXPECT_EQ(s[1], 30);
  EXPECT_EQ(s[2], 40);
}

TEST(BytesTest, SliceEmpty) {
  auto b = Bytes::from(std::vector<uint8_t>{1, 2, 3});
  auto s = b.slice(1, 1);
  EXPECT_TRUE(s.empty());
}

TEST(BytesTest, SliceFull) {
  auto b = Bytes::from(std::vector<uint8_t>{1, 2, 3});
  auto s = b.slice(0, 3);
  EXPECT_EQ(s.size(), 3u);
}

TEST(BytesTest, SliceChained) {
  auto b  = Bytes::from(std::vector<uint8_t>{0, 1, 2, 3, 4, 5, 6, 7});
  auto s1 = b.slice(1, 7);
  auto s2 = s1.slice(2, 5);
  EXPECT_EQ(s2.size(), 3u);
  EXPECT_EQ(s2[0], 3);
  EXPECT_EQ(s2[1], 4);
  EXPECT_EQ(s2[2], 5);
}

TEST(BytesTest, ToString) {
  auto b = Bytes::from(std::vector<uint8_t>{'h', 'e', 'l', 'l', 'o'});
  EXPECT_EQ(b.to_string(), "hello");
}

TEST(BytesTest, StringView) {
  auto             b  = Bytes::from(std::vector<uint8_t>{'w', 'o', 'r', 'l', 'd'});
  std::string_view sv = static_cast<std::string_view>(b);
  EXPECT_EQ(sv, "world");
}
