/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * client_test.cpp - Unit tests for xhttp (async HTTP client)
 *
 * All tests use an in-process xHttpServer — no external network required.
 */

#include "server_test_helper.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <x/http/client.h>
#include <x/http/server.h>

/* ───────────────────── Helpers ───────────────────── */

struct ResponseCtx {
  std::atomic<bool> done{false};
  long              status_code{0};
  int               curl_code{-1};
  std::string       body;
  std::string       headers;
  std::string       curl_error;
  /* Upload state — used when on_read is set */
  std::string upload_data;
  size_t      upload_offset{0};
};

static void on_response(xHttpCtx *ctx, void *arg) {
  auto *c        = static_cast<ResponseCtx *>(arg);
  c->status_code = ctx->status_code;
  c->curl_code   = ctx->curl_code;
  if (ctx->headers && ctx->headers_len > 0) c->headers.assign(ctx->headers, ctx->headers_len);
  if (ctx->curl_error) c->curl_error = ctx->curl_error;
  c->done.store(true, std::memory_order_release);
}

static int on_data_collect(const char *data, size_t len, void *arg) {
  auto *c = static_cast<ResponseCtx *>(arg);
  c->body.append(data, len);
  return 0;
}

static size_t on_read_provide(char *buf, size_t bufsize, void *arg) {
  auto  *c         = static_cast<ResponseCtx *>(arg);
  size_t remaining = c->upload_data.size() - c->upload_offset;
  if (remaining == 0) return 0; /* EOF */
  size_t n = bufsize < remaining ? bufsize : remaining;
  memcpy(buf, c->upload_data.data() + c->upload_offset, n);
  c->upload_offset += n;
  return n;
}

/* find_free_port / pump_until / pump_until_count removed — use the
 * shared helpers from server_test_helper.h instead. */

/* ───────────────────── Echo helpers (server-side) ─────────────────────
 * Collects a request body via on_data, then echoes it back from on_done.
 * Used by HttpClientTest's POST /post route. */

struct EchoBody {
  std::string body;
};

static int echo_collect_data(const char *data, size_t len, void *arg) {
  auto *e = static_cast<EchoBody *>(arg);
  e->body.append(data, len);
  return 0;
}

static void echo_send_handler(xHttpCtx *ctx, void *arg) {
  auto *e = static_cast<EchoBody *>(arg);
  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSetHeader(ctx, "Content-Type", "text/plain");
  xHttpCtxSend(ctx, e->body.data(), e->body.size());
}

/* ──────────���────────── Fixture ───────────────────── */

class HttpClientTest : public ::testing::Test {
protected:
  xEventLoop  loop   = nullptr;
  xHttpServer server = nullptr;
  xHttpMux    mux    = nullptr;
  xHttpClient client = nullptr;
  uint16_t    port   = 0;

  /* Echo state for POST /post — collects request body via on_data, then
   * echoes it back from on_done. Fresh per fixture instance. */
  EchoBody echo_body;

  std::string url(const char *path) {
    return "http://127.0.0.1:" + std::to_string(port) + path;
  }

