/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * json_sax_test.cpp - Tests for xJsonSax (SAX-style JSON parser)
 */

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <x/json/json_sax.h>

/* ── Test helper: simple recorded SAX events ─────────────────────────── */

struct SaxEvent {
  enum Type { kNull, kBool, kInt, kDouble, kString, kKey,
              kArrayBegin, kArrayEnd, kObjectBegin, kObjectEnd } type;
  int bool_val = 0;
  int64_t int_val = 0;
  double double_val = 0.0;
  std::string str;
};

struct SaxRecorder {
  std::vector<SaxEvent> events;
  bool aborted = false;
};

static int rec_on_null(void *ctx) {
  auto *r = static_cast<SaxRecorder *>(ctx);
  r->events.push_back({SaxEvent::kNull});
  return 0;
}

static int rec_on_bool(void *ctx, int v) {
  auto *r = static_cast<SaxRecorder *>(ctx);
  r->events.push_back({SaxEvent::kBool, v});
  return 0;
}

static int rec_on_int(void *ctx, int64_t v) {
  auto *r = static_cast<SaxRecorder *>(ctx);
  r->events.push_back({SaxEvent::kInt, 0, v});
  return 0;
}

static int rec_on_double(void *ctx, double v) {
  auto *r = static_cast<SaxRecorder *>(ctx);
  r->events.push_back({SaxEvent::kDouble, 0, 0, v});
  return 0;
}

static int rec_on_string(void *ctx, const char *s, size_t len) {
  auto *r = static_cast<SaxRecorder *>(ctx);
  r->events.push_back({SaxEvent::kString, 0, 0, 0.0, std::string(s, len)});
  return 0;
}

static int rec_on_key(void *ctx, const char *s, size_t len) {
  auto *r = static_cast<SaxRecorder *>(ctx);
  r->events.push_back({SaxEvent::kKey, 0, 0, 0.0, std::string(s, len)});
  return 0;
}

static int rec_on_array_begin(void *ctx) {
  auto *r = static_cast<SaxRecorder *>(ctx);
  r->events.push_back({SaxEvent::kArrayBegin});
  return 0;
}

static int rec_on_array_end(void *ctx) {
  auto *r = static_cast<SaxRecorder *>(ctx);
  r->events.push_back({SaxEvent::kArrayEnd});
  return 0;
}

static int rec_on_object_begin(void *ctx) {
  auto *r = static_cast<SaxRecorder *>(ctx);
  r->events.push_back({SaxEvent::kObjectBegin});
  return 0;
}

static int rec_on_object_end(void *ctx) {
  auto *r = static_cast<SaxRecorder *>(ctx);
  r->events.push_back({SaxEvent::kObjectEnd});
  return 0;
}

static xJsonSaxHandler make_handler(SaxRecorder *r) {
  xJsonSaxHandler h = {};
  h.on_null         = rec_on_null;
  h.on_bool         = rec_on_bool;
  h.on_int          = rec_on_int;
  h.on_double       = rec_on_double;
  h.on_string       = rec_on_string;
  h.on_key          = rec_on_key;
  h.on_array_begin  = rec_on_array_begin;
  h.on_array_end    = rec_on_array_end;
  h.on_object_begin = rec_on_object_begin;
  h.on_object_end   = rec_on_object_end;
  return h;
}

#define EXPECT_EVENT(idx, evtType) \
  EXPECT_EQ(rec.events[(idx)].type, SaxEvent::evtType)

/* ── Basic scalars ───────────────────────────────────────────────────── */

TEST(JsonSax, Null) {
  SaxRecorder rec;
  xJsonSaxHandler h = make_handler(&rec);
  int r = xJsonSaxParse("null", 4, &h, &rec);
  EXPECT_EQ(r, 0);
  ASSERT_EQ(rec.events.size(), 1u);
  EXPECT_EVENT(0, kNull);
}

