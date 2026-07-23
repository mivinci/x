/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * json_test.cpp - Tests for xJson (DOM-style JSON parser/builder/serializer)
 */

#include <cstring>

#include <gtest/gtest.h>

#include <x/json/json.h>

/* ───────────────────── Parse — Basic Types ───────────────────── */

TEST(JsonParse, Null) {
  xJson *root = xJsonParse("null", 4);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(xJsonType(root), XJSON_NULL);
  xJsonFree(root);
}

TEST(JsonParse, BoolTrue) {
  xJson *root = xJsonParse("true", 4);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(xJsonType(root), XJSON_BOOL);
  EXPECT_EQ(xJsonBool(root), 1);
  xJsonFree(root);
}

TEST(JsonParse, BoolFalse) {
  xJson *root = xJsonParse("false", 5);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(xJsonType(root), XJSON_BOOL);
  EXPECT_EQ(xJsonBool(root), 0);
  xJsonFree(root);
}

TEST(JsonParse, IntPositive) {
  xJson *root = xJsonParse("42", 2);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(xJsonType(root), XJSON_INT);
  EXPECT_EQ(xJsonInt(root), 42);
  xJsonFree(root);
}

TEST(JsonParse, IntNegative) {
  xJson *root = xJsonParse("-123", 4);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(xJsonType(root), XJSON_INT);
  EXPECT_EQ(xJsonInt(root), -123);
  xJsonFree(root);
}

TEST(JsonParse, DoubleFraction) {
  xJson *root = xJsonParse("3.14", 4);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(xJsonType(root), XJSON_DOUBLE);
  EXPECT_DOUBLE_EQ(xJsonDouble(root), 3.14);
  xJsonFree(root);
}

TEST(JsonParse, DoubleExponent) {
  xJson *root = xJsonParse("1e10", 4);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(xJsonType(root), XJSON_DOUBLE);
  EXPECT_DOUBLE_EQ(xJsonDouble(root), 1e10);
  xJsonFree(root);
}

TEST(JsonParse, StringSimple) {
  xJson *root = xJsonParse("\"hello\"", 7);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(xJsonType(root), XJSON_STRING);
  EXPECT_STREQ(xJsonString(root), "hello");
  xJsonFree(root);
}