  void SetUp() override {
    loop = xEventLoopCreate();
    ASSERT_NE(loop, nullptr);
    xEventLoopEnter(loop);

    mux = xHttpMuxCreate();
    ASSERT_NE(mux, nullptr);

    xHttpServerConf sconf = {};
    sconf.resolve         = xHttpMuxResolve;
    sconf.router          = mux;
    sconf.idle_timeout_ms = 60000;
    server                = xHttpServerCreate(&sconf);
    ASSERT_NE(server, nullptr);

    /* GET /get — returns 200 with body "ok" */
    {
      xHttpRouteConf rc = {};
      rc.pattern        = "GET /get";
      rc.on_done        = [](xHttpCtx *ctx, void *arg) {
        xHttpCtxSetStatus(ctx, 200);
        xHttpCtxSetHeader(ctx, "Content-Type", "text/plain");
        xHttpCtxSend(ctx, "ok", 2);
      };
      ASSERT_EQ(xHttpMuxHandle(mux, &rc), xErrno_Ok);
    }

    /* POST /post — echoes request body (collected via on_data) */
    {
      xHttpRouteConf rc = {};
      rc.pattern        = "POST /post";
      rc.on_data        = echo_collect_data;
      rc.on_done        = echo_send_handler;
      rc.arg            = &echo_body;
      ASSERT_EQ(xHttpMuxHandle(mux, &rc), xErrno_Ok);
    }

    /* GET /headers — echoes raw request headers for DoWithCustomHeaders */
    {
      xHttpRouteConf rc = {};
      rc.pattern        = "GET /headers";
      rc.on_done        = [](xHttpCtx *ctx, void *arg) {
        xHttpCtxSetStatus(ctx, 200);
        xHttpCtxSetHeader(ctx, "Content-Type", "text/plain");
        if (ctx->headers && ctx->headers_len > 0) {
          xHttpCtxWrite(ctx, ctx->headers, ctx->headers_len);
        }
        xHttpCtxEndStream(ctx);
      };
      ASSERT_EQ(xHttpMuxHandle(mux, &rc), xErrno_Ok);
    }

    /* GET /ping — used by DoWithTimeout; server doesn't respond, client times out */
    {
      xHttpRouteConf rc = {};
      rc.pattern        = "GET /ping";
      rc.on_done        = [](xHttpCtx *ctx, void *arg) {
        /* Don't send a response — client should time out */
      };
      ASSERT_EQ(xHttpMuxHandle(mux, &rc), xErrno_Ok);
    }

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
    if (mux) xHttpMuxDestroy(mux);
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
  ResponseCtx      ctx;
  std::string      u    = url("/get");
  xHttpRequestConf conf = {};
  conf.url              = u.c_str();
  conf.on_data          = on_data_collect;
  conf.on_done          = on_response;
  xErrno err            = xHttpClientGet(client, &conf, &ctx);
  ASSERT_EQ(err, xErrno_Ok);
  run_until(loop, ctx.done);
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
  ctx.upload_data.assign(body, strlen(body));
  std::string      u    = url("/post");
  xHttpRequestConf conf = {};
  conf.url              = u.c_str();
  conf.on_read          = on_read_provide;
  conf.content_length   = strlen(body);
  conf.on_data          = on_data_collect;
  conf.on_done          = on_response;
  xErrno err            = xHttpClientPost(client, &conf, &ctx);
  ASSERT_EQ(err, xErrno_Ok);
  run_until(loop, ctx.done);
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
  for (auto &c : ctxs)
    c.counter = &done_count;

  auto multi_cb = [](xHttpCtx *c, void *arg) {
    auto *ctx        = static_cast<MultiCtx *>(arg);
    ctx->status_code = c->status_code;
    ctx->counter->fetch_add(1);
  };

  std::string u = url("/get");
  for (int i = 0; i < N; i++) {
    xHttpRequestConf conf = {};
    conf.url              = u.c_str();
    conf.on_done          = multi_cb;
    xErrno err            = xHttpClientGet(client, &conf, &ctxs[i]);
    ASSERT_EQ(err, xErrno_Ok);
  }
  run_until_count(loop, done_count, N);
  xHttpClientDestroy(client);
  client = nullptr;

  EXPECT_EQ(done_count.load(), N);
  for (int i = 0; i < N; i++)
    EXPECT_EQ(ctxs[i].status_code, 200);
}

/* ───────────────────── Invalid URL ───────────────────── */

TEST_F(HttpClientTest, InvalidUrlFails) {
  ResponseCtx      ctx;
  xHttpRequestConf conf = {};
  conf.url              = "http://127.0.0.1:1/nope";
  conf.on_data          = on_data_collect;
  conf.on_done          = on_response;
  xErrno err            = xHttpClientGet(client, &conf, &ctx);
  ASSERT_EQ(err, xErrno_Ok);
  run_until(loop, ctx.done, 5000);
  xHttpClientDestroy(client);
  client = nullptr;

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_NE(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 0);
}

/* ───────────────────── Parameter validation ───────────────────── */

TEST_F(HttpClientTest, GetNullUrlReturnsError) {
  xHttpRequestConf conf = {};
  /* conf.url is NULL */
  EXPECT_EQ(xHttpClientGet(client, &conf, nullptr), xErrno_Unknown);
}
TEST_F(HttpClientTest, GetNullClientReturnsError) {
  xHttpRequestConf conf = {};
  conf.url              = "http://example.com";
  EXPECT_EQ(xHttpClientGet(nullptr, &conf, nullptr), xErrno_Unknown);
}
TEST_F(HttpClientTest, PostNullUrlReturnsError) {
  xHttpRequestConf conf = {};
  /* conf.url is NULL */
  EXPECT_EQ(xHttpClientPost(client, &conf, nullptr), xErrno_Unknown);
}

/* ───────────────────── Generic Do request ───────────────────── */

TEST_F(HttpClientTest, DoGetRequest) {
  ResponseCtx      ctx;
  std::string      u = url("/get");
  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url     = u.c_str();
  config.method  = xHttpMethod_GET;
  config.on_data = on_data_collect;
  config.on_done = on_response;

  xErrno err = xHttpClientDo(client, &config, &ctx);
  ASSERT_EQ(err, xErrno_Ok);
  run_until(loop, ctx.done);
  xHttpClientDestroy(client);
  client = nullptr;

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
}

TEST_F(HttpClientTest, DoWithCustomHeaders) {
  ResponseCtx      ctx;
  std::string      u      = url("/headers");
  const char      *hdrs[] = {"X-Custom-Header: test-value", NULL};
  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url     = u.c_str();
  config.method  = xHttpMethod_GET;
  config.headers = hdrs;
  config.on_data = on_data_collect;
  config.on_done = on_response;

  xErrno err = xHttpClientDo(client, &config, &ctx);
  ASSERT_EQ(err, xErrno_Ok);
  run_until(loop, ctx.done);
  xHttpClientDestroy(client);
  client = nullptr;

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_NE(ctx.body.find("test-value"), std::string::npos);
}

TEST_F(HttpClientTest, DoWithTimeout) {
  ResponseCtx      ctx;
  std::string      u = url("/ping");
  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url        = u.c_str();
  config.method     = xHttpMethod_GET;
  config.timeout_ms = 500;
  config.on_data    = on_data_collect;
  config.on_done    = on_response;

  xErrno err = xHttpClientDo(client, &config, &ctx);
  ASSERT_EQ(err, xErrno_Ok);
  run_until(loop, ctx.done, 5000);
  xHttpClientDestroy(client);
  client = nullptr;

  ASSERT_TRUE(ctx.done.load()) << "Request timed out waiting for callback";
  EXPECT_NE(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 0);
}

TEST_F(HttpClientTest, DoNullConfigReturnsError) {
  EXPECT_EQ(xHttpClientDo(client, nullptr, nullptr), xErrno_Unknown);
}

/* ───────────────────── Streaming client API tests ───────────────────── */
/*
 * Tests for the on_response / on_data / on_read / on_done streaming callbacks
 * added to xHttpRequestConf. Uses the HttpServerTest fixture so each test
 * gets its own server + port; the xHttpClient is created per-test.
 */

namespace {

struct StreamCtx {
  std::atomic<bool> done{false};
  long              status_code{0};
  int               curl_code{-1};
  std::string       body_acc; // accumulator for on_data
  std::string       headers;  // full headers from on_response
  size_t            total_bytes{0};
  int               abort_after{0}; // >0 → abort after N data calls
  std::atomic<int>  data_calls{0};
  bool              first_data_seen{false};
  bool              had_headers_before_data{false};
  bool              on_response_called{false};

