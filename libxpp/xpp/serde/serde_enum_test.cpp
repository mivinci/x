/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * serde_enum_test.cpp - Tests for tagged variants (Phase 5).
 *
 * Verifies:
 *  - external tagging round-trips through JSON and binary
 *  - adjacent tagging round-trips through JSON and binary
 *  - unknown tag on deserialize fails with UnknownField
 *  - nested variant inside an XPP_SERDE struct round-trips
 */

#include <cstdint>
#include <cstring>
#include <utility>

#include <gtest/gtest.h>
#include <xpp/enum.h>
#include <xpp/option.h>
#include <xpp/result.h>
#include <xpp/serde/bin.h>
#include <xpp/serde/json.h>
#include <xpp/serde/macros.h>
#include <xpp/string.h>
#include <xpp/vec.h>
#include <xpp/void.h>

/* ── Payload structs (global namespace — XPP_SERDE needs this) ── */

struct ShapeCircle {
  double r;
};
XPP_SERDE(ShapeCircle, (r))

struct ShapeSquare {
  double s;
};
XPP_SERDE(ShapeSquare, (s))

struct ShapeTriangle {
  double base;
  double height;
};
XPP_SERDE(ShapeTriangle, (base), (height))

/* ── Enum type + external serde ── */

using Shape = xpp::Enum<ShapeCircle, ShapeSquare, ShapeTriangle>;
XPP_ENUM_SERDE(Shape, (ShapeCircle, "circle"), (ShapeSquare, "square"), (ShapeTriangle, "triangle"))

/* ── Enum type + adjacent serde ── */

using AdjShape = xpp::Enum<ShapeCircle, ShapeSquare>;
XPP_ENUM_SERDE_ADJACENT(AdjShape, "tag", "content", (ShapeCircle, "circle"),
                        (ShapeSquare, "square"))

/* ── Struct containing a variant field ── */

struct Drawing {
  xpp::String title;
  Shape       shape = Shape(ShapeCircle{0.0});
};
XPP_SERDE(Drawing, (title), (shape))

/* ── Helpers ── */

namespace {

template <class T> xpp::String to_json(const T &v) {
  xpp::serde::json::Serializer ser;
  auto                         r = xpp::serde::serialize(v, ser);
  EXPECT_TRUE(r.is_ok());
  return ser.to_string();
}

template <class T> xpp::Result<T, xpp::serde::Error> from_json(const xpp::String &json) {
  auto d_r = xpp::serde::json::Deserializer::from_string(json);
  if (!d_r.is_ok()) return xpp::err(d_r.unwrap_err());
  auto d = std::move(d_r).unwrap();
  return xpp::serde::deserialize<T>(d);
}

template <class T> xpp::Vec<uint8_t> to_bin(const T &v) {
  xpp::serde::bin::Serializer ser;
  auto                        r = xpp::serde::serialize(v, ser);
  EXPECT_TRUE(r.is_ok());
  return ser.into_buffer();
}

template <class T> xpp::Result<T, xpp::serde::Error> from_bin(const xpp::Vec<uint8_t> &bytes) {
  auto d_res = xpp::serde::bin::Deserializer::from_bytes(bytes);
  if (!d_res.is_ok()) return xpp::err(d_res.unwrap_err());
  auto d = std::move(d_res).unwrap();
  return xpp::serde::deserialize<T>(d);
}

xpp::String S(const char *s) {
  return xpp::String::from_utf8(s).unwrap();
}

} // namespace

/* ═══ External tagging ═══ */

TEST(SerdeEnumTest, ExternalJsonCircleRoundTrip) {
  Shape v(ShapeCircle{1.0});
  auto  json = to_json(v);
  EXPECT_EQ(json, S(R"({"circle":{"r":1.0}})"));

  auto r = from_json<Shape>(json);
  ASSERT_TRUE(r.is_ok());
  EXPECT_TRUE(r.unwrap().is<ShapeCircle>());
  EXPECT_DOUBLE_EQ(r.unwrap().get<ShapeCircle>().r, 1.0);
}

TEST(SerdeEnumTest, ExternalJsonSquareRoundTrip) {
  Shape v(ShapeSquare{2.5});
  auto  json = to_json(v);
  EXPECT_EQ(json, S(R"({"square":{"s":2.5}})"));

  auto r = from_json<Shape>(json);
  ASSERT_TRUE(r.is_ok());
  EXPECT_TRUE(r.unwrap().is<ShapeSquare>());
  EXPECT_DOUBLE_EQ(r.unwrap().get<ShapeSquare>().s, 2.5);
}

