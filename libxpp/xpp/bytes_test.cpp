/*
 * Unit tests for xpp::Bytes — refcounted immutable byte block.
 */

#include <cstring>
#include <gtest/gtest.h>
#include <xpp/bytes.h>
#include <xpp/string.h>

using namespace xpp;

/* ───────────────────────────────────────────────────────────────────
 *  Construction
 * ─────────────────────────────────────────────────────────────────── */

TEST(BytesConstruction, Default) {
  Bytes b;
  EXPECT_TRUE(b.empty());
  EXPECT_EQ(b.size(), 0u);
  EXPECT_EQ(b.data(), nullptr);
}

TEST(BytesConstruction, FromVec) {
  Vec<uint8_t> v;
  v.push(0x01);
  v.push(0x02);
  v.push(0x03);
  Bytes b = Bytes::from(std::move(v));
  EXPECT_FALSE(b.empty());
  EXPECT_EQ(b.size(), 3u);
  EXPECT_NE(b.data(), nullptr);
  EXPECT_EQ(b[0], 0x01);
  EXPECT_EQ(b[1], 0x02);
  EXPECT_EQ(b[2], 0x03);
}

TEST(BytesConstruction, FromEmptyVec) {
  Vec<uint8_t> v;
  Bytes b = Bytes::from(std::move(v));
  EXPECT_TRUE(b.empty());
  EXPECT_EQ(b.size(), 0u);
}

TEST(BytesConstruction, FromCString) {
  Bytes b = Bytes::from("hello");
  EXPECT_EQ(b.size(), 5u);
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(b.data()), b.size()), "hello");
}

TEST(BytesConstruction, FromNullptrCString) {
  // Should panic in debug builds; just exercise the assertion path.
  EXPECT_DEATH({ Bytes::from(static_cast<const char*>(nullptr)); }, "");
}

TEST(BytesConstruction, FromDataLen) {
  const uint8_t data[] = {0xde, 0xad, 0xbe, 0xef};
  Bytes b = Bytes::from(data, 4);
  EXPECT_EQ(b.size(), 4u);
  EXPECT_EQ(b[0], 0xde);
  EXPECT_EQ(b[3], 0xef);
}

TEST(BytesConstruction, FromString) {
  auto r = String::from_utf8("hello");
  ASSERT_TRUE(r.is_ok());
  Bytes b = Bytes::from(r.unwrap());
  EXPECT_EQ(b.size(), 5u);
}

TEST(BytesConstruction, Empty) {
  Bytes b = Bytes::make_empty();
  EXPECT_TRUE(b.empty());
  EXPECT_EQ(b.size(), 0u);
  EXPECT_EQ(b.data(), nullptr);
}

/* ───────────────────────────────────────────────────────────────────
 *  Copy semantics (refcount, no buffer copy)
 * ─────────────────────────────────────────────────────────────────── */

TEST(BytesCopy, CopySharesBuffer) {
  Vec<uint8_t> v;
  v.push(0x01);
  v.push(0x02);
  v.push(0x03);
  Bytes a = Bytes::from(std::move(v));

  Bytes b = a;  // copy
  EXPECT_EQ(b.size(), a.size());
  EXPECT_EQ(b.data(), a.data());  // same pointer — no buffer copy
}

TEST(BytesCopy, CopyIsCheap) {
  // Make a large Bytes, copy it — the pointer must be identical.
  Vec<uint8_t> v;
  for (int i = 0; i < 1024; ++i) v.push(static_cast<uint8_t>(i & 0xff));
  Bytes a = Bytes::from(std::move(v));

  Bytes b = a;
  Bytes c = b;
  EXPECT_EQ(a.data(), c.data());
  EXPECT_EQ(a.size(), c.size());
}

TEST(BytesCopy, Assignment) {
  Vec<uint8_t> v;
  v.push(0xaa);
  Bytes a = Bytes::from(std::move(v));

  Bytes b;
  b = a;
  EXPECT_EQ(b.size(), 1u);
  EXPECT_EQ(b.data(), a.data());
  EXPECT_EQ(b[0], 0xaa);
}

/* ───────────────────────────────────────────────────────────────────
 *  Move semantics
 * ─────────────────────────────────────────────────────────────────── */

TEST(BytesMove, MoveSteals) {
  Vec<uint8_t> v;
  v.push(0x01);
  v.push(0x02);
  Bytes a = Bytes::from(std::move(v));

  const uint8_t* original_ptr = a.data();
  Bytes b = std::move(a);

  EXPECT_EQ(b.data(), original_ptr);
  EXPECT_EQ(b.size(), 2u);
  // a is moved-from — don't inspect its contents, but destroying it
  // must not crash or double-free.
}