TEST(JsonParse, StringEscapes) {
  xJson *root = xJsonParse("\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"", 18);
  ASSERT_NE(root, nullptr);
  EXPECT_STREQ(xJsonString(root), "\"\\/\b\f\n\r\t");
  xJsonFree(root);
}

TEST(JsonParse, StringUnicode) {
  xJson *root = xJsonParse("\"\\u0041\\u0042\\u0043\"", 20);
  ASSERT_NE(root, nullptr);
  EXPECT_STREQ(xJsonString(root), "ABC");
  xJsonFree(root);
}

TEST(JsonParse, StringLength) {
  xJson *root = xJsonParse("\"hello\"", 7);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(xJsonStringLength(root), 5u);
  xJsonFree(root);
}

/* ───────────────────── Parse — Compound Types ───────────────────── */

TEST(JsonParse, EmptyArray) {
  xJson *root = xJsonParse("[]", 2);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(xJsonType(root), XJSON_ARRAY);
  EXPECT_EQ(xJsonArraySize(root), 0);
  xJsonFree(root);
}

TEST(JsonParse, ArrayOfInts) {
  xJson *root = xJsonParse("[1,2,3]", 7);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(xJsonArraySize(root), 3);
  EXPECT_EQ(xJsonInt(xJsonArrayGet(root, 0)), 1);
  EXPECT_EQ(xJsonInt(xJsonArrayGet(root, 1)), 2);
  EXPECT_EQ(xJsonInt(xJsonArrayGet(root, 2)), 3);
  xJsonFree(root);
}

TEST(JsonParse, ArrayNegativeIndex) {
  xJson *root = xJsonParse("[1,2,3]", 7);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(xJsonInt(xJsonArrayGet(root, -1)), 3);
  EXPECT_EQ(xJsonInt(xJsonArrayGet(root, -3)), 1);
  EXPECT_EQ(xJsonArrayGet(root, -4), nullptr);
  xJsonFree(root);
}

TEST(JsonParse, EmptyObject) {
  xJson *root = xJsonParse("{}", 2);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(xJsonType(root), XJSON_OBJECT);
  EXPECT_EQ(xJsonObjectSize(root), 0);
  xJsonFree(root);
}

TEST(JsonParse, ObjectSimple) {
  xJson *root = xJsonParse("{\"name\":\"leo\",\"age\":30}", 23);
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(xJsonObjectSize(root), 2);
  EXPECT_STREQ(xJsonString(xJsonObjectGet(root, "name")), "leo");
  EXPECT_EQ(xJsonInt(xJsonObjectGet(root, "age")), 30);
  xJsonFree(root);
}

TEST(JsonParse, NestedObject) {
  const char *json = "{\"user\":{\"name\":\"leo\",\"tags\":[\"c\",\"json\"]}}";
  xJson *root = xJsonParse(json, strlen(json));
  ASSERT_NE(root, nullptr);

  xJson *user = xJsonObjectGet(root, "user");
  ASSERT_NE(user, nullptr);
  EXPECT_STREQ(xJsonString(xJsonObjectGet(user, "name")), "leo");

  xJson *tags = xJsonObjectGet(user, "tags");
  ASSERT_NE(tags, nullptr);
  EXPECT_EQ(xJsonArraySize(tags), 2);
  EXPECT_STREQ(xJsonString(xJsonArrayGet(tags, 0)), "c");
  EXPECT_STREQ(xJsonString(xJsonArrayGet(tags, 1)), "json");

  xJsonFree(root);
}

/* ───────────────────── Parse — Errors ───────────────────── */

TEST(JsonParse, EmptyInput)   { EXPECT_EQ(xJsonParse("", 0), nullptr); }
TEST(JsonParse, TrailingComma){ EXPECT_EQ(xJsonParse("[1,]", 4), nullptr); }
TEST(JsonParse, MissingColon) { EXPECT_EQ(xJsonParse("{\"a\" 1}", 7), nullptr); }
TEST(JsonParse, UnclosedStr)  { EXPECT_EQ(xJsonParse("\"oops", 5), nullptr); }
TEST(JsonParse, Garbage)      { EXPECT_EQ(xJsonParse("garbage", 7), nullptr); }
TEST(JsonParse, TrailingGarbage) { EXPECT_EQ(xJsonParse("1 2", 3), nullptr); }
TEST(JsonParse, LeadingZeroInt) { EXPECT_EQ(xJsonParse("01", 2), nullptr); }
TEST(JsonParse, LeadingZeroNeg) { EXPECT_EQ(xJsonParse("-01", 3), nullptr); }
TEST(JsonParse, TrailingCommaObj){ EXPECT_EQ(xJsonParse("{\"a\":1,}", 7), nullptr); }

TEST(JsonParse, NullInput) { EXPECT_EQ(xJsonParse(NULL, 0), nullptr); }
TEST(JsonParseCopy, NullInput) { EXPECT_EQ(xJsonParseCopy(NULL, 0), nullptr); }

/* ───────────────────── ParseCopy ───────────────────── */

TEST(JsonParseCopy, Basic) {
  char buf[] = "{\"x\":\"hello\"}";
  xJson *root = xJsonParseCopy(buf, strlen(buf));
  ASSERT_NE(root, nullptr);

  buf[6] = 'X'; /* corrupt buffer */
  EXPECT_STREQ(xJsonString(xJsonObjectGet(root, "x")), "hello");
  xJsonFree(root);
}

/* ───────────────────── Manual Construction ───────────────────── */

TEST(JsonConstruct, NewNull) {
  xJson *n = xJsonNewNull();
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(xJsonType(n), XJSON_NULL);
  xJsonFree(n);
}

TEST(JsonConstruct, NewBool) {
  xJson *n = xJsonNewBool(1);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(xJsonBool(n), 1);
  xJsonFree(n);
}

TEST(JsonConstruct, NewInt) {
  xJson *n = xJsonNewInt(-42);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(xJsonInt(n), -42);
  xJsonFree(n);
}

TEST(JsonConstruct, NewDouble) {
  xJson *n = xJsonNewDouble(2.718);
  ASSERT_NE(n, nullptr);
  EXPECT_DOUBLE_EQ(xJsonDouble(n), 2.718);
  xJsonFree(n);
}

TEST(JsonConstruct, NewString) {
  xJson *n = xJsonNewString("hello");
  ASSERT_NE(n, nullptr);
  EXPECT_STREQ(xJsonString(n), "hello");
  xJsonFree(n);
}

TEST(JsonConstruct, NewStringN) {
  xJson *n = xJsonNewStringN("a\0b", 4);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(xJsonStringLength(n), 4u);
  /* cannot use EXPECT_STREQ because of embedded NUL; verify via length */
  xJsonFree(n);
}

TEST(JsonConstruct, NewArray) {
  xJson *a = xJsonNewArray();
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(xJsonType(a), XJSON_ARRAY);
  EXPECT_EQ(xJsonArraySize(a), 0);
  xJsonFree(a);
}

TEST(JsonConstruct, NewObject) {
  xJson *o = xJsonNewObject();
  ASSERT_NE(o, nullptr);
  EXPECT_EQ(xJsonType(o), XJSON_OBJECT);
  EXPECT_EQ(xJsonObjectSize(o), 0);
  xJsonFree(o);
}

/* ───────────────────── Object Operations ───────────────────── */

TEST(JsonObject, SetAndGet) {
  xJson *obj = xJsonNewObject();
  ASSERT_EQ(xJsonObjectSet(obj, "name", xJsonNewString("leo")), 0);
  ASSERT_EQ(xJsonObjectSet(obj, "age", xJsonNewInt(30)), 0);

  EXPECT_STREQ(xJsonString(xJsonObjectGet(obj, "name")), "leo");
  EXPECT_EQ(xJsonInt(xJsonObjectGet(obj, "age")), 30);
  EXPECT_EQ(xJsonObjectGet(obj, "missing"), nullptr);

  xJsonFree(obj);
}

TEST(JsonObject, SetReplace) {
  xJson *obj = xJsonNewObject();
  xJsonObjectSet(obj, "x", xJsonNewInt(1));
  xJsonObjectSet(obj, "x", xJsonNewInt(2)); /* replace */
  EXPECT_EQ(xJsonInt(xJsonObjectGet(obj, "x")), 2);
  EXPECT_EQ(xJsonObjectSize(obj), 1); /* not 2 */
  xJsonFree(obj);
}

TEST(JsonObject, Del) {
  xJson *obj = xJsonNewObject();
  xJsonObjectSet(obj, "a", xJsonNewInt(1));
  xJsonObjectSet(obj, "b", xJsonNewInt(2));
  EXPECT_EQ(xJsonObjectSize(obj), 2);

  xJsonObjectDel(obj, "a");
  EXPECT_EQ(xJsonObjectSize(obj), 1);
  EXPECT_EQ(xJsonObjectGet(obj, "a"), nullptr);
  EXPECT_EQ(xJsonInt(xJsonObjectGet(obj, "b")), 2);

  xJsonObjectDel(obj, "missing"); /* no-op */
  xJsonFree(obj);
}

TEST(JsonObject, Iterator) {
  xJson *obj = xJsonNewObject();
  xJsonObjectSet(obj, "x", xJsonNewInt(1));
  xJsonObjectSet(obj, "y", xJsonNewInt(2));
  xJsonObjectSet(obj, "z", xJsonNewInt(3));

  xJsonIterator *it = xJsonNewIterator(obj);
  ASSERT_NE(it, nullptr);

  int count = 0;
  size_t key_len = 0;
  while (xJsonIteratorNext(it)) {
    const char *k = xJsonIteratorKey(it, &key_len);
    ASSERT_NE(k, nullptr);
    EXPECT_GT(key_len, 0u);
    EXPECT_NE(xJsonIteratorValue(it), nullptr);
    count++;
  }
  EXPECT_EQ(count, 3);

  xJsonFree(it);
  xJsonFree(obj);
}

TEST(JsonObject, NullSafety) {
  EXPECT_EQ(xJsonObjectGet(nullptr, "x"), nullptr);
  {
    xJson *tmp = xJsonNewObject();
    EXPECT_EQ(xJsonObjectGet(tmp, nullptr), nullptr);
    xJsonFree(tmp);
  }
  {
    xJson *v = xJsonNewInt(1);
    EXPECT_NE(xJsonObjectSet(nullptr, "x", v), 0);
    xJsonFree(v);  /* ownership not transferred on failure */
  }
  {
    xJson *tmp = xJsonNewObject();
    EXPECT_NE(xJsonObjectSet(tmp, "x", nullptr), 0);
    xJsonFree(tmp);
  }
  {
    xJson *v = xJsonNewInt(1);
    EXPECT_EQ(xJsonNewIterator(v), nullptr);
    xJsonFree(v);
  }
  EXPECT_EQ(xJsonNewIterator(nullptr), nullptr);

  xJson *o = xJsonNewObject();
  xJsonObjectDel(o, nullptr);  /* no-op */
  xJsonObjectDel(nullptr, "x");
  xJsonFree(o);
}

/* ───────────────────── Array Operations ───────────────────── */

TEST(JsonArray, Append) {
  xJson *arr = xJsonNewArray();
  ASSERT_EQ(xJsonArrayAppend(arr, xJsonNewInt(10)), 0);
  ASSERT_EQ(xJsonArrayAppend(arr, xJsonNewString("hi")), 0);
  EXPECT_EQ(xJsonArraySize(arr), 2);
  EXPECT_EQ(xJsonInt(xJsonArrayGet(arr, 0)), 10);
  EXPECT_STREQ(xJsonString(xJsonArrayGet(arr, 1)), "hi");
  xJsonFree(arr);
}

TEST(JsonArray, Set) {
  xJson *arr = xJsonNewArray();
  xJsonArrayAppend(arr, xJsonNewInt(1));
  xJsonArrayAppend(arr, xJsonNewInt(2));
  ASSERT_EQ(xJsonArraySet(arr, 0, xJsonNewInt(99)), 0);
  EXPECT_EQ(xJsonInt(xJsonArrayGet(arr, 0)), 99);
  EXPECT_EQ(xJsonArraySize(arr), 2);
  xJsonFree(arr);
}

TEST(JsonArray, SetOutOfBounds) {
  xJson *arr = xJsonNewArray();
  xJson *v1 = xJsonNewInt(1);
  EXPECT_NE(xJsonArraySet(arr, 0, v1), 0);
  xJsonFree(v1);  /* ownership not transferred on failure */
  xJson *v2 = xJsonNewInt(1);
  EXPECT_NE(xJsonArraySet(arr, 1, v2), 0);
  xJsonFree(v2);
  xJsonFree(arr);
}

TEST(JsonArray, Insert) {
  xJson *arr = xJsonNewArray();
  xJsonArrayAppend(arr, xJsonNewInt(1));
  xJsonArrayAppend(arr, xJsonNewInt(3));
  ASSERT_EQ(xJsonArrayInsert(arr, 1, xJsonNewInt(2)), 0); /* insert middle */
  EXPECT_EQ(xJsonArraySize(arr), 3);
  EXPECT_EQ(xJsonInt(xJsonArrayGet(arr, 0)), 1);
  EXPECT_EQ(xJsonInt(xJsonArrayGet(arr, 1)), 2);
  EXPECT_EQ(xJsonInt(xJsonArrayGet(arr, 2)), 3);
  xJsonFree(arr);
}

TEST(JsonArray, InsertAtEnd) {
  xJson *arr = xJsonNewArray();
  ASSERT_EQ(xJsonArrayInsert(arr, 0, xJsonNewInt(1)), 0);
  EXPECT_EQ(xJsonArraySize(arr), 1);
  EXPECT_EQ(xJsonInt(xJsonArrayGet(arr, 0)), 1);
  xJsonFree(arr);
}

TEST(JsonArray, Remove) {
  xJson *arr = xJsonNewArray();
  xJsonArrayAppend(arr, xJsonNewInt(1));
  xJsonArrayAppend(arr, xJsonNewInt(2));
  xJsonArrayAppend(arr, xJsonNewInt(3));
  xJsonArrayRemove(arr, 1); /* remove middle */
  EXPECT_EQ(xJsonArraySize(arr), 2);
  EXPECT_EQ(xJsonInt(xJsonArrayGet(arr, 0)), 1);
  EXPECT_EQ(xJsonInt(xJsonArrayGet(arr, 1)), 3);
  xJsonArrayRemove(arr, 100); /* no-op */
  xJsonFree(arr);
}

TEST(JsonArray, SetNegativeIndex) {
  xJson *arr = xJsonNewArray();
  xJsonArrayAppend(arr, xJsonNewInt(1));
  xJsonArrayAppend(arr, xJsonNewInt(2));
  ASSERT_EQ(xJsonArraySet(arr, -1, xJsonNewInt(99)), 0);
  EXPECT_EQ(xJsonInt(xJsonArrayGet(arr, 1)), 99);
  xJsonFree(arr);
}

TEST(JsonArray, NullSafety) {
  xJson *arr = xJsonNewArray();
  EXPECT_NE(xJsonArraySet(arr, 0, nullptr), 0);
  EXPECT_NE(xJsonArrayAppend(arr, nullptr), 0);
  EXPECT_NE(xJsonArrayInsert(arr, 0, nullptr), 0);
  xJsonArrayRemove(arr, -999);  /* no-op on empty */
  EXPECT_EQ(xJsonArrayGet(arr, 0), nullptr);
  xJsonFree(arr);
}

/* ───────────────────── Serialise ───────────────────── */

TEST(JsonStringify, Compact) {
  xJson *obj = xJsonNewObject();
  xJsonObjectSet(obj, "a", xJsonNewInt(1));
  xJsonObjectSet(obj, "b", xJsonNewString("hi"));

  char *s = xJsonStringify(obj);
  ASSERT_NE(s, nullptr);
  EXPECT_STREQ(s, "{\"a\":1,\"b\":\"hi\"}");
  free(s);
  xJsonFree(obj);
}

TEST(JsonStringify, Pretty) {
  xJson *obj = xJsonNewObject();
  xJsonObjectSet(obj, "a", xJsonNewInt(1));

  char *s = xJsonStringifyPretty(obj);
  ASSERT_NE(s, nullptr);
  /* Should contain newlines and indentation. */
  EXPECT_NE(strstr(s, "\n"), nullptr);
  EXPECT_NE(strstr(s, "  "), nullptr);
  free(s);
  xJsonFree(obj);
}

TEST(JsonStringify, ParseRoundTrip) {
  const char *input = "{\"n\":42,\"s\":\"hello\",\"b\":true,\"arr\":[1,2,3]}";
  xJson *root = xJsonParse(input, strlen(input));
  ASSERT_NE(root, nullptr);

  char *out = xJsonStringify(root);
  ASSERT_NE(out, nullptr);

  /* Re-parse the output — should produce the same structure. */
  xJson *root2 = xJsonParse(out, strlen(out));
  ASSERT_NE(root2, nullptr);
  EXPECT_EQ(xJsonObjectSize(root2), 4);

  free(out);
  xJsonFree(root);
  xJsonFree(root2);
}

TEST(JsonStringify, StringifyTo) {
  xJson *n = xJsonNewString("hi");
  char buf[8];
  size_t len = sizeof(buf);

  int r = xJsonStringifyTo(n, 0, buf, &len);
  EXPECT_EQ(r, 0);
  EXPECT_STREQ(buf, "\"hi\"");
  EXPECT_EQ(len, 5u); /* "hi" → 4 chars + NUL = 5 */
  xJsonFree(n);
}

TEST(JsonStringify, StringifyToTruncated) {
  xJson *n = xJsonNewString("longstring");
  char buf[4];
  size_t len = sizeof(buf);

  int r = xJsonStringifyTo(n, 0, buf, &len);
  EXPECT_NE(r, 0); /* truncated */
  xJsonFree(n);
}

TEST(JsonStringify, StringifyToMeasure) {
  xJson *obj = xJsonNewObject();
  xJsonObjectSet(obj, "x", xJsonNewInt(1));

  size_t len = 0;
  int r = xJsonStringifyTo(obj, 0, NULL, &len);
  EXPECT_EQ(r, -1);
  EXPECT_GT(len, 0u); /* measured size should be positive */
  xJsonFree(obj);
}

TEST(JsonStringify, Array) {
  xJson *arr = xJsonNewArray();
  xJsonArrayAppend(arr, xJsonNewInt(1));
  xJsonArrayAppend(arr, xJsonNewInt(2));

  char *s = xJsonStringify(arr);
  ASSERT_NE(s, nullptr);
  EXPECT_STREQ(s, "[1,2]");
  free(s);
  xJsonFree(arr);
}

TEST(JsonStringify, Scalars) {
  {
    xJson *v = xJsonNewNull();
    char *s = xJsonStringify(v);
    EXPECT_STREQ(s, "null"); free(s); xJsonFree(v);
  }
  {
    xJson *v = xJsonNewBool(1);
    char *s = xJsonStringify(v);
    EXPECT_STREQ(s, "true"); free(s); xJsonFree(v);
  }
  {
    xJson *v = xJsonNewDouble(3.14);
    char *s = xJsonStringify(v);
    EXPECT_NE(strstr(s, "3.14"), nullptr); free(s); xJsonFree(v);
  }
}

TEST(JsonStringify, PrettyArray) {
  xJson *arr = xJsonNewArray();
  xJsonArrayAppend(arr, xJsonNewInt(1));
  char *s = xJsonStringifyPretty(arr);
  ASSERT_NE(s, nullptr);
  EXPECT_NE(strstr(s, "\n"), nullptr);
  free(s);
  xJsonFree(arr);
}

TEST(JsonStringify, StringifyToNullSafety) {
  size_t len = 4;
  EXPECT_NE(xJsonStringifyTo(nullptr, 0, NULL, &len), 0);
  xJson *v = xJsonNewInt(1);
  EXPECT_NE(xJsonStringifyTo(v, 0, NULL, NULL), 0);
  xJsonFree(v);
}

TEST(JsonStringify, StringifyToPretty) {
  xJson *obj = xJsonNewObject();
  xJsonObjectSet(obj, "x", xJsonNewInt(1));
  char buf[64];
  size_t len = sizeof(buf);
  int r = xJsonStringifyTo(obj, 1, buf, &len);
  EXPECT_EQ(r, 0);
  EXPECT_NE(strstr(buf, "  "), nullptr);  /* indentation */
  xJsonFree(obj);
}

/* ───────────────────── Free ───────────────────── */

TEST(JsonFree, NullNoOp) {
  xJsonFree(nullptr); /* should not crash */
}

TEST(JsonFree, IteratorFree) {
  xJson *obj = xJsonNewObject();
  xJsonObjectSet(obj, "x", xJsonNewInt(1));
  xJsonIterator *it = xJsonNewIterator(obj);
  ASSERT_NE(it, nullptr);
  xJsonFree(it);
  xJsonFree(obj);
}

TEST(JsonFree, OwnedNodeDoubleFreeSafe) {
  /* After Set, the value is owned — xJsonFree on the original handle
   * should be a no-op. */
  xJson *v = xJsonNewInt(42);
  xJson *obj = xJsonNewObject();
  xJsonObjectSet(obj, "x", v);

  xJsonFree(v); /* owned → no-op */
  xJsonFree(obj);
}
