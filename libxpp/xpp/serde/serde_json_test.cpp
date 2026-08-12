/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * serde_json_test.cpp - Tests for the JSON backend + hand-written
 * struct specializations (Phase 1 reference example).
 *
 * The Person struct below is the canonical "how to specialize
 * Serialize<T> / Deserialize<T> by hand" example. Later phases
 * (XPP_SERDE macro) generate equivalent code mechanically.
 */

#include <cstdint>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include <xpp/option.h>
#include <xpp/result.h>
#include <xpp/serde/json.h>
#include <xpp/serde/serde.h>
#include <xpp/string.h>
#include <xpp/vec.h>
#include <xpp/void.h>

namespace {

/* ─────────── Reference example: hand-written Person specialization ─────────── */

struct Person {
  xpp::String name;
  int32_t age = 0;
};

}  // namespace

namespace xpp {
namespace serde {

template <>
struct Serialize<Person> {
  template <class S>
  static Result<Void, Error> run(const Person& p, S& s) {
    XPP_SERDE_TRY_VAR(scope, s.serialize_struct("Person", 2));
    XPP_SERDE_TRY(scope.field("name", p.name));
    XPP_SERDE_TRY(scope.field("age", p.age));
    return scope.end();
  }
};

template <>
struct Deserialize<Person> {
  template <class D>
  static Result<Person, Error> run(D& d) {
    struct Visitor {
      Result<Person, Error> visit_map(typename D::MapAccess& m) {
        Person p{};
        bool got_name = false;
        bool got_age = false;
        while (true) {
          XPP_SERDE_TRY_VAR(key, m.next_key());
          if (key.is_none()) break;
          const xpp::String& k = key.unwrap();
          if (k == "name") {
            XPP_SERDE_TRY_VAR(v, m.template next_value<xpp::String>());
            p.name = std::move(v);
            got_name = true;
          } else if (k == "age") {
            XPP_SERDE_TRY_VAR(v, m.template next_value<int32_t>());
            p.age = v;
            got_age = true;
          } else {
            XPP_SERDE_TRY(m.next_value_ignored());
          }
        }
        if (!got_name) {
          return err(error(ErrorKind::MissingField, "missing 'name'"));
        }
        if (!got_age) {
          return err(error(ErrorKind::MissingField, "missing 'age'"));
        }
        return ok(std::move(p));
      }
    };
    static const char* const kFields[] = {"name", "age"};
    return d.deserialize_struct("Person", kFields, 2, Visitor{});
  }
};

}  // namespace serde
}  // namespace xpp

