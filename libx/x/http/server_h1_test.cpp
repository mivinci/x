/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * server_h1_test.cpp - HTTP/1.1 unit tests for xhttp (async HTTP server)
 */

#include "server_test_helper.h"

/* ───────────────────── Lifecycle tests ───────────────────── */

TEST(HttpServerLifecycle, CreateAndDestroy) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  xHttpServerConf conf = {};
  xHttpServer s = xHttpServerCreate(&conf);
  ASSERT_NE(s, nullptr);

  xHttpServerDestroy(s);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(HttpServerLifecycle, CreateWithoutEnteringLoopSucceeds) {
  xHttpServerConf conf = {};
  xHttpServer s = xHttpServerCreate(&conf);
  (void)s;
}

TEST(HttpServerLifecycle, DestroyNullIsNoop) {
  xHttpServerDestroy(nullptr);
}

/* ───────────────────── Listen tests ───────────────────── */

TEST_F(HttpServerTest, ListenOnFreePort) {
  xErrno err = xHttpServerListen(server, "127.0.0.1", port);
  EXPECT_EQ(err, xErrno_Ok);
}

TEST_F(HttpServerTest, ListenNullServerReturnsError) {
  xErrno err = xHttpServerListen(nullptr, "127.0.0.1", port);
  EXPECT_EQ(err, xErrno_InvalidArg);
}

TEST_F(HttpServerTest, ListenInvalidHostReturnsError) {
  xErrno err = xHttpServerListen(server, "not.an.ip.address", port);
  EXPECT_EQ(err, xErrno_InvalidArg);
}

/* ───────────────────── Configuration tests ───────────────────── */

TEST_F(HttpServerTest, SetMaxHeaderSize) {
  EXPECT_EQ(xHttpServerSetMaxHeaderSize(server, 4096), xErrno_Ok);
  EXPECT_EQ(xHttpServerSetMaxHeaderSize(server, 0), xErrno_InvalidArg);
  EXPECT_EQ(xHttpServerSetMaxHeaderSize(nullptr, 4096), xErrno_InvalidArg);
}

/* ───────────────────── Basic GET request ───────────────────── */

static void echo_handler(xHttpCtx *ctx, void *arg) {
  auto *c = static_cast<HandlerCtx *>(arg);
  c->last_method = ctx->method;
  c->last_url    = ctx->url;
  c->call_count.fetch_add(1, std::memory_order_release);

  const char *body = "Hello, World!";
  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSetHeader(ctx, "Content-Type", "text/plain");
  xHttpCtxSend(ctx, body, strlen(body));
}

TEST_F(HttpServerTest, BasicGetRequest) {
  HandlerCtx ctx;
  route("GET /hello", echo_handler, &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0) << "Failed to connect";

  std::string request = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));

  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_EQ(ctx.call_count.load(), 1);
  EXPECT_EQ(ctx.last_method, "GET");
  EXPECT_EQ(ctx.last_url, "/hello");

  EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_NE(response.find("Content-Type: text/plain"), std::string::npos);
  EXPECT_NE(response.find("Hello, World!"), std::string::npos);
}

/* ───────────────────── POST request with body ───────────────────── */

struct BodyEchoCtx {
  std::atomic<int> call_count{0};
  std::string      last_method;
  std::string      last_url;
  std::string      body;
};

static int echo_body_on_data(const char *data, size_t len, void *arg) {
  auto *c = static_cast<BodyEchoCtx *>(arg);
  c->body.append(data, len);
  return 0;
}

static void echo_body_on_done(xHttpCtx *ctx, void *arg) {
  auto *c = static_cast<BodyEchoCtx *>(arg);
  c->last_method = ctx->method;
  c->last_url    = ctx->url;
  c->call_count.fetch_add(1, std::memory_order_release);

  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSend(ctx, c->body.data(), c->body.size());
}