TEST(JsonSax, Bool) {
  SaxRecorder rec;
  xJsonSaxHandler h = make_handler(&rec);
  int r = xJsonSaxParse("true", 4, &h, &rec);
  EXPECT_EQ(r, 0);
  ASSERT_EQ(rec.events.size(), 1u);
  EXPECT_EVENT(0, kBool);
  EXPECT_EQ(rec.events[0].bool_val, 1);
}

TEST(JsonSax, Int) {
  SaxRecorder rec;
  xJsonSaxHandler h = make_handler(&rec);
  int r = xJsonSaxParse("-42", 3, &h, &rec);
  EXPECT_EQ(r, 0);
  ASSERT_EQ(rec.events.size(), 1u);
  EXPECT_EVENT(0, kInt);
  EXPECT_EQ(rec.events[0].int_val, -42);
}

TEST(JsonSax, Double) {
  SaxRecorder rec;
  xJsonSaxHandler h = make_handler(&rec);
  int r = xJsonSaxParse("3.14", 4, &h, &rec);
  EXPECT_EQ(r, 0);
  ASSERT_EQ(rec.events.size(), 1u);
  EXPECT_EVENT(0, kDouble);
  EXPECT_DOUBLE_EQ(rec.events[0].double_val, 3.14);
}

TEST(JsonSax, String) {
  SaxRecorder rec;
  xJsonSaxHandler h = make_handler(&rec);
  int r = xJsonSaxParse("\"hello\"", 7, &h, &rec);
  EXPECT_EQ(r, 0);
  ASSERT_EQ(rec.events.size(), 1u);
  EXPECT_EVENT(0, kString);
  EXPECT_EQ(rec.events[0].str, "hello");
}

/* ── Compound types ──────────────────────────────────────────────────── */

TEST(JsonSax, EmptyArray) {
  SaxRecorder rec;
  xJsonSaxHandler h = make_handler(&rec);
  int r = xJsonSaxParse("[]", 2, &h, &rec);
  EXPECT_EQ(r, 0);
  ASSERT_EQ(rec.events.size(), 2u);
  EXPECT_EVENT(0, kArrayBegin);
  EXPECT_EVENT(1, kArrayEnd);
}

TEST(JsonSax, ArrayOfInts) {
  SaxRecorder rec;
  xJsonSaxHandler h = make_handler(&rec);
  int r = xJsonSaxParse("[1,2,3]", 7, &h, &rec);
  EXPECT_EQ(r, 0);
  ASSERT_EQ(rec.events.size(), 5u);
  EXPECT_EVENT(0, kArrayBegin);
  EXPECT_EVENT(1, kInt);
  EXPECT_EVENT(2, kInt);
  EXPECT_EVENT(3, kInt);
  EXPECT_EVENT(4, kArrayEnd);
  EXPECT_EQ(rec.events[1].int_val, 1);
  EXPECT_EQ(rec.events[2].int_val, 2);
  EXPECT_EQ(rec.events[3].int_val, 3);
}

TEST(JsonSax, EmptyObject) {
  SaxRecorder rec;
  xJsonSaxHandler h = make_handler(&rec);
  int r = xJsonSaxParse("{}", 2, &h, &rec);
  EXPECT_EQ(r, 0);
  ASSERT_EQ(rec.events.size(), 2u);
  EXPECT_EVENT(0, kObjectBegin);
  EXPECT_EVENT(1, kObjectEnd);
}

TEST(JsonSax, ObjectSimple) {
  SaxRecorder rec;
  xJsonSaxHandler h = make_handler(&rec);
  int r = xJsonSaxParse("{\"name\":\"leo\",\"age\":30}", 23, &h, &rec);
  EXPECT_EQ(r, 0);
  ASSERT_EQ(rec.events.size(), 6u);
  EXPECT_EVENT(0, kObjectBegin);
  EXPECT_EVENT(1, kKey);     EXPECT_EQ(rec.events[1].str, "name");
  EXPECT_EVENT(2, kString);  EXPECT_EQ(rec.events[2].str, "leo");
  EXPECT_EVENT(3, kKey);     EXPECT_EQ(rec.events[3].str, "age");
  EXPECT_EVENT(4, kInt);     EXPECT_EQ(rec.events[4].int_val, 30);
  EXPECT_EVENT(5, kObjectEnd);
}

