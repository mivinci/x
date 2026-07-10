/*
 * client_test.cpp — Tests for xpp::http::Client.
 *
 * Uses TestServer (Go httptest-style) for server setup.
 */

#include <string>

#include <gtest/gtest.h>
#include <xpp/bytes/bytes.h>
#include <xpp/bytes/reader.h>
#include <xpp/event.h>
#include <xpp/http/client.h>
#include <xpp/http/test_server.h>
#include <xpp/promise.h>

using namespace xpp::http;

/* ── Helpers ──────────────────────────────────────────────────────── */

/// TryRead adapter for string-based test bodies.
struct StringReader {
  std::string    data;
  mutable size_t off = 0;

  ssize_t try_read(char *buf, size_t cap) {
    if (off >= data.size()) return 0;
    size_t n = std::min(cap, data.size() - off);
    std::memcpy(buf, data.data() + off, n);
    off += n;
    return static_cast<ssize_t>(n);
  }
};

/// Helper: drain a TryRead body into a std::string.
template <class R>
static std::string drain_body(R &&reader) {
  std::string s;
  char        buf[4096];
  ssize_t     n;
  while ((n = reader.try_read(buf, sizeof(buf))) > 0) s.append(buf, (size_t)n);
  return s;
}

/* ── Fixture ──────────────────────────────────────────────────────── */

class HttpClientTest : public ::testing::Test {
protected:
  xpp::EventLoop m_loop;
  xpp::WaitScope m_scope{m_loop};
};


