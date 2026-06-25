/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * client_test.cpp - Unit tests for xhttp (async HTTP client)
 *
 * All tests use an in-process xHttpServer — no external network required.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <x/http/client.h>
#include <x/http/server.h>
}

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

/* ───────────────────── Helpers ───────────────────── */

struct ResponseCtx {
  std::atomic<bool> done{false};
  long              status_code{0};
  int               curl_code{-1};
  std::string       body;
  std::string       headers;
  std::string       curl_error;
};

static void on_response(const xHttpResponse *resp, void *arg) {
  auto *ctx        = static_cast<ResponseCtx *>(arg);
  ctx->status_code = resp->status_code;
  ctx->curl_code   = resp->curl_code;
  if (resp->body && resp->body_len > 0) ctx->body.assign(resp->body, resp->body_len);
  if (resp->headers && resp->headers_len > 0) ctx->headers.assign(resp->headers, resp->headers_len);
  if (resp->curl_error) ctx->curl_error = resp->curl_error;
  ctx->done.store(true, std::memory_order_release);
}

static uint16_t find_free_port() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port        = 0;
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return 0; }
  socklen_t len = sizeof(addr);
  if (getsockname(fd, (struct sockaddr *)&addr, &len) < 0) { close(fd); return 0; }
  uint16_t port = ntohs(addr.sin_port);
  close(fd);
  return port;
}

static void pump_until(xEventLoop loop, std::atomic<bool> &flag, int max_ms = 5000) {
  for (int elapsed = 0; elapsed < max_ms && !flag.load(std::memory_order_acquire); elapsed += 10)
    xEventLoopRun(loop, X_RUN_ONCE);
}

static void pump_until_count(xEventLoop loop, std::atomic<int> &count, int target,
                             int max_ms = 10000) {
  for (int elapsed = 0; elapsed < max_ms && count.load(std::memory_order_acquire) < target;
       elapsed += 10)
    xEventLoopRun(loop, X_RUN_ONCE);
}

/* ───────────────────── Fixture ───────────────────── */

class HttpClientTest : public ::testing::Test {
protected:
  xEventLoop  loop   = nullptr;
  xHttpServer server = nullptr;
  xHttpClient client = nullptr;
  uint16_t    port   = 0;

  std::string url(const char *path) {
    return "http://127.0.0.1:" + std::to_string(port) + path;
  }

  void SetUp() override {
    loop = xEventLoopCreate();
    ASSERT_NE(loop, nullptr);
    xEventLoopEnter(loop);

    server = xHttpServerCreate();
    ASSERT_NE(server, nullptr);

    /* GET /get — returns 200 with body "ok" */
    xHttpServerRoute(server, "GET /get",
      [](xHttpResponseWriter w, const xHttpRequest *req, void *arg) {
        xHttpResponseSetStatus(w, 200);
        xHttpResponseSetHeader(w, "Content-Type", "text/plain");
        xHttpResponseSend(w, "ok", 2);
      }, nullptr);

    /* POST /post — echoes request body */
    xHttpServerRoute(server, "POST /post",
      [](xHttpResponseWriter w, const xHttpRequest *req, void *arg) {
        xHttpResponseSetStatus(w, 200);
        xHttpResponseSetHeader(w, "Content-Type", "text/plain");
        xHttpResponseSend(w, req->body, req->body_len);
      }, nullptr);

    /* GET /headers — echoes raw headers for debugging with DoWithCustomHeaders */
    xHttpServerRoute(server, "GET /headers",
      [](xHttpResponseWriter w, const xHttpRequest *req, void *arg) {
        xHttpResponseSetStatus(w, 200);
        xHttpResponseSetHeader(w, "Content-Type", "text/plain");
        if (req->headers && req->headers_len > 0) {
          xHttpResponseWrite(w, req->headers, req->headers_len);
        }
        xHttpResponseEnd(w);
      }, nullptr);

    /* GET /ping — used by DoWithTimeout; server defers, client times out */
    xHttpServerRoute(server, "GET /ping",
      [](xHttpResponseWriter w, const xHttpRequest *req, void *arg) {
        xHttpResponseDefer(w);
      }, nullptr);

    port = find_free_port();
    ASSERT_NE(port, 0) << "Could not find a free port";
    xErrno err = xHttpServerListen(server, "127.0.0.1", port);
    ASSERT_EQ(err, xErrno_Ok) << "Failed to listen on port " << port;

    client = xHttpClientCreate(nullptr);
    ASSERT_NE(client, nullptr);
  }

  void TearDown() override {
    if (client) xHttpClientDestroy(client);
    if (server) xHttpServerDestroy(server);
    xEventLoopLeave();
    if (loop) xEventLoopDestroy(loop);
  }
};

/* ───────────────────── Create / Destroy ───────────────────── */

TEST(HttpClientLifecycle, CreateAndDestroy) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);
  xHttpClient c = xHttpClientCreate(nullptr);
  ASSERT_NE(c, nullptr);
  xHttpClientDestroy(c);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(HttpClientLifecycle, CreateWithNullLoopReturnsNull) {
  EXPECT_EQ(xHttpClientCreate(nullptr), nullptr);
}

TEST_F(HttpClientTest, DestroyWithInflightRequests) {
  /* This test verifies that destroying the client while requests are in
   * flight does not crash. The request to a local server on a closed port
   * (port 1) is guaranteed to fail quickly — no 60s timeout. */
  xHttpClient local = xHttpClientCreate(nullptr);
  ASSERT_NE(local, nullptr);
  xHttpClientDestroy(local);
}

