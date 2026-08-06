/*
 * Unit tests for xpp::String — UTF-8 String.
 */

#include <xpp/str.h>
#include <gtest/gtest.h>

using namespace xpp;

/* ───────────────────────────────────────────────────────────────────
 *  Construction
 * ─────────────────────────────────────────────────────────────────── */

TEST(StringConstruction, Default) {
    String s;
    EXPECT_EQ(s.len(), 0u);
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.capacity(), 0u);
}

TEST(StringConstruction, WithCapacity) {
    String s(16);
    EXPECT_EQ(s.len(), 0u);
    EXPECT_GE(s.capacity(), 16u);
}

TEST(StringConstruction, Copy) {
    auto r = String::from_utf8("hello");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    String copy(s);
    EXPECT_EQ(copy.len(), 5u);
    EXPECT_EQ(copy, s);
    // independent — modifying one doesn't affect the other
    copy.push('!');
    EXPECT_EQ(copy.len(), 6u);
    EXPECT_EQ(s.len(), 5u);
}

TEST(StringConstruction, Move) {
    auto r = String::from_utf8("hello");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    String moved(std::move(s));
    EXPECT_EQ(moved.len(), 5u);
    EXPECT_EQ(moved, String::from_utf8("hello").unwrap());
    // source is empty
    EXPECT_EQ(s.len(), 0u);
}

TEST(StringConstruction, CopyAssignment) {
    auto r1 = String::from_utf8("hello");
    auto r2 = String::from_utf8("world");
    ASSERT_TRUE(r1.is_ok() && r2.is_ok());
    String a = r1.unwrap();
    String b = r2.unwrap();
    a = b;
    EXPECT_EQ(a.len(), 5u);
    EXPECT_EQ(a, b);
}

TEST(StringConstruction, MoveAssignment) {
    auto r1 = String::from_utf8("hello");
    auto r2 = String::from_utf8("world");
    ASSERT_TRUE(r1.is_ok() && r2.is_ok());
    String a = r1.unwrap();
    String b = r2.unwrap();
    a = std::move(b);
    EXPECT_EQ(a.len(), 5u);
    EXPECT_EQ(b.len(), 0u);
}

TEST(StringConstruction, SelfAssignment) {
    auto r = String::from_utf8("hello");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    s = s; // self-assignment
    EXPECT_EQ(s.len(), 5u);
}

/* ───────────────────────────────────────────────────────────────────
 *  from_utf8 — validation
 * ─────────────────────────────────────────────────────────────────── */

TEST(StringFromUtf8, EmptyString) {
    auto r = String::from_utf8("");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    EXPECT_EQ(s.len(), 0u);
    EXPECT_TRUE(s.empty());
}

TEST(StringFromUtf8, AsciiString) {
    auto r = String::from_utf8("hello world");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    EXPECT_EQ(s.len(), std::strlen("hello world"));
    EXPECT_TRUE(s == "hello world");
}

TEST(StringFromUtf8, ChineseUTF8) {
    auto r = String::from_utf8("你好世界");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    EXPECT_EQ(s.len(), 12u); // 4 * 3 bytes
    EXPECT_EQ(s.char_len(), 4u);
}

TEST(StringFromUtf8, MixedAsciiUTF8) {
    auto r = String::from_utf8("hello 世界 !");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    EXPECT_GT(s.len(), 0u);
}

TEST(StringFromUtf8, NullByte) {
    auto r = String::from_utf8("hello\0world", 11);
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    EXPECT_EQ(s.len(), 11u);
    EXPECT_EQ(s.as_bytes()[5], 0u);
}

TEST(StringFromUtf8, OverlongEncoding) {
    std::string bad = "\xC0\x80"; // overlong encoding of U+0000
    auto r = String::from_utf8(bad.c_str(), bad.size());
    EXPECT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err().error_pos(), 0u);
}

TEST(StringFromUtf8, SurrogateHalf) {
    std::string bad = "\xED\xA0\x80"; // U+D800 surrogate
    auto r = String::from_utf8(bad.c_str(), bad.size());
    EXPECT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err().error_pos(), 0u);
}

TEST(StringFromUtf8, BeyondUnicode) {
    std::string bad = "\xF4\x90\x80\x80"; // > U+10FFFF
    auto r = String::from_utf8(bad.c_str(), bad.size());
    EXPECT_TRUE(r.is_err());
}