  /* Upload state — only used when on_read is set. Embedded so a single
   * arg pointer serves on_read, on_data, and on_done. */
  std::string upload_data;
  size_t      upload_offset{0};
  size_t      upload_chunk_size{1024};
};

static int collect_init(xHttpCtx *ctx, void *arg) {
  auto *c               = static_cast<StreamCtx *>(arg);
  c->on_response_called = true;
  if (ctx->headers && ctx->headers_len > 0) {
    c->headers.assign(ctx->headers, ctx->headers_len);
  }
  return 0;
}

static int collect_data(const char *data, size_t len, void *arg) {
  auto *ctx = static_cast<StreamCtx *>(arg);
  if (!ctx->first_data_seen) {
    ctx->first_data_seen         = true;
    ctx->had_headers_before_data = ctx->on_response_called;
  }
  ctx->body_acc.append(data, len);
  ctx->total_bytes += len;
  int n = ctx->data_calls.fetch_add(1, std::memory_order_acq_rel) + 1;
  if (ctx->abort_after > 0 && n >= ctx->abort_after) return 1; /* abort */
  return 0;
}

static void on_stream_done(xHttpCtx *ctx, void *arg) {
  auto *c        = static_cast<StreamCtx *>(arg);
  c->status_code = ctx->status_code;
  c->curl_code   = ctx->curl_code;
  c->done.store(true, std::memory_order_release);
}

/* Streaming upload provider — hands out up to upload_chunk_size bytes per
 * call, drawing from ctx->upload_data. Returns 0 at EOF. */
static size_t provide_data(char *buf, size_t bufsize, void *arg) {
  auto  *ctx       = static_cast<StreamCtx *>(arg);
  size_t remaining = ctx->upload_data.size() - ctx->upload_offset;
  if (remaining == 0) return 0; /* EOF */
  size_t n = bufsize < remaining ? bufsize : remaining;
  if (n > ctx->upload_chunk_size) n = ctx->upload_chunk_size;
  memcpy(buf, ctx->upload_data.data() + ctx->upload_offset, n);
  ctx->upload_offset += n;
  return n;
}

/* Server handler: send a pre-built body verbatim. */
static void send_body_handler(xHttpCtx *ctx, void *arg) {
  auto *body = static_cast<std::string *>(arg);
  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSetHeader(ctx, "Content-Type", "text/plain");
  xHttpCtxSend(ctx, body->c_str(), body->size());
}

/* Server handler: echo the request body back. Records the method so tests
 * can verify which HTTP method curl actually sent.
 *
 * The new API delivers the body via on_data (no req->body), so we collect
 * it into EchoState::body and echo it back from on_done. */
struct EchoCtx {
  std::string last_method;
  std::string last_body;
};

struct EchoState {
  EchoCtx    *ctx{nullptr};
  std::string body;
};

static int echo_on_data(const char *data, size_t len, void *arg) {
  auto *s = static_cast<EchoState *>(arg);
  s->body.append(data, len);
  return 0;
}

static void echo_handler(xHttpCtx *ctx, void *arg) {
  auto *s = static_cast<EchoState *>(arg);
  if (s->ctx) {
    s->ctx->last_method = ctx->method ? ctx->method : "";
    s->ctx->last_body   = s->body;
  }
  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSetHeader(ctx, "Content-Type", "application/octet-stream");
  xHttpCtxSend(ctx, s->body.data(), s->body.size());
}

/* Server handler: send a small body with custom headers. */
static void custom_headers_handler(xHttpCtx *ctx, void *arg) {
  (void)arg;
  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSetHeader(ctx, "Content-Type", "text/plain");
  xHttpCtxSetHeader(ctx, "X-Custom-A", "alpha");
  xHttpCtxSetHeader(ctx, "X-Custom-B", "beta");
  xHttpCtxSend(ctx, "body", 4);
}

/* Server handler: minimal 200 OK with body "ok". */
static void ok_handler(xHttpCtx *ctx, void *arg) {
  (void)arg;
  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSetHeader(ctx, "Content-Type", "text/plain");
  xHttpCtxSend(ctx, "ok", 2);
}

} // namespace

/* 1. Streaming response: on_data collects a large body; on_done sees body=NULL. */
TEST_F(HttpServerTest, StreamingResponseClient) {
  std::string body(8192, 'x');
  route("GET /big", send_body_handler, &body);
  listen_and_pump();

  xHttpClient client = xHttpClientCreate(nullptr);
  ASSERT_NE(client, nullptr);

  StreamCtx        ctx;
  std::string      u    = "http://127.0.0.1:" + std::to_string(port) + "/big";
  xHttpRequestConf conf = {};
  conf.url              = u.c_str();
  conf.method           = xHttpMethod_GET;
  conf.on_data          = collect_data;
  conf.on_done          = on_stream_done;
  xErrno err            = xHttpClientDo(client, &conf, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 5000);
  xHttpClientDestroy(client);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_EQ(ctx.total_bytes, body.size());
  EXPECT_EQ(ctx.body_acc, body);
}

/* 2. Streaming abort: on_data returns 1 → curl aborts with non-zero curl_code. */
TEST_F(HttpServerTest, StreamingAbort) {
  /* 64KB exceeds curl's 16KB write buffer, guaranteeing multiple chunks so
   * the abort fires before the full body has been delivered. */
  std::string body(65536, 'x');
  route("GET /big", send_body_handler, &body);
  listen_and_pump();

  xHttpClient client = xHttpClientCreate(nullptr);
  ASSERT_NE(client, nullptr);

  StreamCtx ctx;
  ctx.abort_after       = 1; /* abort after the first data chunk */
  std::string      u    = "http://127.0.0.1:" + std::to_string(port) + "/big";
  xHttpRequestConf conf = {};
  conf.url              = u.c_str();
  conf.method           = xHttpMethod_GET;
  conf.on_data          = collect_data;
  conf.on_done          = on_stream_done;
  xErrno err            = xHttpClientDo(client, &conf, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 5000);
  xHttpClientDestroy(client);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_NE(ctx.curl_code, 0);
  EXPECT_GE(ctx.total_bytes, 1u);
  EXPECT_LT(ctx.total_bytes, body.size());
}

/* 3. Header streaming: on_response receives status line + headers before on_data. */
TEST_F(HttpServerTest, HeaderStreaming) {
  route("GET /headers", custom_headers_handler, nullptr);
  listen_and_pump();

  xHttpClient client = xHttpClientCreate(nullptr);
  ASSERT_NE(client, nullptr);

  StreamCtx        ctx;
  std::string      u    = "http://127.0.0.1:" + std::to_string(port) + "/headers";
  xHttpRequestConf conf = {};
  conf.url              = u.c_str();
  conf.method           = xHttpMethod_GET;
  conf.on_response      = collect_init;
  conf.on_data          = collect_data;
  conf.on_done          = on_stream_done;
  xErrno err            = xHttpClientDo(client, &conf, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 5000);
  xHttpClientDestroy(client);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);

