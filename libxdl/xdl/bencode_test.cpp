/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * bencode_test.cpp - Comprehensive bencode tests
 */

#include <gtest/gtest.h>

#include <cstring>
#include <string>

extern "C" {
#include <xdl/bencode.h>
}

static std::string to_string(const xdl_bencode_value_t *v) {
  if (!v) return "(null)";
  uint8_t *data = nullptr;
  size_t len = 0;
  if (xdl_bencode_write(v, &data, &len) < 0) return "(error)";
  std::string s(reinterpret_cast<const char *>(data), len);
  free(data);
  return s;
}

static xdl_bencode_value_t *parse(const std::string &s) {
  xdl_bencode_value_t *v = nullptr;
  int rc = xdl_bencode_parse(reinterpret_cast<const uint8_t *>(s.data()), s.size(), &v);
  return rc == 0 ? v : nullptr;
}

static std::string info_range(const std::string &data,
                              const uint8_t **out_start, size_t *out_len) {
  int rc = xdl_bencode_info_range(reinterpret_cast<const uint8_t *>(data.data()), data.size(),
                                   out_start, out_len);
  if (rc < 0) return "";
  return std::string(reinterpret_cast<const char *>(*out_start), *out_len);
}

/* ── String tests ──────────────────────────────────────── */

TEST(Bencode, ParseString) {
  auto v = parse("4:spam");
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->type, XDL_BENCODE_STRING);
  EXPECT_EQ(v->str.len, static_cast<size_t>(4));
  EXPECT_EQ(memcmp(v->str.data, "spam", 4), 0);
  xdl_bencode_value_free(v);
}

TEST(Bencode, ParseEmptyString) {
  auto v = parse("0:");
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->type, XDL_BENCODE_STRING);
  EXPECT_EQ(v->str.len, static_cast<size_t>(0));
  xdl_bencode_value_free(v);
}

TEST(Bencode, ParseStringWithBinary) {
  const char raw[] = "4:\x00\x01\x02\x03";
  auto v = parse(std::string(raw, 6));
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->type, XDL_BENCODE_STRING);
  EXPECT_EQ(v->str.len, static_cast<size_t>(4));
  EXPECT_EQ(memcmp(v->str.data, "\x00\x01\x02\x03", 4), 0);
  xdl_bencode_value_free(v);
}

TEST(Bencode, ParseLongString) {
  auto v = parse("10:0123456789");
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->str.len, static_cast<size_t>(10));
  EXPECT_EQ(memcmp(v->str.data, "0123456789", 10), 0);
  xdl_bencode_value_free(v);
}

/* ── Integer tests ─────────────────────────────────────── */

TEST(Bencode, ParseInteger) {
  auto v = parse("i3e");
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->type, XDL_BENCODE_INTEGER);
  EXPECT_EQ(v->integer, 3);
  xdl_bencode_value_free(v);
}

TEST(Bencode, ParseNegativeInteger) {
  auto v = parse("i-3e");
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->type, XDL_BENCODE_INTEGER);
  EXPECT_EQ(v->integer, -3);
  xdl_bencode_value_free(v);
}

TEST(Bencode, ParseZero) {
  auto v = parse("i0e");
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->integer, 0);
  xdl_bencode_value_free(v);
}

TEST(Bencode, ParseLargeInteger) {
  auto v = parse("i1234567890e");
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->integer, 1234567890);
  xdl_bencode_value_free(v);
}

TEST(Bencode, ParseNegativeZeroInvalid) {
  auto v = parse("i-0e");
  EXPECT_EQ(v, nullptr);
}

TEST(Bencode, ParseLeadingZeroInvalid) {
  auto v = parse("i03e");
  EXPECT_EQ(v, nullptr);
}

/* ── List tests ────────────────────────────────────────── */

TEST(Bencode, ParseEmptyList) {
  auto v = parse("le");
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->type, XDL_BENCODE_LIST);
  EXPECT_EQ(v->list.count, static_cast<size_t>(0));
  xdl_bencode_value_free(v);
}

TEST(Bencode, ParseListOfStrings) {
  auto v = parse("l4:spam4:eggse");
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->list.count, static_cast<size_t>(2));
  EXPECT_EQ(to_string(v->list.items[0]), "4:spam");
  EXPECT_EQ(to_string(v->list.items[1]), "4:eggs");
  xdl_bencode_value_free(v);
}

TEST(Bencode, ParseListOfIntegers) {
  auto v = parse("li1ei2ei3ee");
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->list.count, static_cast<size_t>(3));
  EXPECT_EQ(v->list.items[0]->integer, 1);
  EXPECT_EQ(v->list.items[1]->integer, 2);
  EXPECT_EQ(v->list.items[2]->integer, 3);
  xdl_bencode_value_free(v);
}

