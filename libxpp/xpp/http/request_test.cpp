/*
 * request_test.cpp — Tests for xpp::http::Request / RequestBuilder.
 */

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <xpp/bytes/bytes.h>
#include <xpp/bytes/reader.h>
#include <xpp/http/request.h>

using namespace xpp::http;

// ── TryRead test helpers ─────────────────────────────────────────

struct StringReader {
  std::string data;
  mutable size_t off = 0;

  ssize_t try_read(char *buf, size_t cap) {
    if (off >= data.size()) return 0;
    size_t n = std::min(cap, data.size() - off);
    std::memcpy(buf, data.data() + off, n);
    off += n;
    return static_cast<ssize_t>(n);
  }
};

static xpp::bytes::Reader make_reader(const char *s) {
  return xpp::bytes::Reader(xpp::bytes::Bytes::copy(
    reinterpret_cast<const uint8_t *>(s), std::strlen(s)));
}

static xpp::bytes::Reader make_reader(const std::vector<uint8_t> &v) {
  return xpp::bytes::Reader(xpp::bytes::Bytes::from(std::vector<uint8_t>(v)));
}

/* ═══════════════════════════════════════════════════════════════════
 *  Basic construction
 * ═══════════════════════════════════════════════════════════════════ */

TEST(RequestTest, GetMethod) {
  auto result = Request::builder()
                  .method(Method::Get)
                  .url("http://x")
                  .body()
                  .unwrap();
  EXPECT_EQ(result.method(), Method::Get);
  EXPECT_EQ(result.url(), "http://x");
  EXPECT_FALSE(result.has_body());
}

TEST(RequestTest, PostMethod) {
  auto result = Request::builder()
                  .method(Method::Post)
                  .url("http://x")
                  .body(make_reader("hello"))
                  .unwrap();
  EXPECT_EQ(result.method(), Method::Post);
  EXPECT_EQ(result.url(), "http://x");
  EXPECT_TRUE(result.has_body());
}

TEST(RequestTest, PutMethod) {
  auto result = Request::builder()
                  .method(Method::Put)
                  .url("http://x")
                  .body()
                  .unwrap();
  EXPECT_EQ(result.method(), Method::Put);
}

TEST(RequestTest, DeleteMethod) {
  auto result = Request::builder()
                  .method(Method::Delete)
                  .url("http://x")
                  .body()
                  .unwrap();
  EXPECT_EQ(result.method(), Method::Delete);
}

TEST(RequestTest, PatchMethod) {
  auto result = Request::builder()
                  .method(Method::Patch)
                  .url("http://x")
                  .body()
                  .unwrap();
  EXPECT_EQ(result.method(), Method::Patch);
}

TEST(RequestTest, HeadMethod) {
  auto result = Request::builder()
                  .method(Method::Head)
                  .url("http://x")
                  .body()
                  .unwrap();
  EXPECT_EQ(result.method(), Method::Head);
}

TEST(RequestTest, DefaultMethodIsGet) {
  auto result = Request::builder()
                  .url("http://x")
                  .body()
                  .unwrap();
  EXPECT_EQ(result.method(), Method::Get);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Headers
 * ═══════════════════════════════════════════════════════════════════ */

TEST(RequestTest, Headers) {
  auto result = Request::builder()
                  .method(Method::Post)
                  .url("http://x")
                  .header("Content-Type", "application/json")
                  .header("X-Custom", "value1")
                  .header("X-Custom", "value2")
                  .body(make_reader("{}"))
                  .unwrap();

  EXPECT_EQ(result.method(), Method::Post);

  // Keys are lowercased (case-insensitive).  Both X-Custom headers are stored as "x-custom".
  auto &hdrs = result.headers();
  int   count = 0;
  for (auto &v : hdrs.get_all("x-custom"))
    ++count;
  EXPECT_EQ(count, 2);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Body variants
 * ═══════════════════════════════════════════════════════════════════ */

TEST(RequestTest, BodyEmpty) {
  auto result = Request::builder()
                  .method(Method::Post)
                  .url("http://x")
                  .body()
                  .unwrap();
  EXPECT_FALSE(result.has_body());
}

TEST(RequestTest, BodyStringReader) {
  auto result = Request::builder()
                  .method(Method::Post)
                  .url("http://x")
                  .body(make_reader("hello world"))
                  .unwrap();
  EXPECT_TRUE(result.has_body());

  // read back the body
  auto fn = std::move(result.take_try_read()).unwrap_unchecked();
  char buf[64];
  ssize_t n = fn(buf, sizeof(buf));
  EXPECT_GT(n, 0);
  EXPECT_EQ(std::string(buf, static_cast<size_t>(n)), "hello world");
}

TEST(RequestTest, BodyBinaryReader) {
  std::vector<uint8_t> data = {0x00, 0x01, 0xFE, 0xFF};
  auto result = Request::builder()
                  .method(Method::Post)
                  .url("http://x")
                  .body(make_reader(data))
                  .unwrap();
  EXPECT_TRUE(result.has_body());

  auto fn = std::move(result.take_try_read()).unwrap_unchecked();
  char buf[16];
  ssize_t n = fn(buf, sizeof(buf));
  EXPECT_EQ(static_cast<size_t>(n), data.size());
}

TEST(RequestTest, BodyCustomTryRead) {
  auto result = Request::builder()
                  .method(Method::Post)
                  .url("http://x")
                  .body(StringReader{"custom-body"})
                  .unwrap();
  EXPECT_TRUE(result.has_body());

  auto fn = std::move(result.take_try_read()).unwrap_unchecked();
  char buf[32];
  ssize_t n = fn(buf, sizeof(buf));
  EXPECT_GT(n, 0);
  EXPECT_EQ(std::string(buf, static_cast<size_t>(n)), "custom-body");
}

/* ═══════════════════════════════════════════════════════════════════
 *  Terminal — body() ends chaining
 * ═══════════════════════════════════════════════════════════════════ */

TEST(RequestTest, BodyStopsChaining) {
  auto result = Request::builder()
                  .url("/")
                  .body()
                  .unwrap();
  EXPECT_EQ(result.method(), Method::Get);
  EXPECT_FALSE(result.has_body());
}

/* ═══════════════════════════════════════════════════════════════════
 *  Builder reuse
 * ═══════════════════════════════════════════════════════════════════ */

TEST(RequestTest, BuilderCanBeReused) {
  auto builder = Request::builder();
  auto a = builder.method(Method::Get).url("/a").body().unwrap();
  EXPECT_EQ(a.method(), Method::Get);
  EXPECT_EQ(a.url(), "/a");
}