  ASSERT_FALSE(ctx.headers.empty()) << "Expected headers to be delivered via on_response";
  EXPECT_NE(ctx.headers.find("HTTP/1.1 200"), std::string::npos)
    << "Status line missing; got: " << ctx.headers;

  EXPECT_NE(ctx.headers.find("X-Custom-A: alpha"), std::string::npos)
    << "X-Custom-A header missing";
  EXPECT_NE(ctx.headers.find("X-Custom-B: beta"), std::string::npos) << "X-Custom-B header missing";

  /* Headers must arrive before any body data. */
  EXPECT_TRUE(ctx.first_data_seen);
  EXPECT_TRUE(ctx.had_headers_before_data) << "on_data was invoked before on_response";

  EXPECT_EQ(ctx.body_acc, "body");
}

/* 4. Streaming upload with known Content-Length (content_length > 0).
 *
 * NOTE: When on_read is set, client.c sets CURLOPT_UPLOAD which makes
 * libcurl use PUT (overriding CURLOPT_POST). The route is registered
 * without a method prefix so it matches any method. */
TEST_F(HttpServerTest, StreamingUpload) {
  EchoCtx   echo;
  EchoState state;
  state.ctx = &echo;
  {
    xHttpRouteConf rc = {};
    rc.pattern        = "/echo";
    rc.on_data        = echo_on_data;
    rc.on_done        = echo_handler;
    rc.arg            = &state;
    ASSERT_EQ(xHttpMuxHandle(mux, &rc), xErrno_Ok);
  }
  listen_and_pump();

  xHttpClient client = xHttpClientCreate(nullptr);
  ASSERT_NE(client, nullptr);

  StreamCtx ctx;
  ctx.upload_data       = std::string(4096, 'y');
  ctx.upload_chunk_size = 1024;

  std::string      u    = "http://127.0.0.1:" + std::to_string(port) + "/echo";
  xHttpRequestConf conf = {};
  conf.url              = u.c_str();
  conf.method           = xHttpMethod_POST;
  conf.on_read          = provide_data;
  conf.content_length   = ctx.upload_data.size(); /* known size → Content-Length */
  conf.on_data          = collect_data;
  conf.on_done          = on_stream_done;
  xErrno err            = xHttpClientDo(client, &conf, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 5000);
  xHttpClientDestroy(client);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  /* Server should have received the full uploaded body. */
  EXPECT_EQ(echo.last_body.size(), ctx.upload_data.size());
  EXPECT_EQ(echo.last_body, ctx.upload_data);
  /* And echoed it back to us via on_data. */
  EXPECT_EQ(ctx.total_bytes, ctx.upload_data.size());
  EXPECT_EQ(ctx.body_acc, ctx.upload_data);
}