/* ═══════════════════════════════════════════════════════════════════
 *  Lifecycle
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(HttpClientTest, CreateClient) {
  auto client = xpp::http::Client::builder().build();
  SUCCEED();
}

/* ═══════════════════════════════════════════════════════════════════
 *  GET request
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(HttpClientTest, GetHelloWorld) {
  xpp::http::TestServer ts;
  ts.router().route("GET /hello", [](xpp::http::IncomingRequest &req) {
    EXPECT_EQ(req.method(), "GET");
    EXPECT_EQ(req.path(), "/hello");
    return xpp::http::Response::builder()
      .status(200)
      .body(StringReader{"Hello, xpp!"})
      .into_promise();
  });
  ts.start();

  auto client = xpp::http::Client::builder().build();
  auto req    = Request::builder().method(Method::Get).url(ts.url("/hello")).body().unwrap();
  auto result = client.send(std::move(req)).await();

  ASSERT_TRUE(result.is_ok()) << "GET /hello failed";
  auto &&response = result.unwrap();
  EXPECT_EQ(response.status(), 200);

  auto body = response.text().await();
  ASSERT_TRUE(body.is_ok());
  EXPECT_EQ(body.unwrap(), "Hello, xpp!");
}

/* ═══════════════════════════════════════════════════════════════════
 *  POST request with body echo
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(HttpClientTest, DISABLED_PostEcho) {
  xpp::http::TestServer ts;
  ts.router().route("POST /echo", [](xpp::http::IncomingRequest &req) {
    auto body = drain_body(req.body());
    return xpp::http::Response::builder()
      .status(200)
      .header("Content-Type", "application/octet-stream")
      .body(StringReader{std::move(body)})
      .into_promise();
  });
  ts.start();

  auto        client  = xpp::http::Client::builder().build();
  std::string payload = "echo this back";
  auto        req     = Request::builder()
               .method(Method::Post)
               .url(ts.url("/echo"))
               .header("Content-Length", std::to_string(payload.size()))
               .body(xpp::bytes::Reader(xpp::bytes::Bytes::copy(
                 reinterpret_cast<const uint8_t *>(payload.data()), payload.size())))
               .unwrap();
  auto result = client.send(std::move(req)).await();

  ASSERT_TRUE(result.is_ok());
  auto &&response = result.unwrap();
  EXPECT_EQ(response.status(), 200);

  auto body = response.text().await();
  ASSERT_TRUE(body.is_ok());
  EXPECT_EQ(body.unwrap(), payload);
}

/* ═══════════════════════════════════════════════════════════════════
 *  HTTP status code forwarding
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(HttpClientTest, StatusCode) {
  xpp::http::TestServer ts;
  ts.router().route("/status/404", [](xpp::http::IncomingRequest &req) {
    int code = 200;
    // Parse from path: "/status/404" → 404
    const std::string &p = req.path();
    auto               s = p.rfind('/');
    if (s != std::string::npos) code = std::stoi(p.substr(s + 1));
    if (code < 100 || code >= 600) code = 200;
    return xpp::http::Response::builder().status(code).into_promise();
  });
  ts.start();

  auto client = xpp::http::Client::builder().build();
  auto req    = Request::builder().method(Method::Get).url(ts.url("/status/404")).body().unwrap();
  auto result = client.send(std::move(req)).await();

  ASSERT_TRUE(result.is_ok());
  EXPECT_EQ(result.unwrap().status(), 404);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Empty body response
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(HttpClientTest, EmptyBody) {
  xpp::http::TestServer ts;
  ts.router().route("/empty", [](xpp::http::IncomingRequest &) {
    return xpp::http::Response::builder().status(200).into_promise();
  });
  ts.start();

  auto client = xpp::http::Client::builder().build();
  auto req    = Request::builder().method(Method::Get).url(ts.url("/empty")).body().unwrap();
  auto result = client.send(std::move(req)).await();

  ASSERT_TRUE(result.is_ok());
  auto &&response = result.unwrap();
  EXPECT_EQ(response.status(), 200);

  auto body = response.text().await();
  ASSERT_TRUE(body.is_ok());
  EXPECT_EQ(body.unwrap(), "");
}

/* ═══════════════════════════════════════════════════════════════════
 *  Streaming body access (chunked via TryRead)
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(HttpClientTest, StreamingBodyViaText) {
  xpp::http::TestServer ts;
  ts.router().route("/chunked", [](xpp::http::IncomingRequest &) {
    return xpp::http::Response::builder()
      .status(200)
      .header("Content-Type", "text/plain")
      .body(StringReader{"chunk-1 chunk-2 chunk-3"})
      .into_promise();
  });
  ts.start();

  auto client = xpp::http::Client::builder().build();
  auto req    = Request::builder().method(Method::Get).url(ts.url("/chunked")).body().unwrap();
  auto result = client.send(std::move(req)).await();

  ASSERT_TRUE(result.is_ok());
  auto &&response = result.unwrap();
  EXPECT_EQ(response.status(), 200);

  auto body = response.text().await();
  ASSERT_TRUE(body.is_ok());
  EXPECT_EQ(body.unwrap(), "chunk-1 chunk-2 chunk-3");
}

/* ═══════════════════════════════════════════════════════════════════
 *  POST with binary body echo
 * ═══════════════════════════════════════════════════════════════════ */

TEST_F(HttpClientTest, DISABLED_PostBinaryBody) {
  xpp::http::TestServer ts;
  ts.router().route("POST /echo-bin", [](xpp::http::IncomingRequest &req) {
    auto body = drain_body(req.body());
    return xpp::http::Response::builder()
      .status(200)
      .header("Content-Type", "application/octet-stream")
      .body(StringReader{std::move(body)})
      .into_promise();
  });
  ts.start();

  auto                 client  = xpp::http::Client::builder().build();
  std::vector<uint8_t> payload = {0x00, 0x01, 0x02, 0xFE, 0xFF};
  auto                 req     = Request::builder()
               .method(Method::Post)
               .url(ts.url("/echo-bin"))
               .header("Content-Length", std::to_string(payload.size()))
               .body(xpp::bytes::Reader(xpp::bytes::Bytes::from(std::vector<uint8_t>(payload))))
               .unwrap();
  auto result = client.send(std::move(req)).await();

  ASSERT_TRUE(result.is_ok());
  EXPECT_EQ(result.unwrap().status(), 200);

  auto body = result.unwrap().text().await();
  ASSERT_TRUE(body.is_ok());
  EXPECT_EQ(body.unwrap(),
            std::string(reinterpret_cast<const char *>(payload.data()), payload.size()));
}