TEST(StringFromUtf8, TruncatedSequence) {
    // 2-byte lead without continuation
    std::string bad = "\xC2";
    auto r = String::from_utf8(bad.c_str(), bad.size());
    EXPECT_TRUE(r.is_err());
}

TEST(StringFromUtf8, InvalidContinuation) {
    // 2-byte lead followed by non-continuation
    std::string bad = "\xC2\xFF";
    auto r = String::from_utf8(bad.c_str(), bad.size());
    EXPECT_TRUE(r.is_err());
    EXPECT_EQ(r.unwrap_err().error_pos(), 0u);
}

TEST(StringFromUtf8, Emoji) {
    auto r = String::from_utf8("🎉🚀");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    EXPECT_EQ(s.char_len(), 2u);
    EXPECT_EQ(s.len(), 8u); // 2 * 4 bytes
}

TEST(StringFromUtf8, ThreeByteChar) {
    // U+4F60 (你)
    std::string v = "\xE4\xBD\xA0";
    auto r = String::from_utf8(v.c_str(), v.size());
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    EXPECT_EQ(s.len(), 3u);
    EXPECT_EQ(s.char_len(), 1u);
}

TEST(StringFromUtf8, UncheckedConstruction) {
    Vec<uint8_t> bytes(11);
    const char* data = "hello world";
    for (size_t i = 0; i < 11; i++) bytes.push(static_cast<uint8_t>(data[i]));
    String s = String::from_utf8_unchecked(std::move(bytes));
    EXPECT_EQ(s.len(), 11u);
}

/* ───────────────────────────────────────────────────────────────────
 *  Utf8Error recovery
 * ─────────────────────────────────────────────────────────────────── */

TEST(StringUtf8Error, RecoverBytes) {
    std::string bad = "\xC0\x80";
    auto r = String::from_utf8(bad.c_str(), bad.size());
    ASSERT_TRUE(r.is_err());
    Utf8Error err = r.unwrap_err();
    EXPECT_EQ(err.error_pos(), 0u);
    Vec<uint8_t> recovered = std::move(err).into_bytes();
    EXPECT_EQ(recovered.len(), 2u);
    EXPECT_EQ(recovered[0], static_cast<uint8_t>('\xC0'));
    EXPECT_EQ(recovered[1], static_cast<uint8_t>('\x80'));
}

/* ───────────────────────────────────────────────────────────────────
 *  Views and conversions
 * ─────────────────────────────────────────────────────────────────── */

TEST(StringViews, AsBytes) {
    auto r = String::from_utf8("hello");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    Span<const uint8_t> bytes = s.as_bytes();
    EXPECT_EQ(bytes.size(), 5u);
    EXPECT_EQ(bytes[0], static_cast<uint8_t>('h'));
}

TEST(StringViews, IntoBytes) {
    auto r = String::from_utf8("hello");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    Vec<uint8_t> bytes = std::move(s).into_bytes();
    EXPECT_EQ(bytes.len(), 5u);
    EXPECT_EQ(bytes[0], static_cast<uint8_t>('h'));
}

/* ───────────────────────────────────────────────────────────────────
 *  Length / Capacity
 * ─────────────────────────────────────────────────────────────────── */

TEST(StringLength, LenAndEmpty) {
    String s(10);
    EXPECT_EQ(s.len(), 0u);
    EXPECT_TRUE(s.empty());
    s.push('a');
    EXPECT_EQ(s.len(), 1u);
    EXPECT_FALSE(s.empty());
}

TEST(StringLength, CharLen) {
    auto r = String::from_utf8("a\xE4\xBD\xA0" "b"); // a + 你 + b
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    EXPECT_EQ(s.char_len(), 3u);
}

TEST(StringLength, CharLenEmpty) {
    String s;
    EXPECT_EQ(s.char_len(), 0u);
}

TEST(StringLength, CharLenEmoji) {
    auto r = String::from_utf8("🎉🎉🎉");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    EXPECT_EQ(s.char_len(), 3u);
}

TEST(StringCapacity, Reserve) {
    String s;
    s.reserve(100);
    EXPECT_GE(s.capacity(), 100u);
    EXPECT_EQ(s.len(), 0u);
}

TEST(StringCapacity, TryReserve) {
    String s;
    auto r = s.try_reserve(100);
    EXPECT_TRUE(r.is_ok());
    EXPECT_GE(s.capacity(), 100u);
}

