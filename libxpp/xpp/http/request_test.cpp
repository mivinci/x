/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 */

#include <gtest/gtest.h>
#include <xpp/http/request.h>
#include <xpp/string.h>

using namespace xpp;
using namespace xpp::http;

/* ───────────────────────────────────────────────────────────────────
 *  Request construction
 * ─────────────────────────────────────────────────────────────────── */

TEST(HttpRequestTest, DefaultConstructorIsGetEmpty) {
  Request r;
  EXPECT_EQ(r.method(), Method::Get);
  EXPECT_TRUE(r.url().empty());
  EXPECT_EQ(r.headers().size(), 0u);
  EXPECT_FALSE(r.has_body());
}

TEST(HttpRequestTest, BuilderSetsMethod) {
  auto r = Request::builder().method(Method::Post).body().unwrap();
  EXPECT_EQ(r.method(), Method::Post);
}

TEST(HttpRequestTest, BuilderSetsUrlString) {
  auto r =
    Request::builder().url(String::from_utf8("https://example.com").unwrap()).body().unwrap();
  EXPECT_EQ(r.url(), String::from_utf8("https://example.com").unwrap());
}

TEST(HttpRequestTest, BuilderSetsUrlCString) {
  auto r = Request::builder().url("https://example.com").body().unwrap();
  EXPECT_EQ(r.url(), String::from_utf8("https://example.com").unwrap());
}

#if __cpp_lib_string_view
TEST(HttpRequestTest, BuilderSetsUrlStringView) {
  std::string s = "https://example.com";
  auto        r = Request::builder().url(std::string_view(s)).body().unwrap();
  EXPECT_EQ(r.url(), String::from_utf8("https://example.com").unwrap());
}
#endif

TEST(HttpRequestTest, BuilderSetsHeader) {
  auto r = Request::builder()
             .header("Content-Type", "application/json")
             .header("X-Custom", "v1")
             .body()
             .unwrap();
  EXPECT_EQ(r.headers().size(), 2u);
  EXPECT_EQ(r.headers().get("content-type").unwrap(),
            String::from_utf8("application/json").unwrap());
  EXPECT_EQ(r.headers().get("x-custom").unwrap(), String::from_utf8("v1").unwrap());
}

TEST(HttpRequestTest, BuilderHeadersAreCaseInsensitive) {
  auto r = Request::builder().header("Content-Type", "text/plain").body().unwrap();
  EXPECT_EQ(r.headers().get("CONTENT-TYPE").unwrap(), String::from_utf8("text/plain").unwrap());
  EXPECT_EQ(r.headers().get("content-type").unwrap(), String::from_utf8("text/plain").unwrap());
}

/* ───────────────────────────────────────────────────────────────────
 *  Body overloads
 * ─────────────────────────────────────────────────────────────────── */

TEST(HttpRequestTest, BodyFromCString) {
  auto r = Request::builder().body("hello").unwrap();
  EXPECT_TRUE(r.has_body());
}

TEST(HttpRequestTest, BodyFromBytes) {
  auto r = Request::builder().body(Bytes::from("hello")).unwrap();
  EXPECT_TRUE(r.has_body());
}

TEST(HttpRequestTest, BodyFromVecUint8) {
  Vec<uint8_t> v;
  v.push('h');
  v.push('i');
  auto r = Request::builder().body(std::move(v)).unwrap();
  EXPECT_TRUE(r.has_body());
}

TEST(HttpRequestTest, BodyFromString) {
  auto r = Request::builder().body(String::from_utf8("hello").unwrap()).unwrap();
  EXPECT_TRUE(r.has_body());
}

TEST(HttpRequestTest, BodyEmpty) {
  auto r = Request::builder().body().unwrap();
  EXPECT_FALSE(r.has_body());
}

TEST(HttpRequestTest, BodyMovePreservesBytes) {
  auto r = Request::builder().body("hello world").unwrap();
  // Verify body content via into_body() → bytes()
  // (cannot await in non-coroutine test, but we can at least move out)
  Body b = r.into_body();
  EXPECT_FALSE(r.has_body());
  // b is now owner; just verify it's non-empty.
  EXPECT_FALSE(b.is_empty());
}

