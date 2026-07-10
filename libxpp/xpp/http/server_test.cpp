/*
 * server_test.cpp — Tests for xpp::http server module.
 *
 * Uses TestServer (Go httptest-style): ts.get() returns
 * Promise<Result<Response>> — each test .await()s and uses
 * Response API (status(), text(), header()) as needed.
 */

#include <string>
#include <utility>

#include <gtest/gtest.h>
#include <xpp/http/test_server.h>

/* ── StringReader — TryRead adapter for string-based test bodies ─── */

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

/* ── Helper — drain a GET response into {status, body} ──────────── */

static std::pair<int, std::string> drain(xpp::http::TestServer &ts, const char *path) {
  auto result = ts.get(path).await();
  EXPECT_TRUE(result.is_ok()) << "GET " << path << " failed";
  auto &resp = result.unwrap();
  auto  body = resp.text().await();
  EXPECT_TRUE(body.is_ok());
  return {resp.status(), body.unwrap()};
}

/* ═══════════════════════════════════════════════════════════════════
 *  Unit tests
 * ═══════════════════════════════════════════════════════════════════ */

TEST(ServerTest, CreateRouter) {
  xpp::http::Router router;
  SUCCEED();
}

TEST(ServerTest, BindInvalidAddr) {
  auto result = xpp::http::Server::bind("not-an-address");
  EXPECT_TRUE(result.is_err());
}

/* ═══════════════════════════════════════════════════════════════════
 *  Integration tests
 * ═══════════════════════════════════════════════════════════════════ */

class ServerIntegrationTest : public ::testing::Test {
protected:
  xpp::EventLoop m_loop;
  xpp::WaitScope m_scope{m_loop};
};

TEST_F(ServerIntegrationTest, HelloWorld) {
  xpp::http::TestServer ts;
  ts.router().route("GET /hello", [](xpp::http::IncomingRequest &req) {
    EXPECT_EQ(req.method(), "GET");
    EXPECT_EQ(req.path(), "/hello");
    return xpp::http::Response::builder()
      .status(200)
      .body(StringReader{"Hello from xpp!"})
      .into_promise();
  });
  ts.start();

  auto [status, body] = drain(ts, "/hello");
  EXPECT_EQ(status, 200);
  EXPECT_EQ(body, "Hello from xpp!");
}

TEST_F(ServerIntegrationTest, StatusAndHeaders) {
  xpp::http::TestServer ts;
  ts.router().route("GET /json", [](xpp::http::IncomingRequest &) {
    return xpp::http::Response::builder()
      .status(201)
      .header("Content-Type", "application/json")
      .header("X-Custom", "v1")
      .body(StringReader{"{}"})
      .into_promise();
  });
  ts.start();

  // Use raw Response to verify headers
  auto result = ts.get("/json").await();
  ASSERT_TRUE(result.is_ok());
  auto &resp = result.unwrap();
  EXPECT_EQ(resp.status(), 201);
  EXPECT_EQ(resp.header("content-type").unwrap(), "application/json");
  EXPECT_EQ(resp.header("x-custom").unwrap(), "v1");

  auto body = resp.text().await();
  ASSERT_TRUE(body.is_ok());
  EXPECT_EQ(body.unwrap(), "{}");
}

TEST_F(ServerIntegrationTest, RouteParams) {
  xpp::http::TestServer ts;
  ts.router().route("GET /users/:id", [](xpp::http::IncomingRequest &req) {
    std::string id  = req.param("id");
    std::string msg = "user:" + id;
    return xpp::http::Response::builder()
      .status(200)
      .body(StringReader{std::move(msg)})
      .into_promise();
  });
  ts.start();

  auto [status, body] = drain(ts, "/users/42");
  EXPECT_EQ(status, 200);
  EXPECT_EQ(body, "user:42");
}

// One-byte-at-a-time reader for the streaming test.
struct OneByteReader {
  const char    *data;
  size_t         len;
  mutable size_t off = 0;

  ssize_t try_read(char *buf, size_t cap) {
    if (off >= len) return 0;
    size_t n = std::min(cap, static_cast<size_t>(1));
    buf[0]   = data[off];
    off++;
    return static_cast<ssize_t>(n);
  }
};