TEST(StringCapacity, ShrinkToFit) {
    String s(100);
    s.push('a');
    s.shrink_to_fit();
    EXPECT_LE(s.capacity(), s.len() + 4u); // small slack allowed
}

TEST(StringCapacity, TryShrinkToFit) {
    String s(100);
    s.push('a');
    auto r = s.try_shrink_to_fit();
    EXPECT_TRUE(r.is_ok());
    EXPECT_LE(s.capacity(), s.len() + 4u);
}

/* ───────────────────────────────────────────────────────────────────
 *  Substring
 * ─────────────────────────────────────────────────────────────────── */

TEST(StringSubstr, FullRange) {
    auto r = String::from_utf8("你好世界");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    String sub = s.substr(0); // all
    EXPECT_EQ(sub, s);
}

TEST(StringSubstr, ChineseCharacter) {
    auto r = String::from_utf8("你好");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    // "你好": byte [0,3) = "你", [3,6) = "好"
    String sub = s.substr(0, 3);
    EXPECT_EQ(sub, String::from_utf8("你").unwrap());
}

TEST(StringSubstr, SecondCharacter) {
    auto r = String::from_utf8("你好");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    String sub = s.substr(3, 3); // "好"
    EXPECT_EQ(sub, String::from_utf8("好").unwrap());
}

TEST(StringSubstr, Empty) {
    auto r = String::from_utf8("hello");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    String sub = s.substr(0, 0);
    EXPECT_EQ(sub.len(), 0u);
    EXPECT_TRUE(sub.empty());
}

TEST(StringSubstr, CountMax) {
    auto r = String::from_utf8("hello");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    String sub = s.substr(2); // SIZE_MAX → to end
    EXPECT_EQ(sub.len(), 3u);
}

/* ───────────────────────────────────────────────────────────────────
 *  Find / Contains / StartsWith / EndsWith
 * ─────────────────────────────────────────────────────────────────── */

TEST(StringFind, Basic) {
    auto r = String::from_utf8("hello world");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    auto found = s.find(String::from_utf8("world").unwrap());
    ASSERT_TRUE(found.is_some());
    EXPECT_EQ(found.unwrap(), 6u);
}

TEST(StringFind, CString) {
    auto r = String::from_utf8("hello world");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    auto found = s.find("world");
    ASSERT_TRUE(found.is_some());
    EXPECT_EQ(found.unwrap(), 6u);
}

TEST(StringFind, NotFound) {
    auto r = String::from_utf8("hello world");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    auto found = s.find("xyz");
    EXPECT_TRUE(found.is_none());
}

TEST(StringFind, Chinese) {
    auto r = String::from_utf8("你好世界");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    auto found = s.find(String::from_utf8("世界").unwrap());
    ASSERT_TRUE(found.is_some());
    EXPECT_EQ(found.unwrap(), 6u); // 你=3, 好=3, 世 starts at 6
}

TEST(StringFind, Rfind) {
    auto r = String::from_utf8("ababa");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    auto found = s.rfind("ba");
    ASSERT_TRUE(found.is_some());
    EXPECT_EQ(found.unwrap(), 3u); // last "ba" at pos 3 (positions: a0 b1 a2 b3 a4)
}

TEST(StringFind, RfindNotFound) {
    auto r = String::from_utf8("hello");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    auto found = s.rfind("xyz");
    EXPECT_TRUE(found.is_none());
}

TEST(StringFind, Contains) {
    auto r = String::from_utf8("hello world");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    EXPECT_TRUE(s.contains("world"));
    EXPECT_FALSE(s.contains("xyz"));
}

TEST(StringFind, StartsWith) {
    auto r = String::from_utf8("hello world");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    EXPECT_TRUE(s.starts_with("hello"));
    EXPECT_FALSE(s.starts_with("world"));
}

TEST(StringFind, EndsWith) {
    auto r = String::from_utf8("hello world");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    EXPECT_TRUE(s.ends_with("world"));
    EXPECT_FALSE(s.ends_with("hello"));
}

TEST(StringFind, StartsWithString) {
    auto r = String::from_utf8("hello world");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    EXPECT_TRUE(s.starts_with(String::from_utf8("hello").unwrap()));
}