TEST(SerdeEnumTest, ExternalJsonTriangleRoundTrip) {
  Shape v(ShapeTriangle{3.0, 4.0});
  auto  json = to_json(v);
  EXPECT_EQ(json, S(R"({"triangle":{"base":3.0,"height":4.0}})"));

  auto r = from_json<Shape>(json);
  ASSERT_TRUE(r.is_ok());
  EXPECT_TRUE(r.unwrap().is<ShapeTriangle>());
  EXPECT_DOUBLE_EQ(r.unwrap().get<ShapeTriangle>().base, 3.0);
  EXPECT_DOUBLE_EQ(r.unwrap().get<ShapeTriangle>().height, 4.0);
}

TEST(SerdeEnumTest, ExternalBinCircleRoundTrip) {
  Shape v(ShapeCircle{1.0});
  auto  bytes = to_bin(v);
  // binary: u32 tag_index(0) + f64 r(1.0)
  ASSERT_EQ(bytes.len(), 4u + 8u);

  auto r = from_bin<Shape>(bytes);
  ASSERT_TRUE(r.is_ok());
  EXPECT_TRUE(r.unwrap().is<ShapeCircle>());
  EXPECT_DOUBLE_EQ(r.unwrap().get<ShapeCircle>().r, 1.0);
}

TEST(SerdeEnumTest, ExternalBinSquareRoundTrip) {
  Shape v(ShapeSquare{2.0});
  auto  bytes = to_bin(v);
  ASSERT_EQ(bytes.len(), 4u + 8u);

  auto r = from_bin<Shape>(bytes);
  ASSERT_TRUE(r.is_ok());
  EXPECT_TRUE(r.unwrap().is<ShapeSquare>());
  EXPECT_DOUBLE_EQ(r.unwrap().get<ShapeSquare>().s, 2.0);
}

TEST(SerdeEnumTest, ExternalBinTriangleRoundTrip) {
  Shape v(ShapeTriangle{3.0, 4.0});
  auto  bytes = to_bin(v);
  // binary: u32 tag_index(2) + f64 base(3.0) + f64 height(4.0)
  ASSERT_EQ(bytes.len(), 4u + 8u + 8u);

  auto r = from_bin<Shape>(bytes);
  ASSERT_TRUE(r.is_ok());
  EXPECT_TRUE(r.unwrap().is<ShapeTriangle>());
  EXPECT_DOUBLE_EQ(r.unwrap().get<ShapeTriangle>().base, 3.0);
  EXPECT_DOUBLE_EQ(r.unwrap().get<ShapeTriangle>().height, 4.0);
}

TEST(SerdeEnumTest, ExternalUnknownTagJson) {
  auto r = from_json<Shape>(S(R"({"hexagon":{"s":3.0}})"));
  ASSERT_FALSE(r.is_ok());
  EXPECT_EQ(r.unwrap_err().kind, xpp::serde::ErrorKind::UnknownField);
}

TEST(SerdeEnumTest, ExternalUnknownTagBin) {
  // Construct binary with tag_index=99 (out of range)
  xpp::Vec<uint8_t> bytes;
  // u32 LE: 99
  bytes.push(99);
  bytes.push(0);
  bytes.push(0);
  bytes.push(0);
  // f64 1.0 (payload, won't be read)
  uint8_t f64_1[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x3F};
  for (auto b : f64_1)
    bytes.push(b);

  auto r = from_bin<Shape>(bytes);
  ASSERT_FALSE(r.is_ok());
  EXPECT_EQ(r.unwrap_err().kind, xpp::serde::ErrorKind::UnknownField);
}

/* ═══ Adjacent tagging ═══ */

TEST(SerdeEnumTest, AdjacentJsonCircleRoundTrip) {
  AdjShape v(ShapeCircle{1.0});
  auto     json = to_json(v);
  EXPECT_EQ(json, S(R"({"tag":"circle","content":{"r":1.0}})"));

  auto r = from_json<AdjShape>(json);
  ASSERT_TRUE(r.is_ok());
  EXPECT_TRUE(r.unwrap().is<ShapeCircle>());
  EXPECT_DOUBLE_EQ(r.unwrap().get<ShapeCircle>().r, 1.0);
}

