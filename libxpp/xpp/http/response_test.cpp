/*
 * response_test.cpp — Unit tests for xpp::http::Response / ResponseBuilder.
 */

#include <string>

#include <gtest/gtest.h>

#include <xpp/http/response.h>

/* ── StringReader — TryRead adapter for string-based test bodies ─── */

struct StringReader {
  std::string   data;
  mutable size_t off = 0;

  ssize_t try_read(char *buf, size_t cap) {
    if (off >= data.size()) return 0;
    size_t n = std::min(cap, data.size() - off);
    std::memcpy(buf, data.data() + off, n);
    off += n;
    return static_cast<ssize_t>(n);
  }
};

/* ═══════════════════════════════════════════════════════════════════
 *  ResponseBuilder — build() with TryRead body
 * ═══════════════════════════════════════════════════════════════════ */

TEST(ResponseBuilderTest, BuilderBuild) {
  auto resp = xpp::http::Response::builder()
    .status(201)
    .header("Content-Type", "application/json")
    .body(StringReader{R"({"ok":true})"})
    .build();

  EXPECT_EQ(resp.status(), 201);
  EXPECT_EQ(resp.header("Content-Type").unwrap(), "application/json");
  EXPECT_TRUE(resp.has_body());
}

TEST(ResponseBuilderTest, MultipleHeaders) {
  auto resp = xpp::http::Response::builder()
    .header("X-A", "1")
    .header("X-B", "2")
    .body(StringReader{""})
    .build();

  EXPECT_EQ(resp.header("X-A").unwrap(), "1");
  EXPECT_EQ(resp.header("X-B").unwrap(), "2");
  EXPECT_TRUE(resp.header("X-C").is_none());
}

TEST(ResponseBuilderTest, IntoPromise) {
  auto promise = xpp::http::Response::builder()
    .status(200)
    .body(StringReader{"async"})
    .into_promise();
  EXPECT_TRUE(static_cast<bool>(promise));
}

TEST(ResponseBuilderTest, EmptyBody) {
  auto resp = xpp::http::Response::builder()
    .status(204)
    .body(StringReader{""})
    .build();

  EXPECT_EQ(resp.status(), 204);
}

TEST(ResponseBuilderTest, DefaultStatus) {
  auto resp = xpp::http::Response::builder()
    .body(StringReader{"default"})
    .build();

  EXPECT_EQ(resp.status(), 200);
}

TEST(ResponseBuilderTest, NoBodyStatusOnly) {
  auto resp = xpp::http::Response::builder()
    .status(418)
    .build();

  EXPECT_EQ(resp.status(), 418);
  EXPECT_FALSE(resp.has_body());
}

/* ═══════════════════════════════════════════════════════════════════
 *  Response accessors (read-only after build)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(ResponseTest, StatusAccessor) {
  auto resp = xpp::http::Response::builder()
    .status(200)
    .body(StringReader{"x"})
    .build();
  EXPECT_EQ(resp.status(), 200);
}

TEST(ResponseTest, HeaderCaseInsensitive) {
  auto resp = xpp::http::Response::builder()
    .header("Content-Type", "text/plain")
    .body(StringReader{""})
    .build();

  // Keys are lowercased — lookup is case-insensitive.
  EXPECT_TRUE(resp.header("content-type").is_some());
  EXPECT_EQ(resp.header("Content-Type").unwrap(), "text/plain");
  EXPECT_EQ(resp.header("CONTENT-TYPE").unwrap(), "text/plain");
}

TEST(ResponseTest, BuilderReturnsBuilder) {
  auto resp = xpp::http::Response::builder()
    .status(302)
    .body(StringReader{"redirect"})
    .build();

  EXPECT_EQ(resp.status(), 302);
}