TEST(StringFind, EndsWithString) {
    auto r = String::from_utf8("hello world");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    EXPECT_TRUE(s.ends_with(String::from_utf8("world").unwrap()));
}

TEST(StringFind, StartsWithLonger) {
    auto r = String::from_utf8("hi");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    EXPECT_FALSE(s.starts_with("hello")); // prefix longer than string
}

TEST(StringFind, EndsWithLonger) {
    auto r = String::from_utf8("hi");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    EXPECT_FALSE(s.ends_with("hello")); // suffix longer than string
}

/* ───────────────────────────────────────────────────────────────────
 *  Mutation — push / push_str / pop
 * ─────────────────────────────────────────────────────────────────── */

TEST(StringMutation, PushAscii) {
    String s;
    s.push('a');
    s.push('b');
    s.push('c');
    EXPECT_EQ(s.len(), 3u);
    EXPECT_TRUE(s == "abc");
}

TEST(StringMutation, PushChinese) {
    String s;
    s.push(0x4F60); // 你
    s.push(0x597D); // 好
    EXPECT_EQ(s.len(), 6u);
    EXPECT_EQ(s.char_len(), 2u);
    EXPECT_TRUE(s == String::from_utf8("你好").unwrap());
}

TEST(StringMutation, PushEmoji) {
    String s;
    s.push(0x1F389); // 🎉
    EXPECT_EQ(s.len(), 4u);
    EXPECT_EQ(s.char_len(), 1u);
}

TEST(StringMutation, PushMaxCodePoint) {
    String s;
    s.push(0x10FFFF);
    EXPECT_EQ(s.len(), 4u);
}

TEST(StringMutation, PushStrString) {
    auto r1 = String::from_utf8("hello ");
    auto r2 = String::from_utf8("world");
    ASSERT_TRUE(r1.is_ok() && r2.is_ok());
    String s = r1.unwrap();
    s.push_str(r2.unwrap());
    EXPECT_EQ(s.len(), 11u);
    EXPECT_TRUE(s == String::from_utf8("hello world").unwrap());
}

TEST(StringMutation, TryPushStr) {
    auto r = String::from_utf8("world");
    ASSERT_TRUE(r.is_ok());
    String s(0); // no initial capacity
    auto res = s.try_push_str(r.unwrap());
    EXPECT_TRUE(res.is_ok());
    EXPECT_EQ(s.len(), 5u);
}

TEST(StringMutation, PushStrCString) {
    String s;
    s.push_str("hello");
    EXPECT_EQ(s.len(), 5u);
    EXPECT_TRUE(s == "hello");
}

TEST(StringMutation, Pop) {
    auto r = String::from_utf8("abc");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    auto cp = s.pop();
    ASSERT_TRUE(cp.is_some());
    EXPECT_EQ(cp.unwrap(), static_cast<char32_t>('c'));
    EXPECT_EQ(s.len(), 2u);
}

TEST(StringMutation, PopChinese) {
    auto r = String::from_utf8("你好");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    auto cp = s.pop();
    ASSERT_TRUE(cp.is_some());
    EXPECT_EQ(cp.unwrap(), static_cast<char32_t>(0x597D)); // 好
    EXPECT_EQ(s.len(), 3u); // 你 = 3 bytes
    auto cp2 = s.pop();
    ASSERT_TRUE(cp2.is_some());
    EXPECT_EQ(cp2.unwrap(), static_cast<char32_t>(0x4F60)); // 你
    EXPECT_EQ(s.len(), 0u);
}

TEST(StringMutation, PopEmpty) {
    String s;
    auto cp = s.pop();
    EXPECT_TRUE(cp.is_none());
}

TEST(StringMutation, PopSingleChar) {
    String s;
    s.push('a');
    auto cp = s.pop();
    ASSERT_TRUE(cp.is_some());
    EXPECT_EQ(cp.unwrap(), static_cast<char32_t>('a'));
    EXPECT_TRUE(s.empty());
    auto cp2 = s.pop();
    EXPECT_TRUE(cp2.is_none());
}

/* ───────────────────────────────────────────────────────────────────
 *  insert / insert_str / remove / truncate / clear
 * ─────────────────────────────────────────────────────────────────── */

TEST(StringMutation, InsertAtBeginning) {
    auto r = String::from_utf8("world");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    s.insert_str(0, String::from_utf8("hello ").unwrap());
    EXPECT_TRUE(s == "hello world");
}