TEST_F(HttpServerTest, PostRequestWithBody) {
  BodyEchoCtx ctx;
  xHttpRouteConf conf = {};
  conf.pattern        = "POST /echo";
  conf.on_data        = echo_body_on_data;
  conf.on_done        = echo_body_on_done;
  conf.arg            = &ctx;
  ASSERT_EQ(xHttpMuxHandle(mux, &conf), xErrno_Ok);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string body    = "{\"key\":\"value\"}";
  std::string request = "POST /echo HTTP/1.1\r\n"
                        "Host: localhost\r\n"
                        "Content-Length: " +
                        std::to_string(body.size()) +
                        "\r\n"
                        "\r\n" +
                        body;
  ASSERT_TRUE(send_str(fd, request));

  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_EQ(ctx.call_count.load(), 1);
  EXPECT_EQ(ctx.last_method, "POST");
  EXPECT_EQ(ctx.body, body);
  EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_NE(response.find(body), std::string::npos);
}

/* ───────────────────── 404 Not Found ───────────────────── */

static void dummy_handler(xHttpCtx *ctx, void *) { xHttpCtxSend(ctx, "", 0); }

TEST_F(HttpServerTest, NotFoundResponse) {
  route("GET /exists", dummy_handler);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string request = "GET /nonexistent HTTP/1.1\r\nHost: localhost\r\n"
                        "Connection: close\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));

  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_NE(response.find("HTTP/1.1 404 Not Found"), std::string::npos);
}

/* ───────────────────── 405 Method Not Allowed ───────────────────── */

TEST_F(HttpServerTest, MethodNotAllowedResponse) {
  route("GET /only-get", dummy_handler);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string request = "POST /only-get HTTP/1.1\r\nHost: localhost\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));

  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_NE(response.find("HTTP/1.1 405 Method Not Allowed"), std::string::npos);
}

/* ───────────────────── Keep-alive connection reuse ───────────────────── */