/* 5. Streaming upload with chunked transfer-encoding (content_length = 0). */
TEST_F(HttpServerTest, StreamingUploadChunked) {
  EchoCtx   echo;
  EchoState state;
  state.ctx = &echo;
  {
    xHttpRouteConf rc = {};
    rc.pattern        = "/echo";
    rc.on_data        = echo_on_data;
    rc.on_done        = echo_handler;
    rc.arg            = &state;
    ASSERT_EQ(xHttpMuxHandle(mux, &rc), xErrno_Ok);
  }
  listen_and_pump();

  xHttpClient client = xHttpClientCreate(nullptr);
  ASSERT_NE(client, nullptr);

  StreamCtx ctx;
  ctx.upload_data       = std::string(4096, 'z');
  ctx.upload_chunk_size = 1024;

  std::string      u    = "http://127.0.0.1:" + std::to_string(port) + "/echo";
  xHttpRequestConf conf = {};
  conf.url              = u.c_str();
  conf.method           = xHttpMethod_POST;
  conf.on_read          = provide_data;
  conf.content_length   = 0; /* unknown size → chunked transfer */
  conf.on_data          = collect_data;
  conf.on_done          = on_stream_done;
  xErrno err            = xHttpClientDo(client, &conf, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 5000);
  xHttpClientDestroy(client);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_EQ(echo.last_body.size(), ctx.upload_data.size());
  EXPECT_EQ(echo.last_body, ctx.upload_data);
  EXPECT_EQ(ctx.total_bytes, ctx.upload_data.size());
  EXPECT_EQ(ctx.body_acc, ctx.upload_data);
}

/* 7. Large file upload (10 MB) — verifies multi-chunk streaming upload
 *     and data integrity over many on_read/on_data callbacks. */
TEST_F(HttpServerTest, LargeFileUpload) {
  constexpr size_t SZ = 10 * 1024 * 1024;

  EchoCtx   echo;
  EchoState state;
  state.ctx = &echo;
  {
    xHttpRouteConf rc = {};
    rc.pattern        = "/echo";
    rc.on_data        = echo_on_data;
    rc.on_done        = echo_handler;
    rc.arg            = &state;
    ASSERT_EQ(xHttpMuxHandle(mux, &rc), xErrno_Ok);
  }
  listen_and_pump();

  xHttpClient client = xHttpClientCreate(nullptr);
  ASSERT_NE(client, nullptr);

  StreamCtx ctx;
  ctx.upload_data.resize(SZ);
  for (size_t i = 0; i < SZ; i++)
    ctx.upload_data[i] = static_cast<char>(i % 251);
  ctx.upload_chunk_size = 16384; /* 16 KB per on_read call */

  std::string      u    = "http://127.0.0.1:" + std::to_string(port) + "/echo";
  xHttpRequestConf conf = {};
  conf.url              = u.c_str();
  conf.method           = xHttpMethod_POST;
  conf.on_read          = provide_data;
  conf.content_length   = SZ;
  conf.on_data          = collect_data;
  conf.on_done          = on_stream_done;
  xErrno err            = xHttpClientDo(client, &conf, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 30000);
  xHttpClientDestroy(client);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  /* Server received the full 10 MB body */
  EXPECT_EQ(echo.last_body.size(), SZ);
  /* Data integrity — byte-for-byte match */
  EXPECT_EQ(echo.last_body, ctx.upload_data);
  /* Client received the full 10 MB echo back */
  EXPECT_EQ(ctx.total_bytes, SZ);
  EXPECT_EQ(ctx.body_acc, ctx.upload_data);
}

/* 8. Large file download (10 MB) — verifies multi-chunk streaming download
 *     and data integrity. */
TEST_F(HttpServerTest, LargeFileDownload) {
  constexpr size_t SZ = 10 * 1024 * 1024;

  std::string big_body(SZ, '\0');
  for (size_t i = 0; i < SZ; i++)
    big_body[i] = static_cast<char>(i % 251);

  route("GET /big", send_body_handler, &big_body);
  listen_and_pump();

  xHttpClient client = xHttpClientCreate(nullptr);
  ASSERT_NE(client, nullptr);

  StreamCtx        ctx;
  std::string      u    = "http://127.0.0.1:" + std::to_string(port) + "/big";
  xHttpRequestConf conf = {};
  conf.url              = u.c_str();
  conf.method           = xHttpMethod_GET;
  conf.on_data          = collect_data;
  conf.on_done          = on_stream_done;
  xErrno err            = xHttpClientDo(client, &conf, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 30000);
  xHttpClientDestroy(client);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_EQ(ctx.total_bytes, SZ);
  /* Data integrity */
  EXPECT_EQ(ctx.body_acc, big_body);
}