TEST(BytesMove, MoveAssignment) {
  Vec<uint8_t> v1;
  v1.push(0x11);
  Bytes a = Bytes::from(std::move(v1));

  Vec<uint8_t> v2;
  v2.push(0x22);
  v2.push(0x33);
  Bytes b = Bytes::from(std::move(v2));

  const uint8_t* b_ptr = b.data();
  a = std::move(b);
  EXPECT_EQ(a.data(), b_ptr);
  EXPECT_EQ(a.size(), 2u);
}

/* ───────────────────────────────────────────────────────────────────
 *  Slicing (zero-copy)
 * ─────────────────────────────────────────────────────────────────── */

TEST(BytesSlice, SliceSharesBuffer) {
  Vec<uint8_t> v;
  for (int i = 0; i < 16; ++i) v.push(static_cast<uint8_t>(i));
  Bytes a = Bytes::from(std::move(v));

  Bytes b = a.slice(4, 8);
  EXPECT_EQ(b.size(), 8u);
  EXPECT_EQ(b.data(), a.data() + 4);  // points into a's buffer
  EXPECT_EQ(b[0], 0x04);
  EXPECT_EQ(b[7], 0x0b);
}

TEST(BytesSlice, SliceFromOffset) {
  Vec<uint8_t> v;
  for (int i = 0; i < 8; ++i) v.push(static_cast<uint8_t>(i));
  Bytes a = Bytes::from(std::move(v));

  Bytes b = a.slice_from(3);
  EXPECT_EQ(b.size(), 5u);
  EXPECT_EQ(b[0], 0x03);
  EXPECT_EQ(b[4], 0x07);
}

TEST(BytesSlice, SliceEntire) {
  Vec<uint8_t> v;
  v.push(0xaa);
  v.push(0xbb);
  Bytes a = Bytes::from(std::move(v));

  Bytes b = a.slice(0, a.size());
  EXPECT_EQ(b.data(), a.data());
  EXPECT_EQ(b.size(), a.size());
}

TEST(BytesSlice, SliceZeroLen) {
  Vec<uint8_t> v;
  v.push(0xaa);
  Bytes a = Bytes::from(std::move(v));

  Bytes b = a.slice(0, 0);
  EXPECT_EQ(b.size(), 0u);
  // It's fine for data() to be non-null here — we only guarantee size == 0.
}

TEST(BytesSlice, SliceFromEmpty) {
  Bytes a;
  Bytes b = a.slice(0, 0);
  EXPECT_EQ(b.size(), 0u);
}

TEST(BytesSlice, SlicedBytesHasItsOwnOffsetLen) {
  Vec<uint8_t> v;
  v.push(0x01);
  v.push(0x02);
  v.push(0x03);
  v.push(0x04);
  Bytes a = Bytes::from(std::move(v));

  Bytes b = a.slice(1, 2);
  ASSERT_EQ(b.size(), 2u);
  EXPECT_EQ(b[0], 0x02);
  EXPECT_EQ(b[1], 0x03);

  // Now slice the slice — offset must accumulate correctly.
  Bytes c = b.slice(1, 1);
  ASSERT_EQ(c.size(), 1u);
  EXPECT_EQ(c[0], 0x03);
  EXPECT_EQ(c.data(), a.data() + 2);  // original buffer offset 2
}

/* ───────────────────────────────────────────────────────────────────
 *  to_vec (copy out)
 * ─────────────────────────────────────────────────────────────────── */

TEST(BytesToVec, CopiesIntoIndependentVec) {
  Vec<uint8_t> v;
  v.push(0x01);
  v.push(0x02);
  v.push(0x03);
  Bytes a = Bytes::from(std::move(v));

  Vec<uint8_t> copy = a.to_vec();
  ASSERT_EQ(copy.len(), 3u);
  EXPECT_EQ(copy[0], 0x01);
  EXPECT_EQ(copy[1], 0x02);
  EXPECT_EQ(copy[2], 0x03);

  // Mutating the Vec must not affect a (already true since Bytes is immutable,
  // but verify data pointer differs).
  EXPECT_NE(copy.data(), a.data());
}

TEST(BytesToVec, SlicedBytesToVecCopiesOnlySlice) {
  Vec<uint8_t> v;
  for (int i = 0; i < 16; ++i) v.push(static_cast<uint8_t>(i));
  Bytes a = Bytes::from(std::move(v));

  Bytes b = a.slice(4, 3);
  Vec<uint8_t> copy = b.to_vec();
  ASSERT_EQ(copy.len(), 3u);
  EXPECT_EQ(copy[0], 0x04);
  EXPECT_EQ(copy[1], 0x05);
  EXPECT_EQ(copy[2], 0x06);
}

TEST(BytesToVec, EmptyToVec) {
  Bytes a;
  Vec<uint8_t> v = a.to_vec();
  EXPECT_EQ(v.len(), 0u);
}

/* ───────────────────────────────────────────────────────────────────
 *  to_string / to_string_lossy
 * ─────────────────────────────────────────────────────────────────── */