TEST(JsonSax, Nested) {
  SaxRecorder rec;
  xJsonSaxHandler h = make_handler(&rec);
  const char *json = "{\"x\":[{\"a\":1},{\"b\":2}]}";
  int r = xJsonSaxParse(json, strlen(json), &h, &rec);
  EXPECT_EQ(r, 0);
  ASSERT_EQ(rec.events.size(), 13u);
  EXPECT_EVENT(0,  kObjectBegin);
  EXPECT_EVENT(1,  kKey);         EXPECT_EQ(rec.events[1].str, "x");
  EXPECT_EVENT(2,  kArrayBegin);
  EXPECT_EVENT(3,  kObjectBegin);
  EXPECT_EVENT(4,  kKey);         EXPECT_EQ(rec.events[4].str, "a");
  EXPECT_EVENT(5,  kInt);         EXPECT_EQ(rec.events[5].int_val, 1);
  EXPECT_EVENT(6,  kObjectEnd);
  EXPECT_EVENT(7,  kObjectBegin);
  EXPECT_EVENT(8,  kKey);         EXPECT_EQ(rec.events[8].str, "b");
  EXPECT_EVENT(9,  kInt);         EXPECT_EQ(rec.events[9].int_val, 2);
  EXPECT_EVENT(10, kObjectEnd);
  EXPECT_EVENT(11, kArrayEnd);
  EXPECT_EVENT(12, kObjectEnd);
}

/* ── Edge cases ──────────────────────────────────────────────────────── */

TEST(JsonSax, StringEscapes) {
  SaxRecorder rec;
  xJsonSaxHandler h = make_handler(&rec);
  int r = xJsonSaxParse("\"\\n\\t\\\"\"", 8, &h, &rec);
  EXPECT_EQ(r, 0);
  ASSERT_EQ(rec.events.size(), 1u);
  EXPECT_EVENT(0, kString);
  EXPECT_EQ(rec.events[0].str, "\n\t\"");
}

TEST(JsonSax, MixedArray) {
  SaxRecorder rec;
  xJsonSaxHandler h = make_handler(&rec);
  const char *json = "[1,\"a\",true,null,2.5]";
  int r = xJsonSaxParse(json, strlen(json), &h, &rec);
  EXPECT_EQ(r, 0);
  ASSERT_EQ(rec.events.size(), 7u);
  EXPECT_EVENT(0, kArrayBegin);
  EXPECT_EVENT(1, kInt);
  EXPECT_EVENT(2, kString);
  EXPECT_EVENT(3, kBool);
  EXPECT_EVENT(4, kNull);
  EXPECT_EVENT(5, kDouble);
  EXPECT_EVENT(6, kArrayEnd);
}

TEST(JsonSax, ObjectMixedTypes) {
  SaxRecorder rec;
  xJsonSaxHandler h = make_handler(&rec);
  const char *json = "{\"b\":true,\"d\":1.5,\"n\":null}";
  int r = xJsonSaxParse(json, strlen(json), &h, &rec);
  EXPECT_EQ(r, 0);
  ASSERT_EQ(rec.events.size(), 8u);
  EXPECT_EVENT(0, kObjectBegin);
  EXPECT_EVENT(1, kKey);   EXPECT_EQ(rec.events[1].str, "b");
  EXPECT_EVENT(2, kBool);
  EXPECT_EVENT(3, kKey);   EXPECT_EQ(rec.events[3].str, "d");
  EXPECT_EVENT(4, kDouble);
  EXPECT_EVENT(5, kKey);   EXPECT_EQ(rec.events[5].str, "n");
  EXPECT_EVENT(6, kNull);
  EXPECT_EVENT(7, kObjectEnd);
}