/* 9. Fire-and-forget: on_done=NULL, request must not crash. */
TEST_F(HttpServerTest, FireAndForget) {
  route("GET /get", ok_handler, nullptr);
  listen_and_pump();

  xHttpClient client = xHttpClientCreate(nullptr);
  ASSERT_NE(client, nullptr);

  std::string      u    = "http://127.0.0.1:" + std::to_string(port) + "/get";
  xHttpRequestConf conf = {};
  conf.url              = u.c_str();
  conf.method           = xHttpMethod_GET;
  conf.on_done          = nullptr; /* fire-and-forget */
  xErrno err            = xHttpClientDo(client, &conf, nullptr);
  ASSERT_EQ(err, xErrno_Ok);

  run_for(loop, 500); /* let the request complete */
  xHttpClientDestroy(client);
  SUCCEED(); /* test passes if no crash */
}

/* 7. Backward compatibility: body/body_len without on_read/on_data. */
TEST_F(HttpServerTest, BackwardCompat) {
  route("GET /get", ok_handler, nullptr);
  listen_and_pump();

  xHttpClient client = xHttpClientCreate(nullptr);
  ASSERT_NE(client, nullptr);

  ResponseCtx      ctx;
  std::string      u    = "http://127.0.0.1:" + std::to_string(port) + "/get";
  xHttpRequestConf conf = {};
  conf.url              = u.c_str();
  conf.method           = xHttpMethod_GET;
  conf.on_data          = on_data_collect;
  conf.on_done          = on_response;
  xErrno err            = xHttpClientDo(client, &conf, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 5000);
  xHttpClientDestroy(client);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_EQ(ctx.body, "ok");
}

/* ───────────────────── Edge case tests ───────────────────── */

/* 8. on_response abort: client rejects based on status code.
 *    Server returns 404 with a body.  Client's on_response sees 404,
 *    returns 1 → write_callback aborts → on_data never called. */
static int reject_non_200(xHttpCtx *ctx, void *arg) {
  auto *c               = static_cast<StreamCtx *>(arg);
  c->on_response_called = true;
  /* curl_easy_getinfo may not have the status code yet during write_callback,
   * so check the raw headers for the status line. */
  if (ctx->headers && ctx->headers_len > 0) {
    c->headers.assign(ctx->headers, ctx->headers_len);
    /* Look for "HTTP/1.1 200" or "HTTP/2 200" */
    if (c->headers.find(" 200 ") != std::string::npos) return 0; /* OK — continue */
  }
  return 1; /* non-200 → abort */
}

TEST_F(HttpServerTest, OnResponseAbort) {
  route(
    "GET /error",
    [](xHttpCtx *ctx, void *) {
      xHttpCtxSetStatus(ctx, 404);
      xHttpCtxSend(ctx, "Not Found Body", 14);
    },
    nullptr);
  listen_and_pump();

  xHttpClient client = xHttpClientCreate(nullptr);
  ASSERT_NE(client, nullptr);

  StreamCtx        ctx;
  std::string      u    = "http://127.0.0.1:" + std::to_string(port) + "/error";
  xHttpRequestConf conf = {};
  conf.url              = u.c_str();
  conf.method           = xHttpMethod_GET;
  conf.on_response      = reject_non_200;
  conf.on_data          = collect_data;
  conf.on_done          = on_stream_done;
  xErrno err            = xHttpClientDo(client, &conf, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 5000);
  xHttpClientDestroy(client);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_TRUE(ctx.on_response_called);
  EXPECT_NE(ctx.curl_code, 0) << "Should have aborted";
  EXPECT_EQ(ctx.total_bytes, 0u) << "on_data should not have been called";
  /* Verify via headers that it was a 404 */
  ASSERT_FALSE(ctx.headers.empty());
  EXPECT_NE(ctx.headers.find("404"), std::string::npos)
    << "Expected 404 in headers: " << ctx.headers;
}

/* 9. Server on_data abort (413): server's on_data returns non-0 after
 *    receiving some body → 413 sent, on_done NOT called. */
struct LimitCtx {
  std::atomic<bool>   on_done_called{false};
  std::atomic<size_t> total_received{0};
  size_t              limit{0};
};

static int limit_on_data(const char *data, size_t len, void *arg) {
  auto *c = static_cast<LimitCtx *>(arg);
  c->total_received += len;
  if (c->total_received > c->limit) return 1; /* abort → 413 */
  return 0;
}

static void record_on_done(xHttpCtx *ctx, void *arg) {
  auto *c = static_cast<LimitCtx *>(arg);
  c->on_done_called.store(true, std::memory_order_release);
  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSend(ctx, "ok", 2);
}

TEST_F(HttpServerTest, ServerOnDataAbort413) {
  LimitCtx lc;
  lc.limit = 100; /* allow 100 bytes, reject after */

  {
    xHttpRouteConf rc = {};
    rc.pattern        = "POST /upload";
    rc.on_data        = limit_on_data;
    rc.on_done        = record_on_done;
    rc.arg            = &lc;
    ASSERT_EQ(xHttpMuxHandle(mux, &rc), xErrno_Ok);
  }
  listen_and_pump();

  xHttpClient client = xHttpClientCreate(nullptr);
  ASSERT_NE(client, nullptr);

  StreamCtx ctx;
  ctx.upload_data       = std::string(4096, 'x');
  ctx.upload_chunk_size = 1024;
  std::string      u    = "http://127.0.0.1:" + std::to_string(port) + "/upload";
  xHttpRequestConf conf = {};
  conf.url              = u.c_str();
  conf.method           = xHttpMethod_POST;
  conf.on_read          = provide_data;
  conf.content_length   = ctx.upload_data.size();
  conf.on_data          = collect_data;
  conf.on_done          = on_stream_done;
  xErrno err            = xHttpClientDo(client, &conf, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 5000);
  xHttpClientDestroy(client);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  /* on_done should NOT have been called (413 aborts before completion) */
  EXPECT_FALSE(lc.on_done_called.load())
    << "Server on_done should not be called after on_data abort";
  /* Client should receive 413 or a curl error (connection closed during
   * upload).  curl_easy_getinfo may not have the response code if the
   * TLS/backend layer tore down the connection before curl processed
   * the 413 response. */
  if (ctx.status_code != 0) {
    EXPECT_EQ(ctx.status_code, 413);
  } else {
    EXPECT_NE(ctx.curl_code, 0) << "Expected either 413 or a curl error";
  }
}