TEST(SerdeEnumTest, AdjacentJsonSquareRoundTrip) {
  AdjShape v(ShapeSquare{2.0});
  auto     json = to_json(v);
  EXPECT_EQ(json, S(R"({"tag":"square","content":{"s":2.0}})"));

  auto r = from_json<AdjShape>(json);
  ASSERT_TRUE(r.is_ok());
  EXPECT_TRUE(r.unwrap().is<ShapeSquare>());
  EXPECT_DOUBLE_EQ(r.unwrap().get<ShapeSquare>().s, 2.0);
}

TEST(SerdeEnumTest, AdjacentBinCircleRoundTrip) {
  AdjShape v(ShapeCircle{1.0});
  auto     bytes = to_bin(v);
  // binary adjacent: struct{tag:str, content:struct{r:f64}}
  // str("circle") = u32(6) + 6 bytes = 10
  // struct{r} = f64(1.0) = 8
  // total = 18
  ASSERT_EQ(bytes.len(), 18u);

  auto r = from_bin<AdjShape>(bytes);
  ASSERT_TRUE(r.is_ok());
  EXPECT_TRUE(r.unwrap().is<ShapeCircle>());
  EXPECT_DOUBLE_EQ(r.unwrap().get<ShapeCircle>().r, 1.0);
}

TEST(SerdeEnumTest, AdjacentBinSquareRoundTrip) {
  AdjShape v(ShapeSquare{2.0});
  auto     bytes = to_bin(v);
  auto     r     = from_bin<AdjShape>(bytes);
  ASSERT_TRUE(r.is_ok());
  EXPECT_TRUE(r.unwrap().is<ShapeSquare>());
  EXPECT_DOUBLE_EQ(r.unwrap().get<ShapeSquare>().s, 2.0);
}

TEST(SerdeEnumTest, AdjacentUnknownTagJson) {
  auto r = from_json<AdjShape>(S(R"({"tag":"hexagon","content":{"s":3.0}})"));
  ASSERT_FALSE(r.is_ok());
  EXPECT_EQ(r.unwrap_err().kind, xpp::serde::ErrorKind::UnknownField);
}

/* ═══ Nested variant inside a struct ═══ */

TEST(SerdeEnumTest, NestedEnumInStructJson) {
  Drawing d;
  d.title = S("My Drawing");
  d.shape = Shape(ShapeCircle{3.0});

  auto json = to_json(d);
  auto r    = from_json<Drawing>(json);
  ASSERT_TRUE(r.is_ok());
  EXPECT_EQ(r.unwrap().title, d.title);
  EXPECT_TRUE(r.unwrap().shape.is<ShapeCircle>());
  EXPECT_DOUBLE_EQ(r.unwrap().shape.get<ShapeCircle>().r, 3.0);
}

TEST(SerdeEnumTest, NestedEnumInStructBin) {
  Drawing d;
  d.title = S("My Drawing");
  d.shape = Shape(ShapeTriangle{3.0, 4.0});

  auto bytes = to_bin(d);
  auto r     = from_bin<Drawing>(bytes);
  ASSERT_TRUE(r.is_ok());
  EXPECT_EQ(r.unwrap().title, d.title);
  EXPECT_TRUE(r.unwrap().shape.is<ShapeTriangle>());
  EXPECT_DOUBLE_EQ(r.unwrap().shape.get<ShapeTriangle>().base, 3.0);
  EXPECT_DOUBLE_EQ(r.unwrap().shape.get<ShapeTriangle>().height, 4.0);
}

/* ═══ Cross-backend: JSON → value → binary → value ═══ */

TEST(SerdeEnumTest, CrossBackendJsonToBin) {
  Shape v(ShapeSquare{5.0});
  auto  json = to_json(v);
  auto  r1   = from_json<Shape>(json);
  ASSERT_TRUE(r1.is_ok());

  auto bytes = to_bin(r1.unwrap());
  auto r2    = from_bin<Shape>(bytes);
  ASSERT_TRUE(r2.is_ok());
  EXPECT_TRUE(r2.unwrap().is<ShapeSquare>());
  EXPECT_DOUBLE_EQ(r2.unwrap().get<ShapeSquare>().s, 5.0);
}