TEST(StringMutation, InsertAtEnd) {
    auto r = String::from_utf8("hello ");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    s.insert_str(s.len(), String::from_utf8("world").unwrap());
    EXPECT_TRUE(s == "hello world");
}

TEST(StringMutation, InsertCodePoint) {
    String s;
    s.push('a');
    s.push('c');
    s.insert(1, 'b');
    EXPECT_EQ(s.len(), 3u);
    EXPECT_TRUE(s == "abc");
}

TEST(StringMutation, InsertCodePointChinese) {
    auto r = String::from_utf8("你好");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    s.insert(0, 0x597D); // 好 at front
    EXPECT_EQ(s, String::from_utf8("好你好").unwrap());
}

TEST(StringMutation, Remove) {
    auto r = String::from_utf8("abc");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    char32_t removed = s.remove(1);
    EXPECT_EQ(removed, static_cast<char32_t>('b'));
    EXPECT_EQ(s.len(), 2u);
    EXPECT_TRUE(s == String::from_utf8("ac").unwrap());
}

TEST(StringMutation, RemoveChinese) {
    auto r = String::from_utf8("你好世界");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    char32_t removed = s.remove(3); // 好
    EXPECT_EQ(removed, static_cast<char32_t>(0x597D));
    EXPECT_EQ(s, String::from_utf8("你世界").unwrap());
}

TEST(StringMutation, Truncate) {
    auto r = String::from_utf8("hello world");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    s.truncate(5); // "hello"
    EXPECT_EQ(s.len(), 5u);
    EXPECT_TRUE(s == "hello");
}

TEST(StringMutation, TruncateChinese) {
    auto r = String::from_utf8("你好世界");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    s.truncate(6); // 你好 (6 bytes)
    EXPECT_EQ(s, String::from_utf8("你好").unwrap());
}

TEST(StringMutation, Clear) {
    auto r = String::from_utf8("hello world");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    size_t cap = s.capacity();
    s.clear();
    EXPECT_EQ(s.len(), 0u);
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.capacity(), cap); // capacity preserved
}

/* ───────────────────────────────────────────────────────────────────
 *  split_off
 * ─────────────────────────────────────────────────────────────────── */

TEST(StringSplitOff, Middle) {
    auto r = String::from_utf8("hello world");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    String tail = s.split_off(6); // split after "hello "
    EXPECT_EQ(s, String::from_utf8("hello ").unwrap());
    EXPECT_EQ(tail, String::from_utf8("world").unwrap());
}

TEST(StringSplitOff, FromStart) {
    auto r = String::from_utf8("hello");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    String tail = s.split_off(0);
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(tail, String::from_utf8("hello").unwrap());
}

TEST(StringSplitOff, FromEnd) {
    auto r = String::from_utf8("hello");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    String tail = s.split_off(5);
    EXPECT_EQ(s, String::from_utf8("hello").unwrap());
    EXPECT_TRUE(tail.empty());
}

TEST(StringSplitOff, Chinese) {
    auto r = String::from_utf8("你好世界");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    String tail = s.split_off(6); // split after 你好 (6 bytes)
    EXPECT_EQ(s, String::from_utf8("你好").unwrap());
    EXPECT_EQ(tail, String::from_utf8("世界").unwrap());
}

/* ───────────────────────────────────────────────────────────────────
 *  replace / replacen
 * ─────────────────────────────────────────────────────────────────── */

TEST(StringReplace, Basic) {
    auto r = String::from_utf8("hello world");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    String result = s.replace(
        String::from_utf8("world").unwrap(),
        String::from_utf8("xpp").unwrap());
    EXPECT_EQ(result, String::from_utf8("hello xpp").unwrap());
}

TEST(StringReplace, MultipleOccurrences) {
    auto r = String::from_utf8("a a a");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    String result = s.replace(
        String::from_utf8("a").unwrap(),
        String::from_utf8("b").unwrap());
    EXPECT_EQ(result, String::from_utf8("b b b").unwrap());
}

TEST(StringReplace, NoMatch) {
    auto r = String::from_utf8("hello");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    String result = s.replace(
        String::from_utf8("xyz").unwrap(),
        String::from_utf8("abc").unwrap());
    EXPECT_EQ(result, s); // unchanged
}