/* 10. Server on_request rejection (401): on_request sends 401 and returns
 *     non-0 → body not read, on_done NOT called. */
struct AuthCtx {
  std::atomic<bool> on_done_called{false};
  std::atomic<bool> on_data_called{false};
};

static int auth_check(xHttpCtx *ctx, void *arg) {
  (void)arg;
  /* Check for Authorization header */
  if (ctx->headers && ctx->headers_len > 0) {
    std::string hdrs(ctx->headers, ctx->headers_len);
    if (hdrs.find("Authorization: Bearer valid") != std::string::npos) return 0; /* authorized */
  }
  /* Not authorized — send 401 and abort */
  xHttpCtxSetStatus(ctx, 401);
  xHttpCtxSend(ctx, "Unauthorized", 12);
  return 1;
}

static int auth_on_data(const char *, size_t, void *arg) {
  auto *c = static_cast<AuthCtx *>(arg);
  c->on_data_called.store(true, std::memory_order_release);
  return 0;
}

static void auth_on_done(xHttpCtx *ctx, void *arg) {
  auto *c = static_cast<AuthCtx *>(arg);
  c->on_done_called.store(true, std::memory_order_release);
  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSend(ctx, "ok", 2);
}

TEST_F(HttpServerTest, ServerOnRequestReject401) {
  AuthCtx ac;

  {
    xHttpRouteConf rc = {};
    rc.pattern        = "POST /secure";
    rc.on_request     = auth_check;
    rc.on_data        = auth_on_data;
    rc.on_done        = auth_on_done;
    rc.arg            = &ac;
    ASSERT_EQ(xHttpMuxHandle(mux, &rc), xErrno_Ok);
  }
  listen_and_pump();

  xHttpClient client = xHttpClientCreate(nullptr);
  ASSERT_NE(client, nullptr);

  /* POST without Authorization header → should get 401 */
  StreamCtx ctx;
  ctx.upload_data       = std::string(256, 'x');
  ctx.upload_chunk_size = 256;
  std::string      u    = "http://127.0.0.1:" + std::to_string(port) + "/secure";
  xHttpRequestConf conf = {};
  conf.url              = u.c_str();
  conf.method           = xHttpMethod_POST;
  conf.on_read          = provide_data;
  conf.content_length   = ctx.upload_data.size();
  conf.on_data          = collect_data;
  conf.on_done          = on_stream_done;
  xErrno err            = xHttpClientDo(client, &conf, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 5000);
  xHttpClientDestroy(client);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.status_code, 401);
  EXPECT_FALSE(ac.on_done_called.load())
    << "Server on_done should not be called after on_request rejection";
  EXPECT_FALSE(ac.on_data_called.load())
    << "Server on_data should not be called after on_request rejection";
}

/* 11. Custom resolver (no mux): use xHttpResolveFunc directly. */
namespace {

struct CustomResolverCtx {
  std::atomic<bool> handler_called{false};
};

static void custom_handler(xHttpCtx *ctx, void *arg) {
  auto *c = static_cast<CustomResolverCtx *>(arg);
  c->handler_called.store(true, std::memory_order_release);
  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSend(ctx, "from custom resolver", 20);
}

static const xHttpRouteInfo *custom_resolve(void *router, xHttpCtx *ctx) {
  (void)ctx;
  auto                       *c    = static_cast<CustomResolverCtx *>(router);
  static const xHttpRouteInfo info = {
    .on_request = nullptr,
    .on_data    = nullptr,
    .on_done    = custom_handler,
    .arg        = nullptr, /* set below */
  };
  /* arg is set dynamically — use a non-const copy */
  static xHttpRouteInfo dynamic_info = info;
  dynamic_info.arg                   = c;
  return &dynamic_info;
}

} // namespace

