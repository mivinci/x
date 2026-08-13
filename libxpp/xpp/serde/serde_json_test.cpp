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
#include <cstring>
#include <limits>
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
  int32_t     age = 0;
};

} // namespace

namespace xpp {
namespace serde {

template <> struct Serialize<Person> {
  template <class S> static Result<Void, Error> run(const Person &p, S &s) {
    XPP_SERDE_TRY_VAR(scope, s.serialize_struct("Person", 2));
    XPP_SERDE_TRY(scope.field("name", p.name));
    XPP_SERDE_TRY(scope.field("age", p.age));
    return scope.end();
  }
};

template <> struct Deserialize<Person> {
  template <class D> static Result<Person, Error> run(D &d) {
    struct Visitor {
      Result<Person, Error> visit_map(typename D::MapAccess &m) {
        Person p{};
        bool   got_name = false;
        bool   got_age  = false;
        while (true) {
          XPP_SERDE_TRY_VAR(key, m.next_key());
          if (key.is_none()) break;
          const xpp::String &k = key.unwrap();
          if (k == "name") {
            XPP_SERDE_TRY_VAR(v, m.template next_value<xpp::String>());
            p.name   = std::move(v);
            got_name = true;
          } else if (k == "age") {
            XPP_SERDE_TRY_VAR(v, m.template next_value<int32_t>());
            p.age   = v;
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
    static const char *const kFields[] = {"name", "age"};
    return d.deserialize_struct("Person", kFields, 2, Visitor{});
  }
};

} // namespace serde
} // namespace xpp

namespace {

using namespace xpp;
using namespace xpp::serde;

/* ───────────────────── Helpers ───────────────────── */

/** @brief Construct a String from a string literal (assumes valid UTF-8). */
String S(const char *s) {
  return String::from_utf8(s).unwrap();
}

template <class T> String to_json(const T &v) {
  json::Serializer ser;
  auto             r = serde::serialize(v, ser);
  EXPECT_TRUE(r.is_ok()) << "serialize failed";
  return ser.to_string();
}

template <class T> xpp::Result<T, xpp::serde::Error> from_json(const char *s) {
  auto d_res = json::Deserializer::from_string(s);
  if (!d_res.is_ok()) {
    return xpp::err(std::move(d_res).unwrap_err());
  }
  auto d = std::move(d_res).unwrap();
  return serde::deserialize<T>(d);
}

/* ───────────────────── Primitive round-trips ───────────────────── */

TEST(SerdeJsonTest, FreeFunctionToString) {
  // json::to_string(value) is the one-step convenience wrapper.
  auto r = json::to_string(42);
  ASSERT_TRUE(r.is_ok());
  EXPECT_EQ(r.unwrap(), "42");

  Person p{S("Alice"), 30};
  auto   r2 = json::to_string(p);
  ASSERT_TRUE(r2.is_ok());
  EXPECT_EQ(r2.unwrap(), R"({"name":"Alice","age":30})");
}

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
  String      s = to_json<double>(3.14);
  std::string tmp(reinterpret_cast<const char *>(s.as_bytes().data()), s.len());
  auto        r2 = from_json<double>(tmp.c_str());
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
  auto &got = r.unwrap();
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

/* ───────────────────── Nested composite round-trips ─────────────────────
 * The whole point of a serde framework is that composites compose. These
 * tests exercise the trait recursion: Vec<Person>, Option<Person>, and
 * a struct that itself contains a Vec<String>.
 */

struct Team {
  xpp::String      name;
  xpp::Vec<Person> members;
};

} // namespace

namespace xpp {
namespace serde {

template <> struct Serialize<Team> {
  template <class S> static Result<Void, Error> run(const Team &t, S &s) {
    XPP_SERDE_TRY_VAR(scope, s.serialize_struct("Team", 2));
    XPP_SERDE_TRY(scope.field("name", t.name));
    XPP_SERDE_TRY(scope.field("members", t.members));
    return scope.end();
  }
};

template <> struct Deserialize<Team> {
  template <class D> static Result<Team, Error> run(D &d) {
    struct Visitor {
      Result<Team, Error> visit_map(typename D::MapAccess &m) {
        Team t{};
        bool got_name    = false;
        bool got_members = false;
        while (true) {
          XPP_SERDE_TRY_VAR(key, m.next_key());
          if (key.is_none()) break;
          const xpp::String &k = key.unwrap();
          if (k == "name") {
            XPP_SERDE_TRY_VAR(v, m.template next_value<xpp::String>());
            t.name   = std::move(v);
            got_name = true;
          } else if (k == "members") {
            XPP_SERDE_TRY_VAR(v, m.template next_value<xpp::Vec<Person>>());
            t.members   = std::move(v);
            got_members = true;
          } else {
            XPP_SERDE_TRY(m.next_value_ignored());
          }
        }
        if (!got_name) {
          return err(error(ErrorKind::MissingField, "missing 'name'"));
        }
        if (!got_members) {
          return err(error(ErrorKind::MissingField, "missing 'members'"));
        }
        return ok(std::move(t));
      }
    };
    static const char *const kFields[] = {"name", "members"};
    return d.deserialize_struct("Team", kFields, 2, Visitor{});
  }
};

} // namespace serde
} // namespace xpp

namespace {

using namespace xpp;
using namespace xpp::serde;

TEST(SerdeJsonTest, VecOfStructRoundTrip) {
  xpp::Vec<Person> v;
  v.push(Person{S("Alice"), 30});
  v.push(Person{S("Bob"), 25});
  EXPECT_EQ(to_json(v), "[{\"name\":\"Alice\",\"age\":30},{\"name\":\"Bob\",\"age\":25}]");

  auto r =
    from_json<xpp::Vec<Person>>("[{\"name\":\"Alice\",\"age\":30},{\"name\":\"Bob\",\"age\":25}]");
  ASSERT_TRUE(r.is_ok()) << r.unwrap_err().message;
  auto &got = r.unwrap();
  ASSERT_EQ(got.len(), 2u);
  EXPECT_EQ(got[0].name, S("Alice"));
  EXPECT_EQ(got[0].age, 30);
  EXPECT_EQ(got[1].name, S("Bob"));
  EXPECT_EQ(got[1].age, 25);
}

TEST(SerdeJsonTest, OptionOfStructSomeRoundTrip) {
  Option<Person> some(Person{S("Alice"), 30});
  EXPECT_EQ(to_json(some), "{\"name\":\"Alice\",\"age\":30}");

  auto r = from_json<Option<Person>>("{\"name\":\"Alice\",\"age\":30}");
  ASSERT_TRUE(r.is_ok()) << r.unwrap_err().message;
  ASSERT_FALSE(r.unwrap().is_none());
  EXPECT_EQ(r.unwrap().unwrap().name, S("Alice"));
}

TEST(SerdeJsonTest, OptionOfStructNoneRoundTrip) {
  Option<Person> none(xpp::none);
  EXPECT_EQ(to_json(none), "null");

  auto r = from_json<Option<Person>>("null");
  ASSERT_TRUE(r.is_ok());
  EXPECT_TRUE(r.unwrap().is_none());
}

TEST(SerdeJsonTest, NestedStructRoundTrip) {
  Team t{
    S("engineering"),
    xpp::Vec<Person>(),
  };
  t.members.push(Person{S("Alice"), 30});
  t.members.push(Person{S("Bob"), 25});

  String s = to_json(t);
  EXPECT_EQ(s, "{\"name\":\"engineering\","
               "\"members\":[{\"name\":\"Alice\",\"age\":30},"
               "{\"name\":\"Bob\",\"age\":25}]}");

  auto r = from_json<Team>("{\"name\":\"engineering\","
                           "\"members\":[{\"name\":\"Alice\",\"age\":30},"
                           "{\"name\":\"Bob\",\"age\":25}]}");
  ASSERT_TRUE(r.is_ok()) << r.unwrap_err().message;
  Team got = r.unwrap();
  EXPECT_EQ(got.name, S("engineering"));
  ASSERT_EQ(got.members.len(), 2u);
  EXPECT_EQ(got.members[0].name, S("Alice"));
  EXPECT_EQ(got.members[0].age, 30);
  EXPECT_EQ(got.members[1].name, S("Bob"));
  EXPECT_EQ(got.members[1].age, 25);
}

/* ───────────────────── i64 boundary round-trips ───────────────────── */

TEST(SerdeJsonTest, I64BoundaryRoundTrip) {
  EXPECT_EQ(to_json<int64_t>(INT64_MAX), std::to_string(INT64_MAX).c_str());
  EXPECT_EQ(to_json<int64_t>(INT64_MIN), std::to_string(INT64_MIN).c_str());

  auto rmax = from_json<int64_t>(std::to_string(INT64_MAX).c_str());
  ASSERT_TRUE(rmax.is_ok()) << rmax.unwrap_err().message;
  EXPECT_EQ(rmax.unwrap(), INT64_MAX);

  auto rmin = from_json<int64_t>(std::to_string(INT64_MIN).c_str());
  ASSERT_TRUE(rmin.is_ok()) << rmin.unwrap_err().message;
  EXPECT_EQ(rmin.unwrap(), INT64_MIN);
}

/* ───────────────────── f64 special values ─────────────────────
 * JSON has no representation for NaN / Infinity. We refuse to
 * serialize them rather than emit non-conforming JSON. Deserialize
 * also rejects them, since xJson would parse "NaN" as a string.
 */

TEST(SerdeJsonTest, F64NanSerializeFails) {
  json::Serializer ser;
  auto             r = ser.serialize_f64(std::numeric_limits<double>::quiet_NaN());
  ASSERT_FALSE(r.is_ok());
  EXPECT_EQ(r.unwrap_err().kind, xpp::serde::ErrorKind::InvalidValue);
}

TEST(SerdeJsonTest, F64InfinitySerializeFails) {
  json::Serializer ser;
  auto             r = ser.serialize_f64(std::numeric_limits<double>::infinity());
  ASSERT_FALSE(r.is_ok());
  EXPECT_EQ(r.unwrap_err().kind, xpp::serde::ErrorKind::InvalidValue);
}

/* ───────────────────── String with embedded NUL ─────────────────────
 * xpp::String is byte-based and supports embedded NULs. xJson's
 * xJsonNewStringN takes an explicit length so it should preserve them.
 */

TEST(SerdeJsonTest, StringWithEmbeddedNulRoundTrip) {
  // Build "a\0b" as a String
  const char buf[] = {'a', '\0', 'b'};
  auto       s_res = String::from_utf8(buf, 3);
  ASSERT_TRUE(s_res.is_ok());
  String s = s_res.unwrap();
  ASSERT_EQ(s.len(), 3u);

  EXPECT_EQ(to_json<String>(s), "\"a\\u0000b\"");

  auto r = from_json<String>("\"a\\u0000b\"");
  ASSERT_TRUE(r.is_ok()) << r.unwrap_err().message;
  String got = r.unwrap();
  ASSERT_EQ(got.len(), 3u);
  EXPECT_EQ(got.as_bytes()[0], 'a');
  EXPECT_EQ(got.as_bytes()[1], '\0');
  EXPECT_EQ(got.as_bytes()[2], 'b');
}

/* ───────────────────── Serializer reuse ─────────────────────
 * `buffer()` does not invalidate the Serializer; calling it multiple
 * times should return the same string. `reset()` clears internal
 * state so a fresh tree can be emitted.
 */

TEST(SerdeJsonTest, SerializerBufferTwiceIdempotent) {
  json::Serializer ser;
  auto             r = ser.serialize_i32(42);
  ASSERT_TRUE(r.is_ok());
  String a = ser.to_string();
  String b = ser.to_string();
  EXPECT_EQ(a, b);
}

TEST(SerdeJsonTest, SerializerResetClearsState) {
  json::Serializer ser;
  auto             r1 = ser.serialize_i32(42);
  ASSERT_TRUE(r1.is_ok());
  EXPECT_EQ(ser.to_string(), "42");

  ser.reset();

  // After reset, buffer() returns "null" (no root).
  EXPECT_EQ(ser.to_string(), "null");

  // Emit a fresh value.
  auto r2 = ser.serialize_str(S("hello"));
  ASSERT_TRUE(r2.is_ok());
  EXPECT_EQ(ser.to_string(), "\"hello\"");
}

/* ───────────────────── Deserializer move semantics ───────────────────── */

TEST(SerdeJsonTest, DeserializerMoveTransfersOwnership) {
  auto d_res = json::Deserializer::from_string("42");
  ASSERT_TRUE(d_res.is_ok());
  json::Deserializer src = std::move(d_res).unwrap();

  // Sanity check: src works before move.
  auto r1 = src.deserialize_i32();
  ASSERT_TRUE(r1.is_ok());
  EXPECT_EQ(r1.unwrap(), 42);

  // Move src into dst. src is now in a moved-from state; only
  // destruction is safe per C++ moved-from contract.
  json::Deserializer dst = std::move(src);

  // dst holds the parsed tree; it should work.
  auto r2 = dst.deserialize_i32();
  ASSERT_TRUE(r2.is_ok());
  EXPECT_EQ(r2.unwrap(), 42);

  // src is destroyed at end of scope — no crash.
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

} // namespace