/* ───────────────────────────────────────────────────────────────────
 *  Method + url + body composition (no convenience terminators)
 * ─────────────────────────────────────────────────────────────────── */

TEST(HttpRequestTest, GetRequest) {
  auto r = Request::builder().method(Method::Get).url("https://example.com").body().unwrap();
  EXPECT_EQ(r.method(), Method::Get);
  EXPECT_EQ(r.url(), String::from_utf8("https://example.com").unwrap());
  EXPECT_FALSE(r.has_body());
}

TEST(HttpRequestTest, PostRequestEmptyBody) {
  auto r = Request::builder().method(Method::Post).url("https://example.com").body().unwrap();
  EXPECT_EQ(r.method(), Method::Post);
}

TEST(HttpRequestTest, PostRequestWithBody) {
  auto r = Request::builder()
             .method(Method::Post)
             .url("https://example.com")
             .body(Body::from("hello"))
             .unwrap();
  EXPECT_EQ(r.method(), Method::Post);
  EXPECT_TRUE(r.has_body());
}

TEST(HttpRequestTest, PutRequest) {
  auto r = Request::builder().method(Method::Put).url("https://example.com").body().unwrap();
  EXPECT_EQ(r.method(), Method::Put);
}

TEST(HttpRequestTest, DeleteRequest) {
  auto r = Request::builder().method(Method::Delete).url("https://example.com").body().unwrap();
  EXPECT_EQ(r.method(), Method::Delete);
}

TEST(HttpRequestTest, PatchRequest) {
  auto r = Request::builder().method(Method::Patch).url("https://example.com").body().unwrap();
  EXPECT_EQ(r.method(), Method::Patch);
}

TEST(HttpRequestTest, HeadRequest) {
  auto r = Request::builder().method(Method::Head).url("https://example.com").body().unwrap();
  EXPECT_EQ(r.method(), Method::Head);
}

/* ───────────────────────────────────────────────────────────────────
 *  Auth helpers
 * ─────────────────────────────────────────────────────────────────── */

TEST(HttpRequestTest, BearerAuthSetsHeader) {
  auto r = Request::builder()
             .url("https://example.com")
             .bearer_auth(String::from_utf8("token123").unwrap())
             .body()
             .unwrap();
  auto h = r.headers().get("authorization");
  ASSERT_TRUE(h.is_some());
  // "Bearer token123"
  EXPECT_EQ(h.unwrap(), String::from_utf8("Bearer token123").unwrap());
}

TEST(HttpRequestTest, BasicAuthSetsBase64Header) {
  // RFC 7617 example: user="Aladdin", password="open sesame"
  // base64("Aladdin:open sesame") = "QWxhZGRpbjpvcGVuIHNlc2FtZQ=="
  auto r =
    Request::builder()
      .url("https://example.com")
      .basic_auth(String::from_utf8("Aladdin").unwrap(), String::from_utf8("open sesame").unwrap())
      .body()
      .unwrap();
  auto h = r.headers().get("authorization");
  ASSERT_TRUE(h.is_some());
  EXPECT_EQ(h.unwrap(), String::from_utf8("Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==").unwrap());
}

/* ───────────────────────────────────────────────────────────────────
 *  Move semantics
 * ─────────────────────────────────────────────────────────────────── */

TEST(HttpRequestTest, MoveConstructorTransfersOwnership) {
  auto    r1 = Request::builder().url("https://example.com").body("hello").unwrap();
  Request r2 = std::move(r1);
  EXPECT_EQ(r2.url(), String::from_utf8("https://example.com").unwrap());
  EXPECT_TRUE(r2.has_body());
}

TEST(HttpRequestTest, MoveAssignmentTransfersOwnership) {
  auto    r1 = Request::builder().url("https://example.com").body("hello").unwrap();
  Request r2;
  r2 = std::move(r1);
  EXPECT_EQ(r2.url(), String::from_utf8("https://example.com").unwrap());
  EXPECT_TRUE(r2.has_body());
}
