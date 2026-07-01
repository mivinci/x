/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * http_test.cpp - Integration tests: xhttp client ↔ server
 *
 * Spins up a real xHttpServer and uses xHttpClient to talk to it,
 * exercising the full request/response path over HTTP/1.1 and HTTP/2.
 */

#include "server_test_helper.h"

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <x/http/client.h>
#include <x/http/server.h>

/* ───────────────────── Response context ───────────────────── */

struct RespCtx {
  std::atomic<bool> done{false};
  long              status_code{0};
  int               curl_code{-1};
  std::string       body;
  std::string       headers;
  std::string       upload_data;
  size_t            upload_offset{0};
};

static void on_resp(xHttpCtx *ctx, void *arg) {
  auto *c        = static_cast<RespCtx *>(arg);
  c->status_code = ctx->status_code;
  c->curl_code   = ctx->curl_code;
  if (ctx->headers && ctx->headers_len > 0) c->headers.assign(ctx->headers, ctx->headers_len);
  c->done.store(true, std::memory_order_release);
}

static int on_data_collect(const char *data, size_t len, void *arg) {
  auto *c = static_cast<RespCtx *>(arg);
  c->body.append(data, len);
  return 0;
}

static size_t on_read_provide(char *buf, size_t bufsize, void *arg) {
  auto  *c         = static_cast<RespCtx *>(arg);
  size_t remaining = c->upload_data.size() - c->upload_offset;
  if (remaining == 0) return 0;
  size_t n = bufsize < remaining ? bufsize : remaining;
  memcpy(buf, c->upload_data.data() + c->upload_offset, n);
  c->upload_offset += n;
  return n;
}

/* ───────────────────── SSE context ───────────────────── */

struct SseTestCtx {
  std::vector<std::string> events;
  std::vector<std::string> data;
  std::atomic<int>         event_count{0};
  std::atomic<bool>        done{false};
  int                      done_curl_code{-1};
};

static int on_sse_ev(const xSseEvent *ev, void *arg) {
  auto *ctx = static_cast<SseTestCtx *>(arg);
  ctx->events.emplace_back(ev->event ? ev->event : "");
  ctx->data.emplace_back(ev->data ? ev->data : "");
  ctx->event_count.fetch_add(1, std::memory_order_release);
  return 0;
}

static void on_sse_end(int curl_code, void *arg) {
  auto *ctx           = static_cast<SseTestCtx *>(arg);
  ctx->done_curl_code = curl_code;
  ctx->done.store(true, std::memory_order_release);
}

/* ───────────────────── Server handlers ───────────────────── */

static void hello_handler(xHttpCtx *ctx, void *arg) {
  (void)arg;
  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSetHeader(ctx, "Content-Type", "text/plain");
  xHttpCtxSend(ctx, "hello", 5);
}

/* Body echo: accumulate body via on_data, echo back in on_done */
struct EchoBodyCtx {
  std::string body;
};

static int echo_body_on_data(const char *data, size_t len, void *arg) {
  auto *c = static_cast<EchoBodyCtx *>(arg);
  c->body.append(data, len);
  return 0;
}

static void echo_body_on_done(xHttpCtx *ctx, void *arg) {
  auto *c = static_cast<EchoBodyCtx *>(arg);
  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSetHeader(ctx, "Content-Type", "application/octet-stream");
  xHttpCtxSend(ctx, c->body.data(), c->body.size());
}

static void echo_header_handler(xHttpCtx *ctx, void *arg) {
  (void)arg;
  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSetHeader(ctx, "Content-Type", "text/plain");
  xHttpCtxSend(ctx, ctx->headers, ctx->headers_len);
}