TEST(StringReplace, EmptyFrom) {
    auto r = String::from_utf8("hello");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    String result = s.replace(
        String(),
        String::from_utf8("x").unwrap());
    EXPECT_EQ(result, s); // replacing empty pattern → no change
}

TEST(StringReplacen, Basic) {
    auto r = String::from_utf8("a a a");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    String result = s.replacen(
        String::from_utf8("a").unwrap(),
        String::from_utf8("b").unwrap(), 2);
    EXPECT_EQ(result, String::from_utf8("b b a").unwrap());
}

TEST(StringReplacen, Zero) {
    auto r = String::from_utf8("a a a");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    String result = s.replacen(
        String::from_utf8("a").unwrap(),
        String::from_utf8("b").unwrap(), 0);
    EXPECT_EQ(result, s);
}

/* ───────────────────────────────────────────────────────────────────
 *  repeat
 * ─────────────────────────────────────────────────────────────────── */

TEST(StringRepeat, Basic) {
    auto r = String::from_utf8("ab");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    String result = s.repeat(3);
    EXPECT_EQ(result, String::from_utf8("ababab").unwrap());
}

TEST(StringRepeat, Zero) {
    auto r = String::from_utf8("hello");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    String result = s.repeat(0);
    EXPECT_TRUE(result.empty());
}

TEST(StringRepeat, One) {
    auto r = String::from_utf8("hello");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    String result = s.repeat(1);
    EXPECT_EQ(result, s);
}

TEST(StringRepeat, Empty) {
    String s;
    String result = s.repeat(5);
    EXPECT_TRUE(result.empty());
}

/* ───────────────────────────────────────────────────────────────────
 *  trim
 * ─────────────────────────────────────────────────────────────────── */

TEST(StringTrim, BothSides) {
    auto r = String::from_utf8("  hello  ");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    EXPECT_EQ(s.trim(), String::from_utf8("hello").unwrap());
}

TEST(StringTrim, Start) {
    auto r = String::from_utf8("  hello");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    EXPECT_EQ(s.trim_start(), String::from_utf8("hello").unwrap());
}

TEST(StringTrim, End) {
    auto r = String::from_utf8("hello  ");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    EXPECT_EQ(s.trim_end(), String::from_utf8("hello").unwrap());
}

TEST(StringTrim, TabsAndNewlines) {
    auto r = String::from_utf8("\t\n hello \r\n");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    EXPECT_EQ(s.trim(), String::from_utf8("hello").unwrap());
}

TEST(StringTrim, AllWhitespace) {
    auto r = String::from_utf8("   ");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    String result = s.trim();
    EXPECT_TRUE(result.empty());
}

TEST(StringTrim, NoWhitespace) {
    auto r = String::from_utf8("hello");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    EXPECT_EQ(s.trim(), s);
}

/* ───────────────────────────────────────────────────────────────────
 *  retain
 * ─────────────────────────────────────────────────────────────────── */

TEST(StringRetain, KeepEven) {
    // "abc" → keep everything → "abc"
    auto r = String::from_utf8("abc");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    s.retain([](char32_t) { return true; });
    EXPECT_EQ(s, String::from_utf8("abc").unwrap());
}

TEST(StringRetain, KeepNone) {
    auto r = String::from_utf8("abc");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    s.retain([](char32_t) { return false; });
    EXPECT_TRUE(s.empty());
}

TEST(StringRetain, KeepAsciiLetters) {
    auto r = String::from_utf8("a1b2c3");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    s.retain([](char32_t cp) { return cp >= 'a' && cp <= 'z'; });
    EXPECT_EQ(s, String::from_utf8("abc").unwrap());
}

TEST(StringRetain, Chinese) {
    auto r = String::from_utf8("a你b好c");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    s.retain([](char32_t cp) { return cp > 127; });
    EXPECT_EQ(s, String::from_utf8("你好").unwrap());
}

/* ───────────────────────────────────────────────────────────────────
 *  Chars iterator
 * ─────────────────────────────────────────────────────────────────── */

TEST(StringChars, Empty) {
    String s;
    auto it = s.chars();
    EXPECT_EQ(it, it.end());
}

TEST(StringChars, AsciiIteration) {
    auto r = String::from_utf8("abc");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    char32_t expected[] = {'a', 'b', 'c'};
    int i = 0;
    for (char32_t cp : s.chars()) {
        EXPECT_EQ(cp, expected[i++]);
    }
    EXPECT_EQ(i, 3);
}