TEST(HttpCustomResolver, BasicRequest) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  CustomResolverCtx rctx;

  xHttpServerConf sconf = {};
  sconf.resolve         = custom_resolve;
  sconf.router          = &rctx;
  sconf.idle_timeout_ms = 60000;
  xHttpServer server    = xHttpServerCreate(&sconf);
  ASSERT_NE(server, nullptr);

  uint16_t port = find_free_port();
  ASSERT_NE(port, 0u);
  ASSERT_EQ(xHttpServerListen(server, "127.0.0.1", port), xErrno_Ok);
  run_for(loop, 20);

  xHttpClient client = xHttpClientCreate(nullptr);
  ASSERT_NE(client, nullptr);

  ResponseCtx      rctx2;
  std::string      u    = "http://127.0.0.1:" + std::to_string(port) + "/anything";
  xHttpRequestConf conf = {};
  conf.url              = u.c_str();
  conf.on_data          = on_data_collect;
  conf.on_done          = on_response;
  xErrno err            = xHttpClientDo(client, &conf, &rctx2);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, rctx2.done, 5000);
  xHttpClientDestroy(client);
  xHttpServerDestroy(server);
  xEventLoopLeave();
  xEventLoopDestroy(loop);

  ASSERT_TRUE(rctx2.done.load()) << "Request timed out";
  EXPECT_EQ(rctx2.curl_code, 0);
  EXPECT_EQ(rctx2.status_code, 200);
  EXPECT_EQ(rctx2.body, "from custom resolver");
  EXPECT_TRUE(rctx.handler_called.load());
}

/* 12. Client on_read empty body: on_read returns 0 immediately →
 *     POST with Content-Length: 0, server receives empty body. */
static size_t empty_provide(char *, size_t, void *) {
  return 0; /* EOF immediately — empty body */
}

TEST_F(HttpServerTest, OnReadEmptyBody) {
  EchoCtx   echo;
  EchoState state;
  state.ctx = &echo;
  {
    xHttpRouteConf rc = {};
    rc.pattern        = "POST /echo";
    rc.on_data        = echo_on_data;
    rc.on_done        = echo_handler;
    rc.arg            = &state;
    ASSERT_EQ(xHttpMuxHandle(mux, &rc), xErrno_Ok);
  }
  listen_and_pump();

  xHttpClient client = xHttpClientCreate(nullptr);
  ASSERT_NE(client, nullptr);

  StreamCtx        ctx;
  std::string      u    = "http://127.0.0.1:" + std::to_string(port) + "/echo";
  xHttpRequestConf conf = {};
  conf.url              = u.c_str();
  conf.method           = xHttpMethod_POST;
  conf.on_read          = empty_provide;
  conf.content_length   = 0; /* empty body */
  conf.on_data          = collect_data;
  conf.on_done          = on_stream_done;
  xErrno err            = xHttpClientDo(client, &conf, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 5000);
  xHttpClientDestroy(client);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  /* Server should have received empty body */
  EXPECT_EQ(echo.last_body.size(), 0u);
  /* Client should have received empty echo */
  EXPECT_EQ(ctx.total_bytes, 0u);
}

/* 13. Full lifecycle: on_request → on_data → on_done with shared arg.
 *     Verifies callback order and arg sharing. */
struct LifecycleCtx {
  std::atomic<int> phase{0}; /* 0=init, 1=on_request, 2=on_data, 3=on_done */
  std::string      collected_body;
};

static int lifecycle_on_request(xHttpCtx *ctx, void *arg) {
  auto *c    = static_cast<LifecycleCtx *>(arg);
  int   prev = c->phase.exchange(1, std::memory_order_acq_rel);
  (void)prev;
  /* Verify we can see the request URL */
  return 0;
}

static int lifecycle_on_data(const char *data, size_t len, void *arg) {
  auto *c = static_cast<LifecycleCtx *>(arg);
  c->phase.store(2, std::memory_order_release);
  c->collected_body.append(data, len);
  return 0;
}

static void lifecycle_on_done(xHttpCtx *ctx, void *arg) {
  auto *c = static_cast<LifecycleCtx *>(arg);
  c->phase.store(3, std::memory_order_release);
  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSend(ctx, c->collected_body.data(), c->collected_body.size());
}

TEST_F(HttpServerTest, FullLifecycleOrder) {
  LifecycleCtx lc;

  {
    xHttpRouteConf rc = {};
    rc.pattern        = "POST /lifecycle";
    rc.on_request     = lifecycle_on_request;
    rc.on_data        = lifecycle_on_data;
    rc.on_done        = lifecycle_on_done;
    rc.arg            = &lc;
    ASSERT_EQ(xHttpMuxHandle(mux, &rc), xErrno_Ok);
  }
  listen_and_pump();

  xHttpClient client = xHttpClientCreate(nullptr);
  ASSERT_NE(client, nullptr);

  StreamCtx ctx;
  ctx.upload_data       = std::string(256, 'L');
  ctx.upload_chunk_size = 256;
  std::string      u    = "http://127.0.0.1:" + std::to_string(port) + "/lifecycle";
  xHttpRequestConf conf = {};
  conf.url              = u.c_str();
  conf.method           = xHttpMethod_POST;
  conf.on_read          = provide_data;
  conf.content_length   = ctx.upload_data.size();
  conf.on_data          = collect_data;
  conf.on_done          = on_stream_done;
  xErrno err            = xHttpClientDo(client, &conf, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 5000);
  xHttpClientDestroy(client);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  /* on_done was called (phase 3) */
  EXPECT_EQ(lc.phase.load(), 3);
  /* Server collected the full body */
  EXPECT_EQ(lc.collected_body.size(), 256u);
  EXPECT_EQ(lc.collected_body, ctx.upload_data);
  /* Client received the echo */
  EXPECT_EQ(ctx.total_bytes, 256u);
  EXPECT_EQ(ctx.body_acc, ctx.upload_data);
}