TEST_F(HttpServerTest, KeepAliveConnectionReuse) {
  HandlerCtx ctx;
  route("GET /ka", echo_handler, &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string req1 = "GET /ka HTTP/1.1\r\nHost: localhost\r\n\r\n";
  ASSERT_TRUE(send_str(fd, req1));
  run_for(loop, 100);

  std::string resp1 = recv_all(fd, 1000);
  EXPECT_NE(resp1.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_EQ(ctx.call_count.load(), 1);

  std::string req2 = "GET /ka HTTP/1.1\r\nHost: localhost\r\n\r\n";
  ASSERT_TRUE(send_str(fd, req2));
  run_for(loop, 100);

  std::string resp2 = recv_all(fd, 1000);
  EXPECT_NE(resp2.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_EQ(ctx.call_count.load(), 2);

  close(fd);
}

/* ───────────────────── Default 200 OK when handler doesn't respond ──── */

TEST_F(HttpServerTest, DefaultResponseWhenHandlerDoesNotSend) {
  route("GET /noop", dummy_handler);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string request = "GET /noop HTTP/1.1\r\nHost: localhost\r\n"
                        "Connection: close\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));

  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
}

/* ───────────────────── Bad request (parse error) ───────────────────── */

TEST_F(HttpServerTest, BadRequestOnParseError) {
  route("GET /test", dummy_handler);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string request = "INVALID GARBAGE\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));

  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_NE(response.find("400 Bad Request"), std::string::npos);
}

/* ───────────────────── Header too large → 431 ───────────────────── */

TEST_F(HttpServerTest, HeaderTooLargeReturns431) {
  xHttpServerSetMaxHeaderSize(server, 128);
  route("GET /test", dummy_handler);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string large_header(256, 'X');
  std::string request = "GET /test HTTP/1.1\r\n"
                        "Host: localhost\r\n"
                        "X-Large: " +
                        large_header +
                        "\r\n"
                        "Connection: close\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));

  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_NE(response.find("431"), std::string::npos);
}

/* ───────────────────── Client disconnect (half-close) ───────────────────── */

TEST_F(HttpServerTest, ClientDisconnectDoesNotCrash) {
  route("GET /test", dummy_handler);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string partial = "GET /test HTTP/1.1\r\nHost: lo";
  send_str(fd, partial);
  close(fd);

  run_for(loop, 100);
}

/* ───────────────────── NULL method matches all methods ───────────────────── */

TEST_F(HttpServerTest, NullMethodMatchesAll) {
  HandlerCtx ctx;
  route("/any", echo_handler, &ctx);
  listen_and_pump();

  {
    int fd = connect_to(port);
    ASSERT_GE(fd, 0);
    std::string request = "GET /any HTTP/1.1\r\nHost: localhost\r\n"
                          "Connection: close\r\n\r\n";
    ASSERT_TRUE(send_str(fd, request));
    run_for(loop, 100);
    std::string response = recv_all(fd);
    close(fd);
    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
  }

  {
    int fd = connect_to(port);
    ASSERT_GE(fd, 0);
    std::string request = "POST /any HTTP/1.1\r\nHost: localhost\r\n"
                          "Content-Length: 0\r\n"
                          "Connection: close\r\n\r\n";
    ASSERT_TRUE(send_str(fd, request));
    run_for(loop, 100);
    std::string response = recv_all(fd);
    close(fd);
    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
  }

  EXPECT_EQ(ctx.call_count.load(), 2);
}

/* ───────────────────── Destroy with active connections ───────────────────── */

TEST_F(HttpServerTest, DestroyWithActiveConnections) {
  route("GET /test", dummy_handler);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  run_for(loop, 50);

  xHttpServerDestroy(server);
  server = nullptr;

  close(fd);
}

/* ───────────────────── Streaming response (xHttpCtxWrite) ───────── */

static void stream_handler(xHttpCtx *ctx, void *arg) {
  auto *c = static_cast<HandlerCtx *>(arg);
  c->call_count.fetch_add(1, std::memory_order_release);

  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSetHeader(ctx, "Content-Type", "text/event-stream");
  xHttpCtxSetHeader(ctx, "Cache-Control", "no-cache");

  xHttpCtxWrite(ctx, "data: hello\n\n", 13);
  xHttpCtxWrite(ctx, "data: world\n\n", 13);
xHttpCtxEndStream(ctx);
}

TEST_F(HttpServerTest, StreamingResponse) {
  HandlerCtx ctx;
  route("GET /stream", stream_handler, &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string request = "GET /stream HTTP/1.1\r\nHost: localhost\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));

  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_EQ(ctx.call_count.load(), 1);
  EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_NE(response.find("Content-Type: text/event-stream"), std::string::npos);
  EXPECT_NE(response.find("Connection: close"), std::string::npos);
  EXPECT_NE(response.find("data: hello"), std::string::npos);
  EXPECT_NE(response.find("data: world"), std::string::npos);
  EXPECT_EQ(response.find("Content-Length"), std::string::npos);
}

/* ───────────────────── Streaming auto-end on handler return ─────────── */

static void stream_no_end_handler(xHttpCtx *ctx, void *arg) {
  auto *c = static_cast<HandlerCtx *>(arg);
  c->call_count.fetch_add(1, std::memory_order_release);

  xHttpCtxSetHeader(ctx, "Content-Type", "text/plain");
  xHttpCtxWrite(ctx, "chunk1", 6);
  xHttpCtxWrite(ctx, "chunk2", 6);
}

TEST_F(HttpServerTest, StreamingAutoEnd) {
  HandlerCtx ctx;
  route("GET /stream-auto", stream_no_end_handler, &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string request = "GET /stream-auto HTTP/1.1\r\nHost: localhost\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));

  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_EQ(ctx.call_count.load(), 1);
  EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_NE(response.find("chunk1"), std::string::npos);
  EXPECT_NE(response.find("chunk2"), std::string::npos);
}

/* ───────────────────── Write and Send are mutually exclusive ─────────── */

static void write_then_send_handler(xHttpCtx *ctx, void *arg) {
  xHttpCtxWrite(ctx, "data", 4);
  xErrno err = xHttpCtxSend(ctx, "body", 4);
  auto  *c = static_cast<HandlerCtx *>(arg);
  c->last_body = (err == xErrno_InvalidState) ? "InvalidState" : "Other";
  c->call_count.fetch_add(1, std::memory_order_release);
xHttpCtxEndStream(ctx);
}

TEST_F(HttpServerTest, WriteAndSendMutuallyExclusive) {
  HandlerCtx ctx;
  route("GET /mix", write_then_send_handler, &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string request = "GET /mix HTTP/1.1\r\nHost: localhost\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));

  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_EQ(ctx.call_count.load(), 1);
  EXPECT_EQ(ctx.last_body, "InvalidState");
}

/* ───────────────────── Parameterized route: /users/:id ───────────────── */

static void param_handler(xHttpCtx *ctx, void *arg) {
  auto *c = static_cast<ParamHandlerCtx *>(arg);
  c->call_count.fetch_add(1, std::memory_order_release);

  size_t      len = 0;
  const char *id  = xHttpCtxParam(ctx, "id", &len);
  if (id && len > 0) c->param_id.assign(id, len);

  char body[128];
  int  blen = snprintf(body, sizeof(body), "id=%s", c->param_id.c_str());
  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSetHeader(ctx, "Content-Type", "text/plain");
  xHttpCtxSend(ctx, body, static_cast<size_t>(blen));
}

TEST_F(HttpServerTest, ParamRouteBasic) {
  ParamHandlerCtx ctx;
  route("GET /users/:id", param_handler, &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string request = "GET /users/42 HTTP/1.1\r\nHost: localhost\r\n"
                        "Connection: close\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));
  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_EQ(ctx.call_count.load(), 1);
  EXPECT_EQ(ctx.param_id, "42");
  EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_NE(response.find("id=42"), std::string::npos);
}

TEST_F(HttpServerTest, ParamRouteStringId) {
  ParamHandlerCtx ctx;
  route("GET /users/:id", param_handler, &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string request = "GET /users/alice HTTP/1.1\r\nHost: localhost\r\n"
                        "Connection: close\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));
  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_EQ(ctx.call_count.load(), 1);
  EXPECT_EQ(ctx.param_id, "alice");
  EXPECT_NE(response.find("id=alice"), std::string::npos);
}

/* ───────────────────── Multiple params: /users/:id/posts/:pid ────────── */

static void multi_param_handler(xHttpCtx *ctx, void *arg) {
  auto *c = static_cast<ParamHandlerCtx *>(arg);
  c->call_count.fetch_add(1, std::memory_order_release);

  size_t      id_len = 0, action_len = 0;
  const char *id     = xHttpCtxParam(ctx, "id", &id_len);
  const char *action = xHttpCtxParam(ctx, "action", &action_len);
  if (id && id_len > 0) c->param_id.assign(id, id_len);
  if (action && action_len > 0) c->param_action.assign(action, action_len);

  char body[256];
  int  blen = snprintf(body, sizeof(body), "id=%s,action=%s", c->param_id.c_str(),
                       c->param_action.c_str());
  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSend(ctx, body, static_cast<size_t>(blen));
}

TEST_F(HttpServerTest, ParamRouteMultipleParams) {
  ParamHandlerCtx ctx;
  route("GET /users/:id/:action", multi_param_handler, &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string request = "GET /users/99/edit HTTP/1.1\r\nHost: localhost\r\n"
                        "Connection: close\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));
  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_EQ(ctx.call_count.load(), 1);
  EXPECT_EQ(ctx.param_id, "99");
  EXPECT_EQ(ctx.param_action, "edit");
  EXPECT_NE(response.find("id=99,action=edit"), std::string::npos);
}

/* ───────────────────── Param route: 404 when extra segments ─────────── */

TEST_F(HttpServerTest, ParamRouteExtraSegments404) {
  ParamHandlerCtx ctx;
  route("GET /users/:id", param_handler, &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string request = "GET /users/42/extra HTTP/1.1\r\nHost: localhost\r\n"
                        "Connection: close\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));
  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_EQ(ctx.call_count.load(), 0);
  EXPECT_NE(response.find("404"), std::string::npos);
}

/* ───────────────────── Param route: missing param returns NULL ───────── */

static void missing_param_handler(xHttpCtx *ctx, void *arg) {
  auto *c = static_cast<ParamHandlerCtx *>(arg);
  c->call_count.fetch_add(1, std::memory_order_release);

  size_t      len = 0;
  const char *val = xHttpCtxParam(ctx, "nonexistent", &len);
  c->param_id   = val ? "found" : "null";

  xHttpCtxSend(ctx, "ok", 2);
}

TEST_F(HttpServerTest, ParamRouteNonexistentParam) {
  ParamHandlerCtx ctx;
  route("GET /items/:id", missing_param_handler, &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string request = "GET /items/7 HTTP/1.1\r\nHost: localhost\r\n"
                        "Connection: close\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));
  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_EQ(ctx.call_count.load(), 1);
  EXPECT_EQ(ctx.param_id, "null");
}

/* ───────────────────── Static route takes priority over param route ──── */

TEST_F(HttpServerTest, StaticRoutePriorityOverParam) {
  HandlerCtx      static_ctx;
  ParamHandlerCtx param_ctx;

  {
    xHttpRouteConf conf = {};
    conf.pattern        = "GET /users/me";
    conf.on_done        = echo_handler;
    conf.arg            = &static_ctx;
    ASSERT_EQ(xHttpMuxHandle(mux, &conf), xErrno_Ok);
  }
  {
    xHttpRouteConf conf = {};
    conf.pattern        = "GET /users/:id";
    conf.on_done        = param_handler;
    conf.arg            = &param_ctx;
    ASSERT_EQ(xHttpMuxHandle(mux, &conf), xErrno_Ok);
  }
  listen_and_pump();

  {
    int fd = connect_to(port);
    ASSERT_GE(fd, 0);
    std::string request = "GET /users/me HTTP/1.1\r\nHost: localhost\r\n"
                          "Connection: close\r\n\r\n";
    ASSERT_TRUE(send_str(fd, request));
    run_for(loop, 100);
    std::string response = recv_all(fd);
    close(fd);
    EXPECT_EQ(static_ctx.call_count.load(), 1);
    EXPECT_EQ(param_ctx.call_count.load(), 0);
  }

  {
    int fd = connect_to(port);
    ASSERT_GE(fd, 0);
    std::string request = "GET /users/42 HTTP/1.1\r\nHost: localhost\r\n"
                          "Connection: close\r\n\r\n";
    ASSERT_TRUE(send_str(fd, request));
    run_for(loop, 100);
    std::string response = recv_all(fd);
    close(fd);
    EXPECT_EQ(param_ctx.call_count.load(), 1);
    EXPECT_EQ(param_ctx.param_id, "42");
  }
}

/* ───────────────────── Param route with method filtering ─────────────── */

TEST_F(HttpServerTest, ParamRouteMethodNotAllowed) {
  ParamHandlerCtx ctx;
  route("GET /items/:id", param_handler, &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string request = "POST /items/5 HTTP/1.1\r\nHost: localhost\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));
  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_EQ(ctx.call_count.load(), 0);
  EXPECT_NE(response.find("405"), std::string::npos);
}

/* ───────────────────── Deferred response ─────────────────────────────── */

struct YieldCtx {
  std::atomic<int>  call_count{0};
  xHttpCtx         *stored_ctx{nullptr};
  std::atomic<bool> send_done{false};
  std::string       response_body;
};

static void yield_handler(xHttpCtx *ctx, void *arg) {
  YieldCtx *c = reinterpret_cast<YieldCtx *>(arg);
  c->call_count.fetch_add(1);

  xHttpCtxSetStatus(ctx, 200);
  xHttpCtxSetHeader(ctx, "Content-Type", "text/plain");

  /* Don't send — store ctx for later. Handler return does nothing. */
  c->stored_ctx = ctx;
}

static void deferred_send(YieldCtx *c) {
  if (c->stored_ctx) {
    xHttpCtxSend(c->stored_ctx, c->response_body.c_str(), c->response_body.size());
    c->stored_ctx = nullptr;
    c->send_done.store(true);
  }
}

TEST_F(HttpServerTest, DeferredResponseSend) {
  YieldCtx ctx;
  ctx.response_body = "hello from deferred";

  route("GET /yielded", yield_handler, &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);
  std::string request = "GET /yielded HTTP/1.1\r\nHost: localhost\r\n"
                        "Connection: close\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));

  run_for(loop, 50);
  EXPECT_EQ(ctx.call_count.load(), 1);
  EXPECT_NE(ctx.stored_ctx, nullptr);
  EXPECT_FALSE(ctx.send_done.load());

  {
    char buf[64];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
    EXPECT_LT(n, 0) << "Expected no data yet, got: " << std::string(buf, n > 0 ? n : 0);
  }

  deferred_send(&ctx);
  EXPECT_TRUE(ctx.send_done.load());

  run_for(loop, 50);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_NE(response.find("200"), std::string::npos);
  EXPECT_NE(response.find("hello from deferred"), std::string::npos);
  EXPECT_NE(response.find("Connection: close"), std::string::npos);
}

TEST_F(HttpServerTest, DeferredResponseMultipleConcurrent) {
  YieldCtx ctx_a, ctx_b;
  ctx_a.response_body = "response A";
  ctx_b.response_body = "response B";

  route("GET /a", yield_handler, &ctx_a);
  route("GET /b", yield_handler, &ctx_b);
  listen_and_pump();

  int fd_a = connect_to(port);
  ASSERT_GE(fd_a, 0);
  int fd_b = connect_to(port);
  ASSERT_GE(fd_b, 0);

  std::string req_a = "GET /a HTTP/1.1\r\nHost: localhost\r\n"
                      "Connection: close\r\n\r\n";
  std::string req_b = "GET /b HTTP/1.1\r\nHost: localhost\r\n"
                      "Connection: close\r\n\r\n";
  ASSERT_TRUE(send_str(fd_a, req_a));
  ASSERT_TRUE(send_str(fd_b, req_b));

  run_for(loop, 100);
  EXPECT_EQ(ctx_a.call_count.load(), 1);
  EXPECT_EQ(ctx_b.call_count.load(), 1);
  EXPECT_NE(ctx_a.stored_ctx, nullptr);
  EXPECT_NE(ctx_b.stored_ctx, nullptr);

  deferred_send(&ctx_a);
  deferred_send(&ctx_b);

  run_for(loop, 100);

  std::string resp_a = recv_all(fd_a);
  std::string resp_b = recv_all(fd_b);
  close(fd_a);
  close(fd_b);

  EXPECT_NE(resp_a.find("200"), std::string::npos);
  EXPECT_NE(resp_a.find("response A"), std::string::npos);
  EXPECT_NE(resp_b.find("200"), std::string::npos);
  EXPECT_NE(resp_b.find("response B"), std::string::npos);
}

TEST_F(HttpServerTest, YieldedResponseWithoutResumeNoResponse) {
  YieldCtx ctx;
  ctx.response_body = "never sent";

  route("GET /leak", yield_handler, &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);
  std::string request = "GET /leak HTTP/1.1\r\nHost: localhost\r\n"
                        "Connection: close\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));

  run_for(loop, 100);
  EXPECT_EQ(ctx.call_count.load(), 1);

  close(fd);
}