TEST(StringChars, ChineseIteration) {
    auto r = String::from_utf8("你好");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    char32_t expected[] = {0x4F60, 0x597D};
    int i = 0;
    for (char32_t cp : s.chars()) {
        EXPECT_EQ(cp, expected[i++]);
    }
    EXPECT_EQ(i, 2);
}

TEST(StringChars, ManualLoop) {
    auto r = String::from_utf8("ab");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    auto it = s.chars();
    auto end = it.end();
    EXPECT_EQ(*it, static_cast<char32_t>('a'));
    ++it;
    EXPECT_NE(it, end);
    EXPECT_EQ(*it, static_cast<char32_t>('b'));
    ++it;
    EXPECT_EQ(it, end);
}

TEST(StringChars, Count) {
    auto r = String::from_utf8("hello世界🎉");
    ASSERT_TRUE(r.is_ok());
    String s = r.unwrap();
    auto it = s.chars();
    EXPECT_EQ(it.count(), 8u); // hello(5) + 世界(2) + 🎉(1)
    // Iterator should still be at the start
    EXPECT_EQ(*it, static_cast<char32_t>('h'));
}

/* ───────────────────────────────────────────────────────────────────
 *  Comparison
 * ─────────────────────────────────────────────────────────────────── */

TEST(StringCompare, Equal) {
    auto r1 = String::from_utf8("hello");
    auto r2 = String::from_utf8("hello");
    ASSERT_TRUE(r1.is_ok() && r2.is_ok());
    EXPECT_EQ(r1.unwrap(), r2.unwrap());
}

TEST(StringCompare, NotEqual) {
    auto r1 = String::from_utf8("hello");
    auto r2 = String::from_utf8("world");
    ASSERT_TRUE(r1.is_ok() && r2.is_ok());
    EXPECT_NE(r1.unwrap(), r2.unwrap());
}

TEST(StringCompare, LessThan) {
    auto r1 = String::from_utf8("abc");
    auto r2 = String::from_utf8("abd");
    ASSERT_TRUE(r1.is_ok() && r2.is_ok());
    EXPECT_LT(r1.unwrap(), r2.unwrap());
}

TEST(StringCompare, LessThanShort) {
    auto r1 = String::from_utf8("ab");
    auto r2 = String::from_utf8("abc");
    ASSERT_TRUE(r1.is_ok() && r2.is_ok());
    EXPECT_LT(r1.unwrap(), r2.unwrap());
}

TEST(StringCompare, CStringEqual) {
    auto r = String::from_utf8("hello");
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.unwrap() == "hello");
    EXPECT_FALSE(r.unwrap() == "world");
}

TEST(StringCompare, CStringNotEqual) {
    auto r = String::from_utf8("hello");
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.unwrap() != "world");
    EXPECT_FALSE(r.unwrap() != "hello");
}

TEST(StringCompare, DecompositionInequality) {
    // "é" (precomposed U+00E9) vs "e\u0301" (decomposed)
    std::string precomposed = "\xC3\xA9";      // é (2 bytes)
    std::string decomposed  = "e\xCC\x81";      // e + combining acute (3 bytes)
    auto r1 = String::from_utf8(precomposed.c_str(), precomposed.size());
    auto r2 = String::from_utf8(decomposed.c_str(), decomposed.size());
    ASSERT_TRUE(r1.is_ok() && r2.is_ok());
    // Byte-wise they differ
    EXPECT_NE(r1.unwrap(), r2.unwrap());
}

/* ───────────────────────────────────────────────────────────────────
 *  Push vector integration
 * ─────────────────────────────────────────────────────────────────── */

TEST(StringVecIntegration, FromVecBytes) {
    Vec<uint8_t> bytes;
    const char* data = "hello";
    for (size_t i = 0; i < 5; i++) bytes.push(static_cast<uint8_t>(data[i]));
    auto r = String::from_utf8(std::move(bytes));
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.unwrap(), String::from_utf8("hello").unwrap());
}

TEST(StringVecIntegration, GrowFromPush) {
    String s; // starts with 0 capacity
    for (int i = 0; i < 200; i++) {
        s.push(static_cast<char32_t>('a' + (i % 26)));
    }
    EXPECT_EQ(s.len(), 200u);
    EXPECT_EQ(s.char_len(), 200u);
}