TEST(JsonSax, DeeplyNested) {
  SaxRecorder rec;
  xJsonSaxHandler h = make_handler(&rec);
  int r = xJsonSaxParse("[[[[]]]]", 8, &h, &rec);
  EXPECT_EQ(r, 0);
  /* 4 begin + 4 end = 8 events */
  ASSERT_EQ(rec.events.size(), 8u);
  EXPECT_EVENT(0, kArrayBegin);
  EXPECT_EVENT(1, kArrayBegin);
  EXPECT_EVENT(2, kArrayBegin);
  EXPECT_EVENT(3, kArrayBegin);
  EXPECT_EVENT(4, kArrayEnd);
  EXPECT_EVENT(5, kArrayEnd);
  EXPECT_EVENT(6, kArrayEnd);
  EXPECT_EVENT(7, kArrayEnd);
}

/* ── Error handling ──────────────────────────────────────────────────── */

TEST(JsonSax, ErrorEmpty) {
  SaxRecorder rec;
  xJsonSaxHandler h = make_handler(&rec);
  EXPECT_EQ(xJsonSaxParse("", 0, &h, &rec), -1);
}

TEST(JsonSax, ErrorGarbage) {
  SaxRecorder rec;
  xJsonSaxHandler h = make_handler(&rec);
  EXPECT_EQ(xJsonSaxParse("garbage", 7, &h, &rec), -1);
}

TEST(JsonSax, ErrorTrailingGarbage) {
  SaxRecorder rec;
  xJsonSaxHandler h = make_handler(&rec);
  EXPECT_EQ(xJsonSaxParse("1 2", 3, &h, &rec), -1);
}

/* ── Null handler check ──────────────────────────────────────────────── */

TEST(JsonSax, NullHandler) {
  EXPECT_EQ(xJsonSaxParse("1", 1, nullptr, nullptr), -1);
}

/* ── Abort via callback return ───────────────────────────────────────── */

static int abort_on_int(void *ctx, int64_t v) {
  auto *r = static_cast<SaxRecorder *>(ctx);
  r->events.push_back({SaxEvent::kInt, 0, v});
  r->aborted = true;
  return 42;  /* non-zero aborts parsing */
}

TEST(JsonSax, AbortOnCallback) {
  SaxRecorder rec;
  xJsonSaxHandler h = make_handler(&rec);
  h.on_int = abort_on_int;

  int r = xJsonSaxParse("[1,2,3]", 7, &h, &rec);
  EXPECT_EQ(r, 42);  /* callback return value propagated */
  EXPECT_TRUE(rec.aborted);
  /* Only first element should be processed */
  EXPECT_EQ(rec.events.size(), 2u);  /* array_begin + first int */
}

/* ── Streaming SAX stubs ─────────────────────────────────────────────── */

TEST(JsonSax, StreamingNewFree) {
  SaxRecorder rec;
  xJsonSaxHandler h = make_handler(&rec);
  xJsonSax *sax = xJsonSaxCreate(&h, &rec, 16);
  ASSERT_NE(sax, nullptr);
  xJsonSaxDestroy(sax);
}

TEST(JsonSax, StreamingNewNullHandler) {
  EXPECT_EQ(xJsonSaxCreate(nullptr, nullptr, 0), nullptr);
}

TEST(JsonSax, StreamingFeedNotImplemented) {
  SaxRecorder rec;
  xJsonSaxHandler h = make_handler(&rec);
  xJsonSax *sax = xJsonSaxCreate(&h, &rec, 0);
  ASSERT_NE(sax, nullptr);

  EXPECT_EQ(xJsonSaxFeed(sax, "1", 1), xJsonSaxResult_Error);
  EXPECT_EQ(xJsonSaxFinalize(sax), xJsonSaxResult_Error);

  xJsonSaxDestroy(sax);
}

TEST(JsonSax, StreamingReset) {
  SaxRecorder rec;
  xJsonSaxHandler h = make_handler(&rec);
  xJsonSax *sax = xJsonSaxCreate(&h, &rec, 0);
  ASSERT_NE(sax, nullptr);
  xJsonSaxReset(sax);  /* should not crash */
  xJsonSaxDestroy(sax);
}

TEST(JsonSax, StreamingFreeNull) {
  xJsonSaxDestroy(nullptr);  /* should not crash */
}