TEST(Bencode, ParseNestedList) {
  auto v = parse("ll4:spam4:eggsel3:foo3:baree");
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->list.count, static_cast<size_t>(2));
  EXPECT_EQ(v->list.items[0]->type, XDL_BENCODE_LIST);
  EXPECT_EQ(v->list.items[0]->list.count, static_cast<size_t>(2));
  EXPECT_EQ(v->list.items[1]->type, XDL_BENCODE_LIST);
  EXPECT_EQ(v->list.items[1]->list.count, static_cast<size_t>(2));
  xdl_bencode_value_free(v);
}

/* ── Dict tests ────────────────────────────────────────── */

TEST(Bencode, ParseEmptyDict) {
  auto v = parse("de");
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->type, XDL_BENCODE_DICT);
  EXPECT_EQ(v->dict.count, static_cast<size_t>(0));
  xdl_bencode_value_free(v);
}

TEST(Bencode, ParseSimpleDict) {
  auto v = parse("d3:cow3:moo4:spam4:eggse");
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->dict.count, static_cast<size_t>(2));
  xdl_bencode_value_free(v);
}

TEST(Bencode, ParseDictWithIntValue) {
  auto v = parse("d3:keyi42ee");
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->dict.count, (size_t)1);
  EXPECT_EQ(to_string(v->dict.keys[0]), "3:key");
  EXPECT_EQ(v->dict.values[0]->type, XDL_BENCODE_INTEGER);
  EXPECT_EQ(v->dict.values[0]->integer, 42);
  xdl_bencode_value_free(v);
}

TEST(Bencode, ParseNestedDict) {
  auto v = parse("d4:infod4:name4:test6:lengthi1000eee");
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->dict.count, (size_t)1);
  EXPECT_EQ(to_string(v->dict.keys[0]), "4:info");
  EXPECT_EQ(v->dict.values[0]->type, XDL_BENCODE_DICT);
  auto *inner = v->dict.values[0];
  EXPECT_EQ(inner->dict.count, static_cast<size_t>(2));
  xdl_bencode_value_free(v);
}

/* ── Dict find ─────────────────────────────────────────── */

TEST(Bencode, DictFind) {
  auto v = parse("d3:cow3:moo4:spam4:eggse");
  ASSERT_NE(v, nullptr);
  auto *cow = xdl_bencode_dict_find(v, "cow");
  ASSERT_NE(cow, nullptr);
  EXPECT_EQ(cow->type, XDL_BENCODE_STRING);
  EXPECT_EQ(memcmp(cow->str.data, "moo", 3), 0);

  auto *spam = xdl_bencode_dict_find(v, "spam");
  ASSERT_NE(spam, nullptr);
  EXPECT_EQ(spam->type, XDL_BENCODE_STRING);
  EXPECT_EQ(memcmp(spam->str.data, "eggs", 4), 0);

  auto *missing = xdl_bencode_dict_find(v, "missing");
  EXPECT_EQ(missing, nullptr);
  xdl_bencode_value_free(v);
}

/* ── Write tests ───────────────────────────────────────── */

TEST(Bencode, WriteString) {
  auto v = parse("4:spam"); ASSERT_NE(v, nullptr);
  EXPECT_EQ(to_string(v), "4:spam"); xdl_bencode_value_free(v);
}

TEST(Bencode, WriteInteger) {
  auto v = parse("i42e"); ASSERT_NE(v, nullptr);
  EXPECT_EQ(to_string(v), "i42e"); xdl_bencode_value_free(v);
}

TEST(Bencode, WriteList) {
  auto v = parse("l4:spam4:eggse"); ASSERT_NE(v, nullptr);
  EXPECT_EQ(to_string(v), "l4:spam4:eggse"); xdl_bencode_value_free(v);
}

TEST(Bencode, WriteDict) {
  auto v = parse("d3:cow3:moo4:spam4:eggse"); ASSERT_NE(v, nullptr);
  EXPECT_EQ(to_string(v), "d3:cow3:moo4:spam4:eggse"); xdl_bencode_value_free(v);
}

TEST(Bencode, WriteLargeInteger) {
  auto v = parse("i1234567890e"); ASSERT_NE(v, nullptr);
  EXPECT_EQ(to_string(v), "i1234567890e"); xdl_bencode_value_free(v);
}

TEST(Bencode, WriteNestedList) {
  auto v = parse("ll4:spam4:eggsel3:foo3:baree"); ASSERT_NE(v, nullptr);
  EXPECT_EQ(to_string(v), "ll4:spam4:eggsel3:foo3:baree"); xdl_bencode_value_free(v);
}

TEST(Bencode, WriteNestedDict) {
  auto v = parse("d4:infod4:name4:test6:lengthi1000eee"); ASSERT_NE(v, nullptr);
  std::string written = to_string(v);
  EXPECT_NE(written.find("4:name"), std::string::npos);
  EXPECT_NE(written.find("6:length"), std::string::npos);
  xdl_bencode_value_free(v);
}

/* ── Dict sort ─────────────────────────────────────────── */

