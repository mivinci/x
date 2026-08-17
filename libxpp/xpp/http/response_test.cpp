/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 */

#include <gtest/gtest.h>
#include <xpp/http/response.h>

using namespace xpp;
using namespace xpp::http;

/* ───────────────────────────────────────────────────────────────────
 *  Default construction
 * ─────────────────────────────────────────────────────────────────── */

TEST(HttpResponseTest, DefaultConstructorIsOkEmpty) {
  Response r;
  EXPECT_EQ(r.status(), StatusCode::Ok);
  EXPECT_EQ(r.status_code(), 200u);
  EXPECT_EQ(r.headers().size(), 0u);
  EXPECT_FALSE(r.has_body());
  EXPECT_TRUE(r.url().is_none());
}

/* ───────────────────────────────────────────────────────────────────
 *  Builder
 * ─────────────────────────────────────────────────────────────────── */

TEST(HttpResponseBuilderTest, SetsStatus) {
  Response r = Response::builder().status(StatusCode::NotFound).body();
  EXPECT_EQ(r.status(), StatusCode::NotFound);
  EXPECT_EQ(r.status_code(), 404u);
}

TEST(HttpResponseBuilderTest, SetsStatusByUint16) {
  Response r = Response::builder().status(503).body();
  EXPECT_EQ(r.status(), StatusCode::ServiceUnavailable);
}

TEST(HttpResponseBuilderTest, SetsHeaders) {
  Response r =
    Response::builder().header("Content-Type", "text/plain").header("X-Custom", "v1").body();
  EXPECT_EQ(r.headers().size(), 2u);
  EXPECT_EQ(r.headers().get("content-type").unwrap(), String::from_utf8("text/plain").unwrap());
}

TEST(HttpResponseBuilderTest, BodyOverloads) {
  Response r1 = Response::builder().body("hello");
  EXPECT_TRUE(r1.has_body());

  Response r2 = Response::builder().body(Bytes::from("hello"));
  EXPECT_TRUE(r2.has_body());

  Response r3 = Response::builder().body();
  EXPECT_FALSE(r3.has_body());
}

TEST(HttpResponseBuilderTest, HeaderLookupCaseInsensitive) {
  Response r = Response::builder().header("Content-Type", "text/plain").body();
  EXPECT_EQ(r.header("CONTENT-TYPE").unwrap(), String::from_utf8("text/plain").unwrap());
  EXPECT_TRUE(r.header("x-missing").is_none());
}

/* ───────────────────────────────────────────────────────────────────
 *  Static convenience constructors
 * ─────────────────────────────────────────────────────────────────── */

TEST(HttpResponseStaticTest, OkWithBody) {
  Response r = ResponseBuilder::ok("hello");
  EXPECT_EQ(r.status(), StatusCode::Ok);
  EXPECT_TRUE(r.has_body());
}

TEST(HttpResponseStaticTest, OkEmpty) {
  Response r = ResponseBuilder::ok();
  EXPECT_EQ(r.status(), StatusCode::Ok);
  EXPECT_FALSE(r.has_body());
}

TEST(HttpResponseStaticTest, Created) {
  Response r = ResponseBuilder::created("id=42");
  EXPECT_EQ(r.status(), StatusCode::Created);
  EXPECT_TRUE(r.has_body());
}

TEST(HttpResponseStaticTest, NoContent) {
  Response r = ResponseBuilder::no_content();
  EXPECT_EQ(r.status(), StatusCode::NoContent);
  EXPECT_FALSE(r.has_body());
}

TEST(HttpResponseStaticTest, BadRequest) {
  Response r = ResponseBuilder::bad_request("bad input");
  EXPECT_EQ(r.status(), StatusCode::BadRequest);
  EXPECT_TRUE(r.has_body());
}

TEST(HttpResponseStaticTest, NotFound) {
  Response r = ResponseBuilder::not_found();
  EXPECT_EQ(r.status(), StatusCode::NotFound);
}

TEST(HttpResponseStaticTest, InternalServerError) {
  Response r = ResponseBuilder::internal_server_error("oops");
  EXPECT_EQ(r.status(), StatusCode::InternalServerError);
  EXPECT_TRUE(r.has_body());
}

/* ───────────────────────────────────────────────────────────────────
 *  Body ownership
 * ─────────────────────────────────────────────────────────────────── */

TEST(HttpResponseBodyTest, IntoBodyEmptiesResponse) {
  Response r = ResponseBuilder::ok("hello");
  ASSERT_TRUE(r.has_body());
  Body b = r.into_body();
  EXPECT_FALSE(r.has_body());
  EXPECT_FALSE(b.is_empty());
}

TEST(HttpResponseBodyTest, BodyAccessorBorrows) {
  Response r = ResponseBuilder::ok("hello");
  Body    &b = r.body();
  EXPECT_FALSE(b.is_empty());
  // Response still owns the body.
  EXPECT_TRUE(r.has_body());
}

/* ───────────────────────────────────────────────────────────────────
 *  Move semantics
 * ─────────────────────────────────────────────────────────────────── */

TEST(HttpResponseMoveTest, MoveConstructorTransfersOwnership) {
  Response r1 = ResponseBuilder::ok("hello");
  Response r2 = std::move(r1);
  EXPECT_EQ(r2.status(), StatusCode::Ok);
  EXPECT_TRUE(r2.has_body());
}

TEST(HttpResponseMoveTest, MoveAssignmentTransfersOwnership) {
  Response r1 = ResponseBuilder::ok("hello");
  Response r2;
  r2 = std::move(r1);
  EXPECT_EQ(r2.status(), StatusCode::Ok);
  EXPECT_TRUE(r2.has_body());
}
