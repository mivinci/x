/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * serde_attrs_test.cpp - Tests for XPP_FIELD_RENAME / DEFAULT / SKIP
 * (Phase 3).
 *
 * Exercises each attribute in isolation and in combination, covering
 * both serialize and deserialize paths.
 */

#include <cstdint>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include <xpp/option.h>
#include <xpp/result.h>
#include <xpp/serde/json.h>
#include <xpp/serde/macros.h>
#include <xpp/string.h>
#include <xpp/vec.h>

/* ───────────────────────── Test fixtures ────────────────────────── */

struct WithRename {
  xpp::String api_key;
  int32_t user_id;
};
XPP_SERDE(WithRename,
  (api_key, XPP_FIELD_RENAME(api_key, "apiKey")),
  (user_id, XPP_FIELD_RENAME(user_id, "userId")))

struct WithDefault {
  xpp::String host;
  int32_t port;
  int32_t retries;
};
XPP_SERDE(WithDefault,
  (host),
  (port,    XPP_FIELD_DEFAULT(port, 8080)),
  (retries, XPP_FIELD_DEFAULT(retries, 3)))

struct WithSkip {
  xpp::String name;
  int32_t age;
  xpp::String internal_cache;  // not serialized
};
XPP_SERDE(WithSkip,
  (name),
  (age),
  (internal_cache, XPP_FIELD_SKIP(internal_cache)))

struct Combined {
  xpp::String public_name;
  int32_t timeout;
  int32_t port;
  xpp::String session_token;
};
XPP_SERDE(Combined,
  (public_name,    XPP_FIELD_RENAME(public_name, "name")),
  (timeout,        XPP_FIELD_DEFAULT(timeout, 30)),
  (port,           XPP_FIELD_DEFAULT(port, 443)),
  (session_token,  XPP_FIELD_SKIP(session_token)))

/* ────────────────────────────── Helpers ────────────────────────────── */

namespace {

using namespace xpp;
using namespace xpp::serde;

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

}  // namespace

/* ═════════════════════════════ Tests ══════════════════════════════ */

/* ── XPP_FIELD_RENAME ─────────────────────────────────────────── */

TEST(SerdeAttrsTest, RenameSerializeOutputsJsonKey) {
  WithRename src;
  src.api_key = S("abc");
  src.user_id = 42;

  String s = to_json(src);
  EXPECT_TRUE(s.contains(S("\"apiKey\":\"abc\"")));
  EXPECT_TRUE(s.contains(S("\"userId\":42")));
  EXPECT_FALSE(s.contains(S("\"api_key\"")));
  EXPECT_FALSE(s.contains(S("\"user_id\"")));
}

TEST(SerdeAttrsTest, RenameDeserializeReadsJsonKey) {
  auto r = from_json<WithRename>("{\"apiKey\":\"xyz\",\"userId\":99}");
  ASSERT_TRUE(r.is_ok());
  EXPECT_EQ(r.unwrap().api_key, S("xyz"));
  EXPECT_EQ(r.unwrap().user_id, 99);
}

TEST(SerdeAttrsTest, RenameMissingJsonKeyFails) {
  auto r = from_json<WithRename>("{\"api_key\":\"x\",\"user_id\":1}");
  ASSERT_FALSE(r.is_ok());
  EXPECT_EQ(r.unwrap_err().kind, ErrorKind::MissingField);
}

TEST(SerdeAttrsTest, RenameRoundTrip) {
  WithRename src;
  src.api_key = S("round");
  src.user_id = 7;
  String s = to_json(src);
  auto r = from_json<WithRename>(
      std::string(reinterpret_cast<const char*>(s.as_bytes().data()),
                  s.len()).c_str());
  ASSERT_TRUE(r.is_ok());
  EXPECT_EQ(r.unwrap().api_key, src.api_key);
  EXPECT_EQ(r.unwrap().user_id, src.user_id);
}

/* ── XPP_FIELD_DEFAULT ───────────────────────────────────────── */

TEST(SerdeAttrsTest, DefaultSerializeOutputsValue) {
  WithDefault src;
  src.host = S("h");
  src.port = 9090;
  src.retries = 5;

  String s = to_json(src);
  EXPECT_TRUE(s.contains(S("\"host\":\"h\"")));
  EXPECT_TRUE(s.contains(S("\"port\":9090")));
  EXPECT_TRUE(s.contains(S("\"retries\":5")));
}