TEST(Bencode, DictSort) {
  auto v = parse("d3:cow3:moo4:spam4:eggse"); ASSERT_NE(v, nullptr);
  xdl_bencode_dict_sort(v);
  EXPECT_EQ(to_string(v->dict.keys[0]), "3:cow");
  EXPECT_EQ(to_string(v->dict.keys[1]), "4:spam");
  EXPECT_EQ(to_string(v), "d3:cow3:moo4:spam4:eggse");
  xdl_bencode_value_free(v);
}

TEST(Bencode, DictSortUnsorted) {
  auto v = parse("d4:spam4:eggs3:cow3:mooe"); ASSERT_NE(v, nullptr);
  xdl_bencode_dict_sort(v);
  EXPECT_EQ(to_string(v->dict.keys[0]), "3:cow");
  EXPECT_EQ(to_string(v->dict.keys[1]), "4:spam");
  EXPECT_EQ(to_string(v), "d3:cow3:moo4:spam4:eggse");
  xdl_bencode_value_free(v);
}

TEST(Bencode, DictSortEmpty) {
  auto v = parse("de"); ASSERT_NE(v, nullptr);
  xdl_bencode_dict_sort(v);
  xdl_bencode_value_free(v);
}

TEST(Bencode, DictSortSingle) {
  auto v = parse("d3:keyi1ee"); ASSERT_NE(v, nullptr);
  xdl_bencode_dict_sort(v);
  EXPECT_EQ(to_string(v), "d3:keyi1ee");
  xdl_bencode_value_free(v);
}

/* ── Info range ────────────────────────────────────────── */

TEST(Bencode, InfoRangeBasic) {
  std::string data = "d4:infod4:name4:test6:lengthi1000eee";
  const uint8_t *start = nullptr;
  size_t len = 0;
  auto s = info_range(data, &start, &len);
  EXPECT_EQ(s, "d4:name4:test6:lengthi1000ee");
}

TEST(Bencode, InfoRangeTrailingData) {
  std::string d = "d3:keyi1e4:infod1:ai2ee";
  const uint8_t *start = nullptr;
  size_t len = 0;
  auto s = info_range(d, &start, &len);
  EXPECT_EQ(s, "d1:ai2ee");
  xdl_bencode_value_free(parse(d));
}

TEST(Bencode, InfoRangeTorrentFormat) {
  std::string d = "d8:announce1:u4:infod1:ai1ee";
  const uint8_t *start = nullptr;
  size_t len = 0;
  auto s = info_range(d, &start, &len);
  EXPECT_EQ(s, "d1:ai1ee");
}

TEST(Bencode, InfoRangeNotFound) {
  std::string data = "d4:name4:teste";
  const uint8_t *start = nullptr;
  size_t len = 0;
  int rc = xdl_bencode_info_range(reinterpret_cast<const uint8_t *>(data.data()), data.size(),
                                   &start, &len);
  EXPECT_NE(rc, 0);
}

/* ── Parse errors ──────────────────────────────────────── */

TEST(Bencode, ParseErrorTruncatedString) { EXPECT_EQ(parse("4:sp"), nullptr); }
TEST(Bencode, ParseErrorTruncatedInt) { EXPECT_EQ(parse("i4"), nullptr); }
TEST(Bencode, ParseErrorTruncatedList) { EXPECT_EQ(parse("l4:spam"), nullptr); }
TEST(Bencode, ParseErrorTruncatedDict) { EXPECT_EQ(parse("d3:cow3:moo"), nullptr); }
TEST(Bencode, ParseErrorUnexpectedChar) { EXPECT_EQ(parse("x"), nullptr); }
TEST(Bencode, ParseErrorEmpty) { EXPECT_EQ(parse(""), nullptr); }
TEST(Bencode, ParseErrorTrailingData) { EXPECT_EQ(parse("i1ei2e"), nullptr); }
TEST(Bencode, ParseErrorNegativeZero) { EXPECT_EQ(parse("i-0e"), nullptr); }
TEST(Bencode, ParseErrorInvalidIntegerChars) { EXPECT_EQ(parse("iabce"), nullptr); }
TEST(Bencode, ParseErrorListWithNonStringKey) { EXPECT_EQ(parse("di42e3:abce"), nullptr); }

/* ── Large data ────────────────────────────────────────── */

TEST(Bencode, LargeList) {
  std::string data = "l";
  for (int i = 0; i < 1000; i++) data += "i" + std::to_string(i) + "e";
  data += "e";
  auto v = parse(data);
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->list.count, static_cast<size_t>(1000));
  for (size_t i = 0; i < 1000; i++) EXPECT_EQ(v->list.items[i]->integer, (int64_t)i);
  xdl_bencode_value_free(v);
}

TEST(Bencode, LargeString) {
  auto v = parse("1000000:" + std::string(1000000, 'X'));
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->str.len, static_cast<size_t>(1000000));
  xdl_bencode_value_free(v);
}