TEST_F(ServerIntegrationTest, TryReadBody) {
  xpp::http::TestServer ts;
  ts.router().route("GET /try-read", [](xpp::http::IncomingRequest &) {
    OneByteReader reader{"hello-try-read", 14};
    return xpp::http::Response::builder().status(200).body(std::move(reader)).into_promise();
  });
  ts.start();

  auto [status, body] = drain(ts, "/try-read");
  EXPECT_EQ(status, 200);
  EXPECT_EQ(body, "hello-try-read");
}

struct EmptyReader {
  ssize_t try_read(char *, size_t) { return 0; }
};

TEST_F(ServerIntegrationTest, TryReadBodyEmpty) {
  xpp::http::TestServer ts;
  ts.router().route("GET /try-read-empty", [](xpp::http::IncomingRequest &) {
    return xpp::http::Response::builder().status(200).body(EmptyReader{}).into_promise();
  });
  ts.start();

  auto [status, body] = drain(ts, "/try-read-empty");
  EXPECT_EQ(status, 200);
  EXPECT_EQ(body, "");
}

TEST_F(ServerIntegrationTest, EmptyBody) {
  xpp::http::TestServer ts;
  ts.router().route("GET /empty", [](xpp::http::IncomingRequest &) {
    return xpp::http::Response::builder().status(200).into_promise();
  });
  ts.start();

  auto [status, body] = drain(ts, "/empty");
  EXPECT_EQ(status, 200);
  EXPECT_EQ(body, "");
}

TEST_F(ServerIntegrationTest, LargeBody) {
  xpp::http::TestServer ts;
  std::string           large(10240, 'x');
  ts.router().route("GET /large", [large](xpp::http::IncomingRequest &) {
    return xpp::http::Response::builder()
      .status(200)
      .body(StringReader{std::move(large)})
      .into_promise();
  });
  ts.start();

  auto [status, body] = drain(ts, "/large");
  EXPECT_EQ(status, 200);
  EXPECT_EQ(body, large);
}

TEST_F(ServerIntegrationTest, MultipleRequests) {
  xpp::http::TestServer ts;
  int                   counter = 0;
  ts.router().route("GET /count", [&counter](xpp::http::IncomingRequest &) {
    counter++;
    return xpp::http::Response::builder()
      .status(200)
      .body(StringReader{std::to_string(counter)})
      .into_promise();
  });
  ts.start();

  for (int i = 1; i <= 5; i++) {
    auto [status, body] = drain(ts, "/count");
    EXPECT_EQ(status, 200);
    EXPECT_EQ(body, std::to_string(i));
  }
  EXPECT_EQ(counter, 5);
}

TEST_F(ServerIntegrationTest, CustomStatusCodes) {
  xpp::http::TestServer ts;
  ts.router().route("GET /404", [](xpp::http::IncomingRequest &) {
    return xpp::http::Response::builder()
      .status(404)
      .body(StringReader{"not found"})
      .into_promise();
  });
  ts.router().route("GET /500", [](xpp::http::IncomingRequest &) {
    return xpp::http::Response::builder().status(500).body(StringReader{"boom"}).into_promise();
  });
  ts.start();

  auto [s1, b1] = drain(ts, "/404");
  EXPECT_EQ(s1, 404);

  auto [s2, b2] = drain(ts, "/500");
  EXPECT_EQ(s2, 500);
}

TEST_F(ServerIntegrationTest, SequentialHandlers) {
  xpp::http::TestServer ts;
  ts.router().route("GET /a", [](xpp::http::IncomingRequest &) {
    return xpp::http::Response::builder().status(200).body(StringReader{"a"}).into_promise();
  });
  ts.router().route("GET /b", [](xpp::http::IncomingRequest &) {
    return xpp::http::Response::builder().status(200).body(StringReader{"b"}).into_promise();
  });
  ts.start();

  auto [_, a1] = drain(ts, "/a"); EXPECT_EQ(a1, "a");
  auto [__, b] = drain(ts, "/b"); EXPECT_EQ(b, "b");
  auto [___, a2] = drain(ts, "/a"); EXPECT_EQ(a2, "a");
}