TEST(BytesToString, ValidUtf8) {
  Bytes b = Bytes::from("hello");
  auto r = b.to_string();
  ASSERT_TRUE(r.is_ok());
  EXPECT_EQ(r.unwrap(), String::from_utf8("hello").unwrap());
}

TEST(BytesToString, InvalidUtf8ReturnsErr) {
  Vec<uint8_t> v;
  v.push(0xff);
  v.push(0xfe);
  Bytes b = Bytes::from(std::move(v));
  auto r = b.to_string();
  EXPECT_TRUE(r.is_err());
}

TEST(BytesToString, EmptyBytesToString) {
  Bytes b;
  auto r = b.to_string();
  ASSERT_TRUE(r.is_ok());
  EXPECT_TRUE(r.unwrap().empty());
}

TEST(BytesToStringLossy, ValidUtf8Unchanged) {
  Bytes b = Bytes::from("hello");
  String s = b.to_string_lossy();
  EXPECT_EQ(s, String::from_utf8("hello").unwrap());
}

TEST(BytesToStringLossy, InvalidBytesReplacedWithReplacementChar) {
  Vec<uint8_t> v;
  v.push(0x68);  // 'h'
  v.push(0x69);  // 'i'
  v.push(0xff);  // invalid
  Bytes b = Bytes::from(std::move(v));
  String s = b.to_string_lossy();

  // Should start with "hi" then contain U+FFFD.
  EXPECT_GE(s.len(), 5u);  // "hi" (2 bytes) + U+FFFD (3 bytes) = 5
  auto expected = String::from_utf8("hi\xef\xbf\xbd").unwrap();
  EXPECT_EQ(s, expected);
}

TEST(BytesToStringLossy, EmptyBytes) {
  Bytes b;
  String s = b.to_string_lossy();
  EXPECT_TRUE(s.empty());
}

TEST(BytesToStringLossy, MultipleInvalidBytes) {
  Vec<uint8_t> v;
  v.push(0xff);
  v.push(0xfe);
  v.push(0xfd);
  Bytes b = Bytes::from(std::move(v));
  String s = b.to_string_lossy();
  // 3 invalid bytes → 3 replacement chars = 3 * 3 = 9 bytes
  EXPECT_EQ(s.len(), 9u);
}

TEST(BytesToStringLossy, ChineseUtf8Preserved) {
  const char* chinese = "你好世界";
  Bytes b = Bytes::from(chinese);
  String s = b.to_string_lossy();
  EXPECT_EQ(s, String::from_utf8(chinese).unwrap());
}

/* ───────────────────────────────────────────────────────────────────
 *  Iteration
 * ─────────────────────────────────────────────────────────────────── */

TEST(BytesIter, BeginEnd) {
  Vec<uint8_t> v;
  v.push(0x01);
  v.push(0x02);
  v.push(0x03);
  Bytes b = Bytes::from(std::move(v));

  Vec<uint8_t> collected;
  for (uint8_t x : b) collected.push(x);
  ASSERT_EQ(collected.len(), 3u);
  EXPECT_EQ(collected[0], 0x01);
  EXPECT_EQ(collected[1], 0x02);
  EXPECT_EQ(collected[2], 0x03);
}

TEST(BytesIter, EmptyBeginEnd) {
  Bytes b;
  EXPECT_EQ(b.begin(), b.end());
}

/* ───────────────────────────────────────────────────────────────────
 *  Comparison
 * ─────────────────────────────────────────────────────────────────── */

TEST(BytesCompare, EqualBytes) {
  Bytes a = Bytes::from("hello");
  Bytes b = Bytes::from("hello");
  EXPECT_TRUE(a == b);
  EXPECT_FALSE(a != b);
}

TEST(BytesCompare, DifferentSize) {
  Bytes a = Bytes::from("hello");
  Bytes b = Bytes::from("hi");
  EXPECT_FALSE(a == b);
  EXPECT_TRUE(a != b);
}

TEST(BytesCompare, SameSizeDifferentContent) {
  Bytes a = Bytes::from("hello");
  Bytes b = Bytes::from("world");
  EXPECT_FALSE(a == b);
}

TEST(BytesCompare, EmptyEqual) {
  Bytes a;
  Bytes b;
  EXPECT_TRUE(a == b);
}

/* ───────────────────────────────────────────────────────────────────
 *  as_span
 * ─────────────────────────────────────────────────────────────────── */

TEST(BytesSpan, AsSpan) {
  Vec<uint8_t> v;
  v.push(0x01);
  v.push(0x02);
  v.push(0x03);
  Bytes b = Bytes::from(std::move(v));

  Span<const uint8_t> s = b.as_span();
  EXPECT_EQ(s.size(), 3u);
  EXPECT_EQ(s.data(), b.data());
  EXPECT_EQ(s[0], 0x01);
  EXPECT_EQ(s[2], 0x03);
}

TEST(BytesSpan, EmptyAsSpan) {
  Bytes b;
  Span<const uint8_t> s = b.as_span();
  EXPECT_EQ(s.size(), 0u);
}
