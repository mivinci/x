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

  xHttpServer s = xHttpServerCreate();
  ASSERT_NE(s, nullptr);

  xHttpServerDestroy(s);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(HttpServerLifecycle, CreateWithoutEnteringLoopSucceeds) {
  /* With the new API, xHttpServerCreate() uses xEventLoopCurrent(),
   * which falls back to the global loop when no loop is entered.
   * Creation should succeed (no crash). */
  xHttpServer s = xHttpServerCreate();
  (void)s;
}

TEST(HttpServerLifecycle, DestroyNullIsNoop) {
  xHttpServerDestroy(nullptr); /* should not crash */
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

TEST_F(HttpServerTest, SetIdleTimeout) {
  EXPECT_EQ(xHttpServerSetIdleTimeout(server, 5000), xErrno_Ok);
  EXPECT_EQ(xHttpServerSetIdleTimeout(server, -1), xErrno_InvalidArg);
  EXPECT_EQ(xHttpServerSetIdleTimeout(server, 0), xErrno_InvalidArg);
  EXPECT_EQ(xHttpServerSetIdleTimeout(nullptr, 5000), xErrno_InvalidArg);
}

TEST_F(HttpServerTest, SetMaxHeaderSize) {
  EXPECT_EQ(xHttpServerSetMaxHeaderSize(server, 4096), xErrno_Ok);
  EXPECT_EQ(xHttpServerSetMaxHeaderSize(server, 0), xErrno_InvalidArg);
  EXPECT_EQ(xHttpServerSetMaxHeaderSize(nullptr, 4096), xErrno_InvalidArg);
}

TEST_F(HttpServerTest, SetMaxBodySize) {
  EXPECT_EQ(xHttpServerSetMaxBodySize(server, 2048), xErrno_Ok);
  EXPECT_EQ(xHttpServerSetMaxBodySize(server, 0), xErrno_InvalidArg);
  EXPECT_EQ(xHttpServerSetMaxBodySize(nullptr, 2048), xErrno_InvalidArg);
}

/* ───────────────────── Route registration tests ───────────────────── */

static void dummy_handler(xHttpResponseWriter, const xHttpRequest *, void *) {}

TEST_F(HttpServerTest, RouteRegistration) {
  EXPECT_EQ(xHttpServerRoute(server, "GET /test", dummy_handler, nullptr), xErrno_Ok);
  EXPECT_EQ(xHttpServerRoute(server, "/any", dummy_handler, nullptr), xErrno_Ok);
}

TEST_F(HttpServerTest, RouteNullPathReturnsError) {
  EXPECT_EQ(xHttpServerRoute(server, nullptr, dummy_handler, nullptr), xErrno_InvalidArg);
}

TEST_F(HttpServerTest, RouteNullHandlerReturnsError) {
  EXPECT_EQ(xHttpServerRoute(server, "GET /test", nullptr, nullptr), xErrno_InvalidArg);
}

TEST_F(HttpServerTest, RouteNullServerReturnsError) {
  EXPECT_EQ(xHttpServerRoute(nullptr, "GET /test", dummy_handler, nullptr), xErrno_InvalidArg);
}

/* ───────────────────── Basic GET request ───────────────────── */

static void echo_handler(xHttpResponseWriter writer, const xHttpRequest *req, void *arg) {
  auto *ctx        = static_cast<HandlerCtx *>(arg);
  ctx->last_method = req->method;
  ctx->last_url    = req->url;
  if (req->body && req->body_len > 0) ctx->last_body.assign(req->body, req->body_len);
  ctx->call_count.fetch_add(1, std::memory_order_release);

  const char *body = "Hello, World!";
  xHttpResponseSetStatus(writer, 200);
  xHttpResponseSetHeader(writer, "Content-Type", "text/plain");
  xHttpResponseSend(writer, body, strlen(body));
}

TEST_F(HttpServerTest, BasicGetRequest) {
  HandlerCtx ctx;
  xHttpServerRoute(server, "GET /hello", echo_handler, &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0) << "Failed to connect";

  std::string request = "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));

  pump_loop(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_EQ(ctx.call_count.load(), 1);
  EXPECT_EQ(ctx.last_method, "GET");
  EXPECT_EQ(ctx.last_url, "/hello");

  /* Verify response contains expected parts */
  EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_NE(response.find("Content-Type: text/plain"), std::string::npos);
  EXPECT_NE(response.find("Hello, World!"), std::string::npos);
}

/* ───────────────────── POST request with body ───────────────────── */

static void body_echo_handler(xHttpResponseWriter writer, const xHttpRequest *req, void *arg) {
  auto *ctx        = static_cast<HandlerCtx *>(arg);
  ctx->last_method = req->method;
  ctx->last_url    = req->url;
  if (req->body && req->body_len > 0) ctx->last_body.assign(req->body, req->body_len);
  ctx->call_count.fetch_add(1, std::memory_order_release);

  /* Echo the body back */
  xHttpResponseSetStatus(writer, 200);
  xHttpResponseSend(writer, req->body, req->body_len);
}

TEST_F(HttpServerTest, PostRequestWithBody) {
  HandlerCtx ctx;
  xHttpServerRoute(server, "POST /echo", body_echo_handler, &ctx);
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

  pump_loop(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_EQ(ctx.call_count.load(), 1);
  EXPECT_EQ(ctx.last_method, "POST");
  EXPECT_EQ(ctx.last_body, body);
  EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_NE(response.find(body), std::string::npos);
}

/* ───────────────────── 404 Not Found ───────────────────── */

TEST_F(HttpServerTest, NotFoundResponse) {
  xHttpServerRoute(server, "GET /exists", dummy_handler, nullptr);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string request = "GET /nonexistent HTTP/1.1\r\nHost: localhost\r\n"
                        "Connection: close\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));

  pump_loop(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_NE(response.find("HTTP/1.1 404 Not Found"), std::string::npos);
}

/* ───────────────────── 405 Method Not Allowed ───────────────────── */

TEST_F(HttpServerTest, MethodNotAllowedResponse) {
  xHttpServerRoute(server, "GET /only-get", dummy_handler, nullptr);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string request = "POST /only-get HTTP/1.1\r\nHost: localhost\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));

  pump_loop(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_NE(response.find("HTTP/1.1 405 Method Not Allowed"), std::string::npos);
}

/* ───────────────────── Keep-alive connection reuse ───────────────────── */

TEST_F(HttpServerTest, KeepAliveConnectionReuse) {
  HandlerCtx ctx;
  xHttpServerSetIdleTimeout(server, 60000);
  xHttpServerRoute(server, "GET /ka", echo_handler, &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  /* Send first request */
  std::string req1 = "GET /ka HTTP/1.1\r\nHost: localhost\r\n\r\n";
  ASSERT_TRUE(send_str(fd, req1));
  pump_loop(loop, 100);

  std::string resp1 = recv_all(fd, 1000);
  EXPECT_NE(resp1.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_EQ(ctx.call_count.load(), 1);

  /* Send second request on the same connection */
  std::string req2 = "GET /ka HTTP/1.1\r\nHost: localhost\r\n\r\n";
  ASSERT_TRUE(send_str(fd, req2));
  pump_loop(loop, 100);

  std::string resp2 = recv_all(fd, 1000);
  EXPECT_NE(resp2.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_EQ(ctx.call_count.load(), 2);

  close(fd);
}

/* ───────────────────── Default 200 OK when handler doesn't respond ──── */

TEST_F(HttpServerTest, DefaultResponseWhenHandlerDoesNotSend) {
  auto noop_handler = [](xHttpResponseWriter, const xHttpRequest *, void *) {
    /* Handler does nothing — server should auto-send 200 OK */
  };

  xHttpServerRoute(server, "GET /noop", (xHttpHandlerFunc)noop_handler, nullptr);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string request = "GET /noop HTTP/1.1\r\nHost: localhost\r\n"
                        "Connection: close\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));

  pump_loop(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
}

/* ───────────────────── Bad request (parse error) ───────────────────── */

TEST_F(HttpServerTest, BadRequestOnParseError) {
  xHttpServerRoute(server, "GET /test", dummy_handler, nullptr);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  /* Send malformed HTTP */
  std::string request = "INVALID GARBAGE\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));

  pump_loop(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_NE(response.find("400 Bad Request"), std::string::npos);
}

/* ───────────────────── Header too large → 431 ───────────────────── */

TEST_F(HttpServerTest, HeaderTooLargeReturns431) {
  xHttpServerSetMaxHeaderSize(server, 128);
  xHttpServerRoute(server, "GET /test", dummy_handler, nullptr);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  /* Build a request with a very large header */
  std::string large_header(256, 'X');
  std::string request = "GET /test HTTP/1.1\r\n"
                        "Host: localhost\r\n"
                        "X-Large: " +
                        large_header +
                        "\r\n"
                        "Connection: close\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));

  pump_loop(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_NE(response.find("431"), std::string::npos);
}

/* ───────────────────── Body too large → 413 ───────────────────── */

TEST_F(HttpServerTest, BodyTooLargeReturns413) {
  xHttpServerSetMaxBodySize(server, 32);
  xHttpServerRoute(server, "POST /test", dummy_handler, nullptr);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string body(64, 'A');
  std::string request = "POST /test HTTP/1.1\r\n"
                        "Host: localhost\r\n"
                        "Content-Length: " +
                        std::to_string(body.size()) +
                        "\r\n"
                        "Connection: close\r\n\r\n" +
                        body;
  ASSERT_TRUE(send_str(fd, request));

  pump_loop(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_NE(response.find("413"), std::string::npos);
}

/* ───────────────────── Client disconnect (half-close) ───────────────────── */

TEST_F(HttpServerTest, ClientDisconnectDoesNotCrash) {
  xHttpServerRoute(server, "GET /test", dummy_handler, nullptr);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  /* Send partial request and close immediately */
  std::string partial = "GET /test HTTP/1.1\r\nHost: lo";
  send_str(fd, partial);
  close(fd);

  /* Pump to process the disconnect — should not crash */
  pump_loop(loop, 100);
}

/* ───────────────────── NULL method matches all methods ─────────────────────
 */

TEST_F(HttpServerTest, NullMethodMatchesAll) {
  HandlerCtx ctx;
  xHttpServerRoute(server, "/any", echo_handler, &ctx);
  listen_and_pump();

  /* Test with GET */
  {
    int fd = connect_to(port);
    ASSERT_GE(fd, 0);
    std::string request = "GET /any HTTP/1.1\r\nHost: localhost\r\n"
                          "Connection: close\r\n\r\n";
    ASSERT_TRUE(send_str(fd, request));
    pump_loop(loop, 100);
    std::string response = recv_all(fd);
    close(fd);
    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
  }

  /* Test with POST */
  {
    int fd = connect_to(port);
    ASSERT_GE(fd, 0);
    std::string request = "POST /any HTTP/1.1\r\nHost: localhost\r\n"
                          "Content-Length: 0\r\n"
                          "Connection: close\r\n\r\n";
    ASSERT_TRUE(send_str(fd, request));
    pump_loop(loop, 100);
    std::string response = recv_all(fd);
    close(fd);
    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
  }

  EXPECT_EQ(ctx.call_count.load(), 2);
}

/* ───────────────────── Destroy with active connections ─────────────────────
 */

TEST_F(HttpServerTest, DestroyWithActiveConnections) {
  xHttpServerRoute(server, "GET /test", dummy_handler, nullptr);
  listen_and_pump();

  /* Open a connection but don't send anything */
  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  pump_loop(loop, 50);

  /* Destroy server while connection is active — should not crash */
  xHttpServerDestroy(server);
  server = nullptr; /* prevent double-destroy in TearDown */

  close(fd);
}

/* ───────────────────── Streaming response (xHttpResponseWrite) ───────── */

static void stream_handler(xHttpResponseWriter writer, const xHttpRequest *req, void *arg) {
  (void)req;
  auto *ctx = static_cast<HandlerCtx *>(arg);
  ctx->call_count.fetch_add(1, std::memory_order_release);

  xHttpResponseSetStatus(writer, 200);
  xHttpResponseSetHeader(writer, "Content-Type", "text/event-stream");
  xHttpResponseSetHeader(writer, "Cache-Control", "no-cache");

  xHttpResponseWrite(writer, "data: hello\n\n", 13);
  xHttpResponseWrite(writer, "data: world\n\n", 13);
  xHttpResponseEnd(writer);
}

TEST_F(HttpServerTest, StreamingResponse) {
  HandlerCtx ctx;
  xHttpServerRoute(server, "GET /stream", stream_handler, &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string request = "GET /stream HTTP/1.1\r\nHost: localhost\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));

  pump_loop(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_EQ(ctx.call_count.load(), 1);
  EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_NE(response.find("Content-Type: text/event-stream"), std::string::npos);
  EXPECT_NE(response.find("Connection: close"), std::string::npos);
  EXPECT_NE(response.find("data: hello"), std::string::npos);
  EXPECT_NE(response.find("data: world"), std::string::npos);
  /* Streaming responses should NOT have Content-Length */
  EXPECT_EQ(response.find("Content-Length"), std::string::npos);
}

/* ───────────────────── Streaming auto-end on handler return ─────────── */

static void stream_no_end_handler(xHttpResponseWriter writer, const xHttpRequest *req, void *arg) {
  (void)req;
  auto *ctx = static_cast<HandlerCtx *>(arg);
  ctx->call_count.fetch_add(1, std::memory_order_release);

  xHttpResponseSetHeader(writer, "Content-Type", "text/plain");
  xHttpResponseWrite(writer, "chunk1", 6);
  xHttpResponseWrite(writer, "chunk2", 6);
  /* No xHttpResponseEnd() — should be auto-ended */
}

TEST_F(HttpServerTest, StreamingAutoEnd) {
  HandlerCtx ctx;
  xHttpServerRoute(server, "GET /stream-auto", stream_no_end_handler, &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string request = "GET /stream-auto HTTP/1.1\r\nHost: localhost\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));

  pump_loop(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_EQ(ctx.call_count.load(), 1);
  EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_NE(response.find("chunk1"), std::string::npos);
  EXPECT_NE(response.find("chunk2"), std::string::npos);
}

/* ───────────────────── Write and Send are mutually exclusive ─────────── */

static void write_then_send_handler(xHttpResponseWriter writer, const xHttpRequest *req,
                                    void *arg) {
  (void)req;
  (void)arg;
  xHttpResponseWrite(writer, "data", 4);
  /* Send after Write should fail */
  xErrno err = xHttpResponseSend(writer, "body", 4);
  auto  *ctx = static_cast<HandlerCtx *>(arg);
  /* Store the error in last_body for verification */
  ctx->last_body = (err == xErrno_InvalidState) ? "InvalidState" : "Other";
  ctx->call_count.fetch_add(1, std::memory_order_release);
}

TEST_F(HttpServerTest, WriteAndSendMutuallyExclusive) {
  HandlerCtx ctx;
  xHttpServerRoute(server, "GET /mix", write_then_send_handler, &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string request = "GET /mix HTTP/1.1\r\nHost: localhost\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));

  pump_loop(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_EQ(ctx.call_count.load(), 1);
  EXPECT_EQ(ctx.last_body, "InvalidState");
}

/* ───────────────────── Parameterized route: /users/:id ───────────────── */

static void param_handler(xHttpResponseWriter writer, const xHttpRequest *req, void *arg) {
  auto *ctx = static_cast<ParamHandlerCtx *>(arg);
  ctx->call_count.fetch_add(1, std::memory_order_release);

  size_t      len = 0;
  const char *id  = xHttpRequestParam(req, "id", &len);
  if (id && len > 0) ctx->param_id.assign(id, len);

  char body[128];
  int  blen = snprintf(body, sizeof(body), "id=%s", ctx->param_id.c_str());
  xHttpResponseSetStatus(writer, 200);
  xHttpResponseSetHeader(writer, "Content-Type", "text/plain");
  xHttpResponseSend(writer, body, (size_t)blen);
}

TEST_F(HttpServerTest, ParamRouteBasic) {
  ParamHandlerCtx ctx;
  xHttpServerRoute(server, "GET /users/:id", param_handler, &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string request = "GET /users/42 HTTP/1.1\r\nHost: localhost\r\n"
                        "Connection: close\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));
  pump_loop(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_EQ(ctx.call_count.load(), 1);
  EXPECT_EQ(ctx.param_id, "42");
  EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_NE(response.find("id=42"), std::string::npos);
}

TEST_F(HttpServerTest, ParamRouteStringId) {
  ParamHandlerCtx ctx;
  xHttpServerRoute(server, "GET /users/:id", param_handler, &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string request = "GET /users/alice HTTP/1.1\r\nHost: localhost\r\n"
                        "Connection: close\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));
  pump_loop(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_EQ(ctx.call_count.load(), 1);
  EXPECT_EQ(ctx.param_id, "alice");
  EXPECT_NE(response.find("id=alice"), std::string::npos);
}

/* ───────────────────── Multiple params: /users/:id/posts/:pid ────────── */

static void multi_param_handler(xHttpResponseWriter writer, const xHttpRequest *req, void *arg) {
  auto *ctx = static_cast<ParamHandlerCtx *>(arg);
  ctx->call_count.fetch_add(1, std::memory_order_release);

  size_t      id_len = 0, action_len = 0;
  const char *id     = xHttpRequestParam(req, "id", &id_len);
  const char *action = xHttpRequestParam(req, "action", &action_len);
  if (id && id_len > 0) ctx->param_id.assign(id, id_len);
  if (action && action_len > 0) ctx->param_action.assign(action, action_len);

  char body[256];
  int  blen = snprintf(body, sizeof(body), "id=%s,action=%s", ctx->param_id.c_str(),
                       ctx->param_action.c_str());
  xHttpResponseSetStatus(writer, 200);
  xHttpResponseSend(writer, body, (size_t)blen);
}

TEST_F(HttpServerTest, ParamRouteMultipleParams) {
  ParamHandlerCtx ctx;
  xHttpServerRoute(server, "GET /users/:id/:action", multi_param_handler, &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string request = "GET /users/99/edit HTTP/1.1\r\nHost: localhost\r\n"
                        "Connection: close\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));
  pump_loop(loop, 100);

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
  xHttpServerRoute(server, "GET /users/:id", param_handler, &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  /* /users/42/extra should NOT match /users/:id */
  std::string request = "GET /users/42/extra HTTP/1.1\r\nHost: localhost\r\n"
                        "Connection: close\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));
  pump_loop(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_EQ(ctx.call_count.load(), 0);
  EXPECT_NE(response.find("404"), std::string::npos);
}

/* ───────────────────── Param route: missing param returns NULL ───────── */

static void missing_param_handler(xHttpResponseWriter writer, const xHttpRequest *req, void *arg) {
  auto *ctx = static_cast<ParamHandlerCtx *>(arg);
  ctx->call_count.fetch_add(1, std::memory_order_release);

  size_t      len = 0;
  const char *val = xHttpRequestParam(req, "nonexistent", &len);
  ctx->param_id   = val ? "found" : "null";

  xHttpResponseSend(writer, "ok", 2);
}

TEST_F(HttpServerTest, ParamRouteNonexistentParam) {
  ParamHandlerCtx ctx;
  xHttpServerRoute(server, "GET /items/:id", missing_param_handler, &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string request = "GET /items/7 HTTP/1.1\r\nHost: localhost\r\n"
                        "Connection: close\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));
  pump_loop(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_EQ(ctx.call_count.load(), 1);
  EXPECT_EQ(ctx.param_id, "null");
}

/* ───────────────────── Static route takes priority over param route ──── */

TEST_F(HttpServerTest, StaticRoutePriorityOverParam) {
  HandlerCtx      static_ctx;
  ParamHandlerCtx param_ctx;

  /* Register static route first (first match wins) */
  xHttpServerRoute(server, "GET /users/me", echo_handler, &static_ctx);
  xHttpServerRoute(server, "GET /users/:id", param_handler, &param_ctx);
  listen_and_pump();

  /* /users/me should match the static route */
  {
    int fd = connect_to(port);
    ASSERT_GE(fd, 0);
    std::string request = "GET /users/me HTTP/1.1\r\nHost: localhost\r\n"
                          "Connection: close\r\n\r\n";
    ASSERT_TRUE(send_str(fd, request));
    pump_loop(loop, 100);
    std::string response = recv_all(fd);
    close(fd);
    EXPECT_EQ(static_ctx.call_count.load(), 1);
    EXPECT_EQ(param_ctx.call_count.load(), 0);
  }

  /* /users/42 should match the param route */
  {
    int fd = connect_to(port);
    ASSERT_GE(fd, 0);
    std::string request = "GET /users/42 HTTP/1.1\r\nHost: localhost\r\n"
                          "Connection: close\r\n\r\n";
    ASSERT_TRUE(send_str(fd, request));
    pump_loop(loop, 100);
    std::string response = recv_all(fd);
    close(fd);
    EXPECT_EQ(param_ctx.call_count.load(), 1);
    EXPECT_EQ(param_ctx.param_id, "42");
  }
}

/* ───────────────────── Param route with method filtering ─────────────── */

TEST_F(HttpServerTest, ParamRouteMethodNotAllowed) {
  ParamHandlerCtx ctx;
  xHttpServerRoute(server, "GET /items/:id", param_handler, &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  /* POST to a GET-only param route should be 405 */
  std::string request = "POST /items/5 HTTP/1.1\r\nHost: localhost\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));
  pump_loop(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_EQ(ctx.call_count.load(), 0);
  EXPECT_NE(response.find("405"), std::string::npos);
}

/* ───────────────────── Deferred response ──────────────────────────────── */

struct DeferCtx {
  std::atomic<int>     call_count{0};
  xHttpResponseWriter  stored_writer{nullptr};
  std::atomic<bool>    send_done{false};
  std::string          response_body;
};

static void defer_handler(xHttpResponseWriter writer, const xHttpRequest *req, void *arg) {
  (void)req;
  DeferCtx *ctx = (DeferCtx *)arg;
  ctx->call_count.fetch_add(1);

  xHttpResponseSetStatus(writer, 200);
  xHttpResponseSetHeader(writer, "Content-Type", "text/plain");

  /* Defer: keep connection alive, send response later */
  xHttpResponseDefer(writer);
  ctx->stored_writer = writer;
}

/* Simulate: call xHttpResponseSend + xHttpConnResume from a channel-like callback */
static void deferred_send_and_resume(DeferCtx *ctx) {
  if (ctx->stored_writer) {
    xHttpResponseSend(ctx->stored_writer, ctx->response_body.c_str(),
                      ctx->response_body.size());
    xHttpConnResume(ctx->stored_writer);
    ctx->stored_writer = nullptr;
    ctx->send_done.store(true);
  }
}

TEST_F(HttpServerTest, DeferredResponseSendAndResume) {
  DeferCtx ctx;
  ctx.response_body = "hello from deferred";

  /* Avoid read timeout firing while response is deferred */
  xHttpServerSetIdleTimeout(server, 60000);

  xHttpServerRoute(server, "GET /deferred", defer_handler, &ctx);
  listen_and_pump();

  /* Send request */
  int fd = connect_to(port);
  ASSERT_GE(fd, 0);
  std::string request = "GET /deferred HTTP/1.1\r\nHost: localhost\r\n"
                        "Connection: close\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));

  /* Pump — handler should have been called and deferred */
  run_for(loop, 50);
  EXPECT_EQ(ctx.call_count.load(), 1);
  EXPECT_NE(ctx.stored_writer, nullptr);
  EXPECT_FALSE(ctx.send_done.load());

  /* Verify no response has been sent yet (connection still open) */
  {
    char buf[64];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
    EXPECT_LT(n, 0) << "Expected no data yet, got: " << std::string(buf, n > 0 ? n : 0);
  }

  /* Now send the response via the deferred path */
  deferred_send_and_resume(&ctx);
  EXPECT_TRUE(ctx.send_done.load());

  /* Pump to flush */
  run_for(loop, 50);

  /* Read the response */
  std::string response = recv_all(fd);
  close(fd);

  EXPECT_NE(response.find("200"), std::string::npos);
  EXPECT_NE(response.find("hello from deferred"), std::string::npos);
  EXPECT_NE(response.find("Connection: close"), std::string::npos);
}

TEST_F(HttpServerTest, DeferredResponseMultipleConcurrent) {
  DeferCtx ctx_a, ctx_b;
  ctx_a.response_body = "response A";
  ctx_b.response_body = "response B";

  xHttpServerSetIdleTimeout(server, 60000);

  xHttpServerRoute(server, "GET /a", defer_handler, &ctx_a);
  xHttpServerRoute(server, "GET /b", defer_handler, &ctx_b);
  listen_and_pump();

  /* Send two concurrent requests */
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

  /* Both handlers should fire and defer */
  pump_loop(loop, 100);
  EXPECT_EQ(ctx_a.call_count.load(), 1);
  EXPECT_EQ(ctx_b.call_count.load(), 1);
  EXPECT_NE(ctx_a.stored_writer, nullptr);
  EXPECT_NE(ctx_b.stored_writer, nullptr);

  /* Send both responses (simulating async download completion) */
  deferred_send_and_resume(&ctx_a);
  deferred_send_and_resume(&ctx_b);

  pump_loop(loop, 100);

  std::string resp_a = recv_all(fd_a);
  std::string resp_b = recv_all(fd_b);
  close(fd_a);
  close(fd_b);

  EXPECT_NE(resp_a.find("200"), std::string::npos);
  EXPECT_NE(resp_a.find("response A"), std::string::npos);
  EXPECT_NE(resp_b.find("200"), std::string::npos);
  EXPECT_NE(resp_b.find("response B"), std::string::npos);
}

TEST_F(HttpServerTest, DeferredResponseWithoutResumeNoResponse) {
  DeferCtx ctx;
  ctx.response_body = "never sent";

  xHttpServerRoute(server, "GET /leak", defer_handler, &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);
  std::string request = "GET /leak HTTP/1.1\r\nHost: localhost\r\n"
                        "Connection: close\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));

  pump_loop(loop, 100);
  EXPECT_EQ(ctx.call_count.load(), 1);

  /* Never call send_and_resume — connection should stay alive
     until idle timeout (no crash, no memory corruption) */
  close(fd);
}