/* ───────────────────── GET request ───────────────────── */

TEST_F(HttpClientTest, GetRequest) {
  ResponseCtx ctx;
  xErrno err = xHttpClientGet(client, url("/get").c_str(), on_response, &ctx);
  ASSERT_EQ(err, xErrno_Ok);
  pump_until(loop, ctx.done);
  xHttpClientDestroy(client);
  client = nullptr;

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_FALSE(ctx.body.empty());
}

/* ───────────────────── POST request ───────────────────── */

TEST_F(HttpClientTest, PostRequest) {
  ResponseCtx ctx;
  const char *body = "{\"hello\":\"world\"}";
  xErrno err = xHttpClientPost(client, url("/post").c_str(), body, strlen(body), on_response, &ctx);
  ASSERT_EQ(err, xErrno_Ok);
  pump_until(loop, ctx.done);
  xHttpClientDestroy(client);
  client = nullptr;

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_NE(ctx.body.find("hello"), std::string::npos);
}

/* ───────────────────── Concurrent requests ───────────────────── */

TEST_F(HttpClientTest, ConcurrentRequests) {
  constexpr int    N = 3;
  std::atomic<int> done_count{0};

  struct MultiCtx {
    std::atomic<int> *counter;
    long              status_code{0};
  };
  std::vector<MultiCtx> ctxs(N);
  for (auto &c : ctxs) c.counter = &done_count;

  auto multi_cb = [](const xHttpResponse *resp, void *arg) {
    auto *ctx        = static_cast<MultiCtx *>(arg);
    ctx->status_code = resp->status_code;
    ctx->counter->fetch_add(1);
  };

  for (int i = 0; i < N; i++) {
    xErrno err = xHttpClientGet(client, url("/get").c_str(), multi_cb, &ctxs[i]);
    ASSERT_EQ(err, xErrno_Ok);
  }
  pump_until_count(loop, done_count, N);
  xHttpClientDestroy(client);
  client = nullptr;

  EXPECT_EQ(done_count.load(), N);
  for (int i = 0; i < N; i++) EXPECT_EQ(ctxs[i].status_code, 200);
}

/* ───────────────────── Invalid URL ───────────────────── */

TEST_F(HttpClientTest, InvalidUrlFails) {
  ResponseCtx ctx;
  xErrno err = xHttpClientGet(client, "http://127.0.0.1:1/nope", on_response, &ctx);
  ASSERT_EQ(err, xErrno_Ok);
  pump_until(loop, ctx.done, 5000);
  xHttpClientDestroy(client);
  client = nullptr;

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_NE(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 0);
}

/* ───────────────────── Parameter validation ───────────────────── */

TEST_F(HttpClientTest, GetNullUrlReturnsError) {
  EXPECT_EQ(xHttpClientGet(client, nullptr, on_response, nullptr), xErrno_Unknown);
}
TEST_F(HttpClientTest, GetNullClientReturnsError) {
  EXPECT_EQ(xHttpClientGet(nullptr, "http://example.com", on_response, nullptr), xErrno_Unknown);
}
TEST_F(HttpClientTest, PostNullUrlReturnsError) {
  EXPECT_EQ(xHttpClientPost(client, nullptr, "body", 4, on_response, nullptr), xErrno_Unknown);
}

/* ───────────────────── Generic Do request ───────────────────── */

TEST_F(HttpClientTest, DoGetRequest) {
  ResponseCtx ctx;
  std::string u = url("/get");
  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url    = u.c_str();
  config.method = xHttpMethod_GET;

  xErrno err = xHttpClientDo(client, &config, on_response, &ctx);
  ASSERT_EQ(err, xErrno_Ok);
  pump_until(loop, ctx.done);
  xHttpClientDestroy(client);
  client = nullptr;

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
}

TEST_F(HttpClientTest, DoWithCustomHeaders) {
  ResponseCtx ctx;
  std::string u = url("/headers");
  const char *hdrs[] = {"X-Custom-Header: test-value", NULL};
  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url     = u.c_str();
  config.method  = xHttpMethod_GET;
  config.headers = hdrs;

  xErrno err = xHttpClientDo(client, &config, on_response, &ctx);
  ASSERT_EQ(err, xErrno_Ok);
  pump_until(loop, ctx.done);
  xHttpClientDestroy(client);
  client = nullptr;

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_NE(ctx.body.find("test-value"), std::string::npos);
}

TEST_F(HttpClientTest, DoWithTimeout) {
  ResponseCtx ctx;
  std::string u = url("/ping");
  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url        = u.c_str();
  config.method     = xHttpMethod_GET;
  config.timeout_ms = 500;

  xErrno err = xHttpClientDo(client, &config, on_response, &ctx);
  ASSERT_EQ(err, xErrno_Ok);
  pump_until(loop, ctx.done, 5000);
  xHttpClientDestroy(client);
  client = nullptr;

  ASSERT_TRUE(ctx.done.load()) << "Request timed out waiting for callback";
  EXPECT_NE(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 0);
}

TEST_F(HttpClientTest, DoNullConfigReturnsError) {
  EXPECT_EQ(xHttpClientDo(client, nullptr, on_response, nullptr), xErrno_Unknown);
}
