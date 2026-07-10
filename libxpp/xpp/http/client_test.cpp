/*
 * client_test.cpp — Tests for xpp::http::Client.
 *
 * Uses TestServer (Go httptest-style) for server setup, plus a
 * route_raw helper for POST echo tests that need on_data access.
 */

#include <string>

#include <gtest/gtest.h>
#include <xpp/bytes/bytes.h>
#include <xpp/bytes/reader.h>
#include <xpp/event.h>
#include <xpp/http/client.h>
#include <xpp/http/test_server.h>
#include <xpp/promise.h>

#include <x/http/server.h>

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

/* ── EchoCtx — used by route_with_data for POST body echo ────────── */

struct EchoCtx {
  std::string body;
  size_t      offset = 0;
};

/* ── Fixture ──────────────────────────────────────────────────────── */

class HttpClientTest : public ::testing::Test {
protected:
  xpp::EventLoop m_loop;
  xpp::WaitScope m_scope{m_loop};

  /// Register a route that needs on_data (POST body collection).
  /// The handler receives a shared_ptr<string> that will hold the
  /// accumulated request body — usable from on_read after on_data
  /// has finished.
  void route_with_data(const char *pattern, EchoCtx *echo) {
    xHttpRouteConf conf = {};
    conf.pattern        = pattern;
    conf.on_request     = echo_on_request;
    conf.on_read        = echo_on_read;
    conf.on_data        = echo_on_data;
    conf.arg            = echo;
    ASSERT_EQ(xHttpMuxHandle(m_mux, &conf), xErrno_Ok);
  }

  /// Access the raw mux for route_with_data.  TestServer's router
  /// owns it — we grab a copy of the handle for raw route registration.
  void bind_mux(xHttpMux mux) { m_mux = mux; }

private:
  static int echo_on_request(xHttpCtx *ctx, void *arg) {
    auto *c = static_cast<EchoCtx *>(arg);
    xHttpCtxSetStatus(ctx, 200);
    xHttpCtxSetHeader(ctx, "Content-Type", "application/octet-stream");
    return 0;
  }
  static size_t echo_on_read(char *buf, size_t bufsize, void *arg) {
    auto *c = static_cast<EchoCtx *>(arg);
    if (c->offset >= c->body.size()) return 0;
    size_t n = std::min(bufsize, c->body.size() - c->offset);
    std::memcpy(buf, c->body.data() + c->offset, n);
    c->offset += n;
    return n;
  }
  static int echo_on_data(const char *data, size_t len, void *arg) {
    static_cast<EchoCtx *>(arg)->body.append(data, len);
    return 0;
  }

  xHttpMux m_mux = nullptr; // borrow from TestServer's router
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

TEST_F(HttpClientTest, PostEcho) {
  xpp::http::TestServer ts;
  EchoCtx               echo;
  // Register the raw route before start() so on_data is wired.
  // We grab the mux from the router, register, then start.
  bind_mux(ts.router().raw());
  route_with_data("POST /echo", &echo);
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

TEST_F(HttpClientTest, PostBinaryBody) {
  xpp::http::TestServer ts;
  EchoCtx               echo;
  bind_mux(ts.router().raw());
  route_with_data("POST /echo-bin", &echo);
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