namespace {

using namespace xpp;
using namespace xpp::serde;

/* ───────────────────── Helpers ───────────────────── */

/** @brief Construct a String from a string literal (assumes valid UTF-8). */
String S(const char* s) {
  return String::from_utf8(s).unwrap();
}

template <class T>
String to_json(const T& v) {
  json::Serializer ser;
  auto r = serde::serialize(v, ser);
  EXPECT_TRUE(r.is_ok()) << "serialize failed";
  return ser.buffer();
}

template <class T>
xpp::Result<T, xpp::serde::Error> from_json(const char* s) {
  auto d_res = json::Deserializer::from_string(s);
  if (!d_res.is_ok()) {
    return xpp::err(std::move(d_res).unwrap_err());
  }
  auto d = std::move(d_res).unwrap();
  return serde::deserialize<T>(d);
}

/* ───────────────────── Primitive round-trips ───────────────────── */

TEST(SerdeJsonTest, BoolRoundTrip) {
  EXPECT_EQ(to_json(true), "true");
  EXPECT_EQ(to_json(false), "false");
  auto r = from_json<bool>("true");
  ASSERT_TRUE(r.is_ok());
  EXPECT_EQ(r.unwrap(), true);
}

TEST(SerdeJsonTest, I32RoundTrip) {
  EXPECT_EQ(to_json<int32_t>(42), "42");
  EXPECT_EQ(to_json<int32_t>(-1), "-1");
  EXPECT_EQ(to_json<int32_t>(INT32_MIN), std::to_string(INT32_MIN).c_str());
  auto r = from_json<int32_t>("-12345");
  ASSERT_TRUE(r.is_ok());
  EXPECT_EQ(r.unwrap(), -12345);
}

TEST(SerdeJsonTest, I64RoundTrip) {
  int64_t big = static_cast<int64_t>(INT32_MAX) + 100;
  EXPECT_EQ(to_json<int64_t>(big), std::to_string(big).c_str());
  auto r = from_json<int64_t>(std::to_string(big).c_str());
  ASSERT_TRUE(r.is_ok());
  EXPECT_EQ(r.unwrap(), big);
}

TEST(SerdeJsonTest, U32RoundTrip) {
  EXPECT_EQ(to_json<uint32_t>(7u), "7");
  auto r = from_json<uint32_t>("42");
  ASSERT_TRUE(r.is_ok());
  EXPECT_EQ(r.unwrap(), 42u);
}

TEST(SerdeJsonTest, F64RoundTrip) {
  // 2.5 has an exact binary representation, so it round-trips cleanly.
  EXPECT_EQ(to_json<double>(2.5), "2.5");
  auto r = from_json<double>("2.5");
  ASSERT_TRUE(r.is_ok());
  EXPECT_DOUBLE_EQ(r.unwrap(), 2.5);

  // 3.14 is not exactly representable; verify round-trip fidelity via
  // parse(serialize(x)) == x rather than a literal string compare.
  String s = to_json<double>(3.14);
  std::string tmp(reinterpret_cast<const char*>(s.as_bytes().data()), s.len());
  auto r2 = from_json<double>(tmp.c_str());
  ASSERT_TRUE(r2.is_ok());
  EXPECT_DOUBLE_EQ(r2.unwrap(), 3.14);
}

TEST(SerdeJsonTest, StringRoundTrip) {
  EXPECT_EQ(to_json<String>(S("hello")), "\"hello\"");
  EXPECT_EQ(to_json<String>(S("a\"b\nc")), "\"a\\\"b\\nc\"");
  auto r = from_json<String>("\"world\"");
  ASSERT_TRUE(r.is_ok());
  EXPECT_EQ(r.unwrap(), S("world"));
}

/* ───────────────────── Option round-trips ───────────────────── */

TEST(SerdeJsonTest, OptionSomeRoundTrip) {
  EXPECT_EQ(to_json<Option<int32_t>>(Option<int32_t>(7)), "7");
  auto r = from_json<Option<int32_t>>("7");
  ASSERT_TRUE(r.is_ok());
  ASSERT_FALSE(r.unwrap().is_none());
  EXPECT_EQ(r.unwrap().unwrap(), 7);
}

TEST(SerdeJsonTest, OptionNoneRoundTrip) {
  EXPECT_EQ(to_json<Option<int32_t>>(Option<int32_t>(xpp::none)), "null");
  auto r = from_json<Option<int32_t>>("null");
  ASSERT_TRUE(r.is_ok());
  EXPECT_TRUE(r.unwrap().is_none());
}

/* ───────────────────── Vec round-trips ───────────────────── */

TEST(SerdeJsonTest, VecRoundTrip) {
  xpp::Vec<int32_t> v;
  v.push(1);
  v.push(2);
  v.push(3);
  EXPECT_EQ(to_json(v), "[1,2,3]");

  auto r = from_json<xpp::Vec<int32_t>>("[10,20,30]");
  ASSERT_TRUE(r.is_ok());
  auto& got = r.unwrap();
  ASSERT_EQ(got.len(), 3u);
  EXPECT_EQ(got[0], 10);
  EXPECT_EQ(got[1], 20);
  EXPECT_EQ(got[2], 30);
}

TEST(SerdeJsonTest, VecEmptyRoundTrip) {
  xpp::Vec<int32_t> v;
  EXPECT_EQ(to_json(v), "[]");
  auto r = from_json<xpp::Vec<int32_t>>("[]");
  ASSERT_TRUE(r.is_ok());
  EXPECT_EQ(r.unwrap().len(), 0u);
}

/* ───────────────────── Struct round-trip ───────────────────── */

TEST(SerdeJsonTest, PersonRoundTrip) {
  Person p{S("Alice"), 30};
  EXPECT_EQ(to_json(p), "{\"name\":\"Alice\",\"age\":30}");

  auto r = from_json<Person>("{\"name\":\"Bob\",\"age\":25}");
  ASSERT_TRUE(r.is_ok()) << r.unwrap_err().message;
  EXPECT_EQ(r.unwrap().name, S("Bob"));
  EXPECT_EQ(r.unwrap().age, 25);
}

TEST(SerdeJsonTest, PersonUnknownFieldSkipped) {
  auto r = from_json<Person>("{\"name\":\"Alice\",\"age\":30,\"extra\":\"x\"}");
  ASSERT_TRUE(r.is_ok()) << r.unwrap_err().message;
  EXPECT_EQ(r.unwrap().name, S("Alice"));
  EXPECT_EQ(r.unwrap().age, 30);
}

/* ───────────────────── Error cases ───────────────────── */

TEST(SerdeJsonTest, MissingFieldFails) {
  auto r = from_json<Person>("{\"name\":\"Alice\"}");
  ASSERT_FALSE(r.is_ok());
  EXPECT_EQ(r.unwrap_err().kind, xpp::serde::ErrorKind::MissingField);
}

TEST(SerdeJsonTest, InvalidValueFails) {
  auto r = from_json<int32_t>("\"hello\"");
  ASSERT_FALSE(r.is_ok());
  EXPECT_EQ(r.unwrap_err().kind, xpp::serde::ErrorKind::InvalidValue);
}

TEST(SerdeJsonTest, MalformedJsonFails) {
  auto d_res = json::Deserializer::from_string("{not json");
  ASSERT_FALSE(d_res.is_ok());
  EXPECT_EQ(d_res.unwrap_err().kind, xpp::serde::ErrorKind::Unexpected);
}

TEST(SerdeJsonTest, StructFromNonObjectFails) {
  auto r = from_json<Person>("[1,2,3]");
  ASSERT_FALSE(r.is_ok());
  EXPECT_EQ(r.unwrap_err().kind, xpp::serde::ErrorKind::InvalidValue);
}

TEST(SerdeJsonTest, SeqFromNonArrayFails) {
  auto r = from_json<xpp::Vec<int32_t>>("42");
  ASSERT_FALSE(r.is_ok());
  EXPECT_EQ(r.unwrap_err().kind, xpp::serde::ErrorKind::InvalidValue);
}

}  // namespace