static void sse_handler(xHttpCtx *ctx, void *arg) {
  (void)arg;
  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSetHeader(ctx, "Content-Type", "text/event-stream");
  xHttpCtxSetHeader(ctx, "Cache-Control", "no-cache");

  xHttpCtxWrite(ctx, "data: alpha\n\n", 13);
  xHttpCtxWrite(ctx, "event: custom\ndata: beta\n\n", 26);
  xHttpCtxWrite(ctx, "data: gamma\n\n", 13);
  xHttpCtxEndStream(ctx);
}

/* ───────────────────── Fixture ───────────────────── */

class IntegrationTest : public ::testing::Test {
protected:
  xEventLoop  loop   = nullptr;
  xHttpServer server = nullptr;
  xHttpMux    mux    = nullptr;
  xHttpClient client = nullptr;
  uint16_t    port   = 0;

  void SetUp() override {
    loop = xEventLoopCreate();
    ASSERT_NE(loop, nullptr);
    xEventLoopEnter(loop);

    mux = xHttpMuxCreate();
    ASSERT_NE(mux, nullptr);

    xHttpServerConf conf = {};
    conf.resolve         = xHttpMuxResolve;
    conf.router          = mux;
    conf.idle_timeout_ms = 60000;

    server = xHttpServerCreate(&conf);
    ASSERT_NE(server, nullptr);

    client = xHttpClientCreate(nullptr);
    ASSERT_NE(client, nullptr);

    port = find_free_port();
    ASSERT_NE(port, 0) << "Could not find a free port";
  }

  void TearDown() override {
    if (client) xHttpClientDestroy(client);
    if (server) xHttpServerDestroy(server);
    if (mux) xHttpMuxDestroy(mux);
    xEventLoopLeave();
    if (loop) xEventLoopDestroy(loop);
  }

  void listen_and_pump() {
    xErrno err = xHttpServerListen(server, "127.0.0.1", port);
    ASSERT_EQ(err, xErrno_Ok) << "Failed to listen on port " << port;
    run_for(loop, 20);
  }

  std::string make_url(const char *path) {
    return "http://127.0.0.1:" + std::to_string(port) + path;
  }

  void route(const char *pattern, xHttpDoneFunc on_done, void *arg = nullptr) {
    xHttpRouteConf conf = {};
    conf.pattern        = pattern;
    conf.on_done        = on_done;
    conf.arg            = arg;
    ASSERT_EQ(xHttpMuxHandle(mux, &conf), xErrno_Ok);
  }

  void route_with_data(const char *pattern, xHttpDataFunc on_data, xHttpDoneFunc on_done,
                       void *arg) {
    xHttpRouteConf conf = {};
    conf.pattern        = pattern;
    conf.on_data        = on_data;
    conf.on_done        = on_done;
    conf.arg            = arg;
    ASSERT_EQ(xHttpMuxHandle(mux, &conf), xErrno_Ok);
  }
};

/* ───────────────────── H1 GET ───────────────────── */