TEST(SerdeAttrsTest, DefaultMissingFieldUsesDefault) {
  auto r = from_json<WithDefault>("{\"host\":\"h\"}");
  ASSERT_TRUE(r.is_ok());
  EXPECT_EQ(r.unwrap().host, S("h"));
  EXPECT_EQ(r.unwrap().port, 8080);
  EXPECT_EQ(r.unwrap().retries, 3);
}

TEST(SerdeAttrsTest, DefaultPresentFieldOverridesDefault) {
  auto r = from_json<WithDefault>("{\"host\":\"h\",\"port\":1,\"retries\":2}");
  ASSERT_TRUE(r.is_ok());
  EXPECT_EQ(r.unwrap().port, 1);
  EXPECT_EQ(r.unwrap().retries, 2);
}

TEST(SerdeAttrsTest, DefaultMissingHostStillFails) {
  // `host` has no default attribute — must be present.
  auto r = from_json<WithDefault>("{\"port\":1}");
  ASSERT_FALSE(r.is_ok());
  EXPECT_EQ(r.unwrap_err().kind, ErrorKind::MissingField);
}

/* ── XPP_FIELD_SKIP ───────────────────────────────────────────── */

TEST(SerdeAttrsTest, SkipSerializeOmitsField) {
  WithSkip src;
  src.name = S("Alice");
  src.age = 30;
  src.internal_cache = S("should_not_appear");

  String s = to_json(src);
  EXPECT_TRUE(s.contains(S("\"name\":\"Alice\"")));
  EXPECT_TRUE(s.contains(S("\"age\":30")));
  EXPECT_FALSE(s.contains(S("internal_cache")));
  EXPECT_FALSE(s.contains(S("should_not_appear")));
}

TEST(SerdeAttrsTest, SkipDeserializeKeepsDefaultConstructed) {
  auto r = from_json<WithSkip>("{\"name\":\"Bob\",\"age\":25}");
  ASSERT_TRUE(r.is_ok());
  EXPECT_EQ(r.unwrap().name, S("Bob"));
  EXPECT_EQ(r.unwrap().age, 25);
  // internal_cache was default-constructed (empty String)
  EXPECT_EQ(r.unwrap().internal_cache, S(""));
}

TEST(SerdeAttrsTest, SkipFieldPresentInJsonIsIgnored) {
  // JSON contains `internal_cache` — should be skipped, not error.
  auto r = from_json<WithSkip>(
      "{\"name\":\"X\",\"age\":1,\"internal_cache\":\"surprise\"}");
  ASSERT_TRUE(r.is_ok());
  EXPECT_EQ(r.unwrap().name, S("X"));
  EXPECT_EQ(r.unwrap().internal_cache, S(""));  // skip wins over JSON
}

/* ── Combined ────────────────────────────────────────────────── */

TEST(SerdeAttrsTest, CombinedSerializeRespectsAllAttrs) {
  Combined src;
  src.public_name = S("svc");
  src.timeout = 60;
  src.port = 8443;
  src.session_token = S("secret");

  String s = to_json(src);
  EXPECT_TRUE(s.contains(S("\"name\":\"svc\"")));
  EXPECT_TRUE(s.contains(S("\"timeout\":60")));
  EXPECT_TRUE(s.contains(S("\"port\":8443")));
  EXPECT_FALSE(s.contains(S("session_token")));
  EXPECT_FALSE(s.contains(S("secret")));
}

TEST(SerdeAttrsTest, CombinedDeserializeAppliesDefaults) {
  auto r = from_json<Combined>("{\"name\":\"svc\"}");
  ASSERT_TRUE(r.is_ok());
  EXPECT_EQ(r.unwrap().public_name, S("svc"));
  EXPECT_EQ(r.unwrap().timeout, 30);
  EXPECT_EQ(r.unwrap().port, 443);
  EXPECT_EQ(r.unwrap().session_token, S(""));
}

TEST(SerdeAttrsTest, CombinedRoundTripWithAllAttrs) {
  Combined src;
  src.public_name = S("app");
  src.timeout = 99;
  src.port = 7000;
  src.session_token = S("ignored");  // not serialized

  String s = to_json(src);
  // session_token is skipped on serialize — deserialized value will be empty
  auto r = from_json<Combined>(
      std::string(reinterpret_cast<const char*>(s.as_bytes().data()),
                  s.len()).c_str());
  ASSERT_TRUE(r.is_ok());
  EXPECT_EQ(r.unwrap().public_name, src.public_name);
  EXPECT_EQ(r.unwrap().timeout, src.timeout);
  EXPECT_EQ(r.unwrap().port, src.port);
  EXPECT_EQ(r.unwrap().session_token, S(""));  // skipped
}
