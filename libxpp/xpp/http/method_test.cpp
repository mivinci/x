/*
 * Unit tests for xpp::http::Method.
 */

#include <gtest/gtest.h>
#include <xpp/http/method.h>

using namespace xpp;
using namespace xpp::http;

/* ───────────────────────────────────────────────────────────────────
 *  to_string
 * ─────────────────────────────────────────────────────────────────── */

TEST(MethodToString, AllVariants) {
  EXPECT_STREQ(to_string(Method::Get), "GET");
  EXPECT_STREQ(to_string(Method::Post), "POST");
  EXPECT_STREQ(to_string(Method::Put), "PUT");
  EXPECT_STREQ(to_string(Method::Delete), "DELETE");
  EXPECT_STREQ(to_string(Method::Patch), "PATCH");
  EXPECT_STREQ(to_string(Method::Head), "HEAD");
  EXPECT_STREQ(to_string(Method::Options), "OPTIONS");
  EXPECT_STREQ(to_string(Method::Trace), "TRACE");
  EXPECT_STREQ(to_string(Method::Connect), "CONNECT");
}

/* ───────────────────────────────────────────────────────────────────
 *  from_string — round trip
 * ─────────────────────────────────────────────────────────────────── */

TEST(MethodFromString, UppercaseRoundTrip) {
  for (Method m : {Method::Get, Method::Post, Method::Put, Method::Delete, Method::Patch,
                   Method::Head, Method::Options, Method::Trace, Method::Connect}) {
    auto r = from_string(String::from_utf8(to_string(m)).unwrap());
    ASSERT_TRUE(r.is_some()) << "to_string=" << to_string(m);
    EXPECT_EQ(r.unwrap(), m);
  }
}

TEST(MethodFromString, CaseInsensitive) {
  EXPECT_EQ(from_string(String::from_utf8("get").unwrap()).unwrap(), Method::Get);
  EXPECT_EQ(from_string(String::from_utf8("Get").unwrap()).unwrap(), Method::Get);
  EXPECT_EQ(from_string(String::from_utf8("GeT").unwrap()).unwrap(), Method::Get);
  EXPECT_EQ(from_string(String::from_utf8("PoSt").unwrap()).unwrap(), Method::Post);
}

TEST(MethodFromString, UnknownReturnsNone) {
  EXPECT_TRUE(from_string(String::from_utf8("").unwrap()).is_none());
  EXPECT_TRUE(from_string(String::from_utf8("foo").unwrap()).is_none());
  EXPECT_TRUE(from_string(String::from_utf8("getpost").unwrap()).is_none());
  // Prefix shouldn't match — "gets" is not Method::Get
  EXPECT_TRUE(from_string(String::from_utf8("gets").unwrap()).is_none());
}

TEST(MethodFromString, NonAsciiReturnsNone) {
  // Non-ASCII byte sequences are rejected.
  EXPECT_TRUE(from_string(String::from_utf8("gét").unwrap()).is_none());
}