TEST_F(IntegrationTest, H1Get) {
  route("GET /hello", hello_handler);
  listen_and_pump();

  RespCtx          ctx;
  std::string      url  = make_url("/hello");
  xHttpRequestConf conf = {};
  conf.url              = url.c_str();
  conf.on_data          = on_data_collect;
  conf.on_done          = on_resp;
  xErrno err            = xHttpClientGet(client, &conf, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_EQ(ctx.body, "hello");
}

/* ───────────────────── H1 POST with body echo ───────────────────── */

TEST_F(IntegrationTest, H1PostEcho) {
  EchoBodyCtx echo_ctx;
  route_with_data("POST /echo", echo_body_on_data, echo_body_on_done, &echo_ctx);
  listen_and_pump();

  RespCtx     ctx;
  std::string url  = make_url("/echo");
  const char *body = "{\"msg\":\"integration\"}";
  ctx.upload_data.assign(body, strlen(body));
  xHttpRequestConf conf = {};
  conf.url              = url.c_str();
  conf.on_read          = on_read_provide;
  conf.content_length   = strlen(body);
  conf.on_data          = on_data_collect;
  conf.on_done          = on_resp;
  xErrno err            = xHttpClientPost(client, &conf, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_EQ(ctx.body, body);
}

/* ───────────────────── H1 Do with custom headers ───────────────────── */

TEST_F(IntegrationTest, H1DoCustomHeaders) {
  route("GET /headers", echo_header_handler);
  listen_and_pump();

  RespCtx     ctx;
  std::string url = make_url("/headers");

  const char      *hdrs[] = {"X-Test-Key: test-value-123", NULL};
  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url     = url.c_str();
  config.method  = xHttpMethod_GET;
  config.headers = hdrs;
  config.on_data = on_data_collect;
  config.on_done = on_resp;

  xErrno err = xHttpClientDo(client, &config, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_NE(ctx.body.find("X-Test-Key"), std::string::npos);
  EXPECT_NE(ctx.body.find("test-value-123"), std::string::npos);
}

/* ───────────────────── H1 404 Not Found ───────────────────── */

TEST_F(IntegrationTest, H1NotFound) {
  route("GET /exists", hello_handler);
  listen_and_pump();

  RespCtx          ctx;
  std::string      url  = make_url("/nonexistent");
  xHttpRequestConf conf = {};
  conf.url              = url.c_str();
  conf.on_data          = on_data_collect;
  conf.on_done          = on_resp;
  xErrno err            = xHttpClientGet(client, &conf, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 404);
}

/* ───────────────────── H2C Prior Knowledge GET ───────────────────── */

TEST_F(IntegrationTest, H2cGet) {
  route("GET /hello", hello_handler);
  listen_and_pump();

  RespCtx     ctx;
  std::string url = make_url("/hello");

  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url          = url.c_str();
  config.method       = xHttpMethod_GET;
  config.http_version = xHttpVersion_H2C;
  config.on_data      = on_data_collect;
  config.on_done      = on_resp;

  xErrno err = xHttpClientDo(client, &config, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "H2C request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_EQ(ctx.body, "hello");
}

/* ───────────────────── H2C POST with body echo ───────────────────── */

TEST_F(IntegrationTest, H2cPostEcho) {
  EchoBodyCtx echo_ctx;
  route_with_data("POST /echo", echo_body_on_data, echo_body_on_done, &echo_ctx);
  listen_and_pump();

  RespCtx     ctx;
  std::string url  = make_url("/echo");
  const char *body = "h2c-body-test";
  ctx.upload_data.assign(body, strlen(body));

  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url            = url.c_str();
  config.method         = xHttpMethod_POST;
  config.on_read        = on_read_provide;
  config.content_length = strlen(body);
  config.http_version   = xHttpVersion_H2C;
  config.on_data        = on_data_collect;
  config.on_done        = on_resp;

  xErrno err = xHttpClientDo(client, &config, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "H2C POST request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_EQ(ctx.body, body);
}

/* ───────────────────── Client default HTTP version ───────────────────── */

TEST_F(IntegrationTest, ClientDefaultH2c) {
  route("GET /hello", hello_handler);
  listen_and_pump();

  xHttpClientDestroy(client);
  xHttpClientConf conf = {};
  conf.http_version    = xHttpVersion_H2C;
  client               = xHttpClientCreate(&conf);
  ASSERT_NE(client, nullptr);

  RespCtx     ctx;
  std::string url = make_url("/hello");

  xHttpRequestConf req_conf = {};
  req_conf.url              = url.c_str();
  req_conf.on_data          = on_data_collect;
  req_conf.on_done          = on_resp;
  xErrno err                = xHttpClientGet(client, &req_conf, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "H2C request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_EQ(ctx.body, "hello");
}

/* ───────────────────── SSE GET ───────────────────── */

TEST_F(IntegrationTest, SseGet) {
  route("GET /events", sse_handler);
  listen_and_pump();

  SseTestCtx  ctx;
  std::string url = make_url("/events");

  xErrno err = xHttpClientGetSse(client, url.c_str(), on_sse_ev, on_sse_end, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "SSE stream did not finish";
  EXPECT_EQ(ctx.done_curl_code, 0);
  ASSERT_EQ(ctx.event_count.load(), 3);
  EXPECT_EQ(ctx.data[0], "alpha");
  EXPECT_EQ(ctx.events[1], "custom");
  EXPECT_EQ(ctx.data[1], "beta");
  EXPECT_EQ(ctx.data[2], "gamma");
}

/* ───────────────────── SSE GET over H2C ─���─────────────────── */

TEST_F(IntegrationTest, SseGetH2c) {
  route("GET /events", sse_handler);
  listen_and_pump();

  SseTestCtx  ctx;
  std::string url = make_url("/events");

  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url          = url.c_str();
  config.http_version = xHttpVersion_H2C;

  xErrno err = xHttpClientDoSse(client, &config, on_sse_ev, on_sse_end, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "SSE H2C stream did not finish";
  EXPECT_EQ(ctx.done_curl_code, 0);
  ASSERT_EQ(ctx.event_count.load(), 3);
}

/* ───────────────────── H1 404 via client ───────────────────── */

TEST_F(IntegrationTest, H1NotFoundClient) {
  route("GET /exists", hello_handler);
  listen_and_pump();

  RespCtx     ctx;
  std::string url = make_url("/nope");

  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url     = url.c_str();
  config.method  = xHttpMethod_GET;
  config.on_data = on_data_collect;
  config.on_done = on_resp;

  xErrno err = xHttpClientDo(client, &config, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.status_code, 404);
}

/* ───────────────────── H1 custom headers via client ───────────────────── */

TEST_F(IntegrationTest, H1CustomHeaders) {
  route("GET /headers", echo_header_handler);
  listen_and_pump();

  RespCtx     ctx;
  std::string url = make_url("/headers");

  const char      *hdrs[] = {"X-Custom: my-value", NULL};
  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url     = url.c_str();
  config.method  = xHttpMethod_GET;
  config.headers = hdrs;
  config.on_data = on_data_collect;
  config.on_done = on_resp;

  xErrno err = xHttpClientDo(client, &config, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_NE(ctx.body.find("X-Custom"), std::string::npos);
  EXPECT_NE(ctx.body.find("my-value"), std::string::npos);
}

/* ───────────────────── Route params ───────────────────── */

static void param_echo_handler(xHttpCtx *ctx, void *arg) {
  (void)arg;
  size_t      len = 0;
  const char *id  = xHttpCtxParam(ctx, "id", &len);

  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSetHeader(ctx, "Content-Type", "text/plain");
  if (id && len > 0) {
    char buf[128];
    int  n = snprintf(buf, sizeof(buf), "id=%.*s", static_cast<int>(len), id);
    xHttpCtxSend(ctx, buf, static_cast<size_t>(n));
  } else {
    xHttpCtxSend(ctx, "id=none", 7);
  }
}

TEST_F(IntegrationTest, H1RouteParam) {
  route("GET /users/:id", param_echo_handler);
  listen_and_pump();

  RespCtx          ctx;
  std::string      url  = make_url("/users/42");
  xHttpRequestConf conf = {};
  conf.url              = url.c_str();
  conf.on_data          = on_data_collect;
  conf.on_done          = on_resp;
  xErrno err            = xHttpClientGet(client, &conf, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "Request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_EQ(ctx.body, "id=42");
}

/* ───────────────────── Route params over H2C ───────────────────── */

TEST_F(IntegrationTest, H2cRouteParam) {
  route("GET /users/:id", param_echo_handler);
  listen_and_pump();

  RespCtx     ctx;
  std::string url = make_url("/users/alice");

  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url          = url.c_str();
  config.method       = xHttpMethod_GET;
  config.http_version = xHttpVersion_H2C;
  config.on_data      = on_data_collect;
  config.on_done      = on_resp;

  xErrno err = xHttpClientDo(client, &config, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "H2C param request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_EQ(ctx.body, "id=alice");
}

/* ───────────────────── PUT method ───────────────────── */

struct PutCtx {
  std::string body;
};

static int put_on_data(const char *data, size_t len, void *arg) {
  auto *c = static_cast<PutCtx *>(arg);
  c->body.append(data, len);
  return 0;
}

static void put_on_done(xHttpCtx *ctx, void *arg) {
  auto *c = static_cast<PutCtx *>(arg);
  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSetHeader(ctx, "Content-Type", "text/plain");
  char buf[256];
  int  n = snprintf(buf, sizeof(buf), "%s:%s", ctx->method, c->body.c_str());
  xHttpCtxSend(ctx, buf, static_cast<size_t>(n));
}

TEST_F(IntegrationTest, H1PutMethod) {
  PutCtx put_ctx;
  route_with_data("PUT /resource", put_on_data, put_on_done, &put_ctx);
  listen_and_pump();

  RespCtx     ctx;
  std::string url  = make_url("/resource");
  const char *body = "updated-data";
  ctx.upload_data.assign(body, strlen(body));

  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url            = url.c_str();
  config.method         = xHttpMethod_PUT;
  config.on_read        = on_read_provide;
  config.content_length = strlen(body);
  config.on_data        = on_data_collect;
  config.on_done        = on_resp;

  xErrno err = xHttpClientDo(client, &config, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "PUT request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_EQ(ctx.body, "PUT:updated-data");
}

/* ───────────────────── DELETE method over H2C ───────────────────── */

static void delete_handler(xHttpCtx *ctx, void *arg) {
  (void)arg;
  xHttpCtxSetStatus(ctx, 204);
  xHttpCtxSend(ctx, NULL, 0);
}

TEST_F(IntegrationTest, H2cDeleteMethod) {
  route("DELETE /resource", delete_handler);
  listen_and_pump();

  RespCtx     ctx;
  std::string url = make_url("/resource");

  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url          = url.c_str();
  config.method       = xHttpMethod_DELETE;
  config.http_version = xHttpVersion_H2C;
  config.on_data      = on_data_collect;
  config.on_done      = on_resp;

  xErrno err = xHttpClientDo(client, &config, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "DELETE request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 204);
  EXPECT_TRUE(ctx.body.empty());
}

/* ───────────────────── Large body round-trip ───────────────────── */

TEST_F(IntegrationTest, LargeBodyRoundTrip) {
  EchoBodyCtx echo_ctx;
  route_with_data("POST /echo", echo_body_on_data, echo_body_on_done, &echo_ctx);
  listen_and_pump();

  std::string large_body(4000, 'X');

  RespCtx ctx;
  ctx.upload_data       = large_body;
  std::string      url  = make_url("/echo");
  xHttpRequestConf conf = {};
  conf.url              = url.c_str();
  conf.on_read          = on_read_provide;
  conf.content_length   = large_body.size();
  conf.on_data          = on_data_collect;
  conf.on_done          = on_resp;
  xErrno err            = xHttpClientPost(client, &conf, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 10000);

  ASSERT_TRUE(ctx.done.load()) << "Large body request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 200);
  EXPECT_EQ(ctx.body.size(), large_body.size());
  EXPECT_EQ(ctx.body, large_body);
}

/* ───────────────────── SSE POST context (combines SSE + upload) ───────────────────── */

struct SsePostCtx {
  SseTestCtx  sse;
  std::string upload_data;
  size_t      upload_offset{0};
  std::string body;
};

static size_t sse_post_on_read(char *buf, size_t bufsize, void *arg) {
  auto  *c         = static_cast<SsePostCtx *>(arg);
  size_t remaining = c->upload_data.size() - c->upload_offset;
  if (remaining == 0) return 0;
  size_t n = bufsize < remaining ? bufsize : remaining;
  memcpy(buf, c->upload_data.data() + c->upload_offset, n);
  c->upload_offset += n;
  return n;
}

static int sse_post_on_ev(const xSseEvent *ev, void *arg) {
  auto *c = static_cast<SsePostCtx *>(arg);
  return on_sse_ev(ev, &c->sse);
}

static void sse_post_on_end(int curl_code, void *arg) {
  auto *c = static_cast<SsePostCtx *>(arg);
  on_sse_end(curl_code, &c->sse);
}

static int sse_post_on_data(const char *data, size_t len, void *arg) {
  auto *c = static_cast<SsePostCtx *>(arg);
  c->body.append(data, len);
  return 0;
}

/* ───────────────────── SSE via DoSse POST (LLM-style) ───────────────────── */

static void sse_post_handler(xHttpCtx *ctx, void *arg) {
  auto *c = static_cast<SsePostCtx *>(arg);
  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSetHeader(ctx, "Content-Type", "text/event-stream");
  xHttpCtxSetHeader(ctx, "Cache-Control", "no-cache");

  char buf[512];
  int  n = snprintf(buf, sizeof(buf), "data: %s\n\n", c->body.c_str());
  xHttpCtxWrite(ctx, buf, static_cast<size_t>(n));
  xHttpCtxWrite(ctx, "event: done\ndata: [DONE]\n\n", 26);
  xHttpCtxEndStream(ctx);
}

TEST_F(IntegrationTest, SseDoPostH1) {
  SsePostCtx sse_ctx;
  route_with_data("POST /v1/chat", sse_post_on_data, sse_post_handler, &sse_ctx);
  listen_and_pump();

  RespCtx     ctx;
  std::string url  = make_url("/v1/chat");
  const char *body = "{\"model\":\"gpt-4\"}";
  sse_ctx.upload_data.assign(body, strlen(body));

  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url            = url.c_str();
  config.method         = xHttpMethod_POST;
  config.on_read        = sse_post_on_read;
  config.content_length = strlen(body);

  xErrno err = xHttpClientDoSse(client, &config, sse_post_on_ev, sse_post_on_end, &sse_ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, sse_ctx.sse.done, 5000);

  ASSERT_TRUE(sse_ctx.sse.done.load()) << "SSE POST stream did not finish";
  EXPECT_EQ(sse_ctx.sse.done_curl_code, 0);
  ASSERT_EQ(sse_ctx.sse.event_count.load(), 2);
  EXPECT_EQ(sse_ctx.sse.data[0], body);
  EXPECT_EQ(sse_ctx.sse.events[1], "done");
  EXPECT_EQ(sse_ctx.sse.data[1], "[DONE]");
}

/* ───────────────────── SSE via DoSse POST over H2C ───────────────────── */

TEST_F(IntegrationTest, SseDoPostH2c) {
  SsePostCtx sse_ctx;
  route_with_data("POST /v1/chat", sse_post_on_data, sse_post_handler, &sse_ctx);
  listen_and_pump();

  std::string url  = make_url("/v1/chat");
  const char *body = "{\"stream\":true}";
  sse_ctx.upload_data.assign(body, strlen(body));

  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url            = url.c_str();
  config.method         = xHttpMethod_POST;
  config.on_read        = sse_post_on_read;
  config.content_length = strlen(body);
  config.http_version   = xHttpVersion_H2C;

  xErrno err = xHttpClientDoSse(client, &config, sse_post_on_ev, sse_post_on_end, &sse_ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, sse_ctx.sse.done, 5000);

  ASSERT_TRUE(sse_ctx.sse.done.load()) << "SSE POST/H2C stream did not finish";
  EXPECT_EQ(sse_ctx.sse.done_curl_code, 0);
  ASSERT_EQ(sse_ctx.sse.event_count.load(), 2);
  EXPECT_EQ(sse_ctx.sse.data[0], body);
  EXPECT_EQ(sse_ctx.sse.events[1], "done");
  EXPECT_EQ(sse_ctx.sse.data[1], "[DONE]");
}

/* ───────────────────── Empty body response (204) ───────────────────── */

TEST_F(IntegrationTest, H1EmptyBodyResponse) {
  route("DELETE /item", delete_handler);
  listen_and_pump();

  RespCtx     ctx;
  std::string url = make_url("/item");

  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url     = url.c_str();
  config.method  = xHttpMethod_DELETE;
  config.on_data = on_data_collect;
  config.on_done = on_resp;

  xErrno err = xHttpClientDo(client, &config, &ctx);
  ASSERT_EQ(err, xErrno_Ok);

  run_until(loop, ctx.done, 5000);

  ASSERT_TRUE(ctx.done.load()) << "204 request timed out";
  EXPECT_EQ(ctx.curl_code, 0);
  EXPECT_EQ(ctx.status_code, 204);
  EXPECT_TRUE(ctx.body.empty());
}

/* ───────────────────── Concurrent H1 + H2C requests ───────────────────── */

TEST_F(IntegrationTest, ConcurrentH1AndH2c) {
  route("GET /hello", hello_handler);
  listen_and_pump();

  RespCtx     ctx_h1, ctx_h2c;
  std::string url = make_url("/hello");

  /* Use separate clients — curl may reuse the H1 connection for
   * the H2C request, which fails because H2C prior-knowledge
   * requires a fresh connection to send the HTTP/2 preface. */
  xHttpClient client_h2c = xHttpClientCreate(nullptr);
  ASSERT_NE(client_h2c, nullptr);

  xHttpRequestConf req_conf = {};
  req_conf.url              = url.c_str();
  req_conf.method           = xHttpMethod_GET;
  req_conf.on_data          = on_data_collect;
  req_conf.on_done          = on_resp;
  xErrno err1               = xHttpClientGet(client, &req_conf, &ctx_h1);
  ASSERT_EQ(err1, xErrno_Ok);

  xHttpRequestConf config;
  memset(&config, 0, sizeof(config));
  config.url          = url.c_str();
  config.method       = xHttpMethod_GET;
  config.http_version = xHttpVersion_H2C;
  config.on_data      = on_data_collect;
  config.on_done      = on_resp;

  xErrno err2 = xHttpClientDo(client_h2c, &config, &ctx_h2c);
  ASSERT_EQ(err2, xErrno_Ok);

  /* Wait for both concurrently */
  struct BothDone {
    std::atomic<bool> *a, *b;
  } both{&ctx_h1.done, &ctx_h2c.done};

  xTimer checker = xTimerStart(
    [](void *arg) {
      auto *b = static_cast<BothDone *>(arg);
      if (b->a->load(std::memory_order_acquire) && b->b->load(std::memory_order_acquire))
        xEventLoopStop(xEventLoopCurrent());
    },
    &both, 5, 5);
  xTimer watchdog =
    xTimerStart([](void *arg) { xEventLoopStop(static_cast<xEventLoop>(arg)); }, loop, 5000, 0);

  xEventLoopRun(loop, X_RUN_DEFAULT);
  if (checker) xTimerStop(checker);
  if (watchdog) xTimerStop(watchdog);

  xHttpClientDestroy(client_h2c);

  ASSERT_TRUE(ctx_h1.done.load()) << "H1 request timed out";
  EXPECT_EQ(ctx_h1.curl_code, 0);
  EXPECT_EQ(ctx_h1.status_code, 200);
  EXPECT_EQ(ctx_h1.body, "hello");

  ASSERT_TRUE(ctx_h2c.done.load()) << "H2C request timed out";
  EXPECT_EQ(ctx_h2c.curl_code, 0);
  EXPECT_EQ(ctx_h2c.status_code, 200);
  EXPECT_EQ(ctx_h2c.body, "hello");
}
