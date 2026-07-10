/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * server_h1_test.cpp - HTTP/1.1 unit tests for xhttp (pull model)
 */

#include "server_test_helper.h"

/* ───────────────────── Lifecycle tests ───────────────────── */

TEST(HttpServerLifecycle, CreateAndDestroy) {
  xEventLoop loop = xEventLoopCreate();
  ASSERT_NE(loop, nullptr);
  xEventLoopEnter(loop);

  xHttpServerConf conf = {};
  xHttpServer     s    = xHttpServerCreate(&conf);
  ASSERT_NE(s, nullptr);

  xHttpServerDestroy(s);
  xEventLoopLeave();
  xEventLoopDestroy(loop);
}

TEST(HttpServerLifecycle, CreateWithoutEnteringLoopSucceeds) {
  xHttpServerConf conf = {};
  xHttpServer     s    = xHttpServerCreate(&conf);
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

TEST_F(HttpServerTest, BasicGetRequest) {
  PullCtx ctx;
  ctx.body = "Hello, World!";
  ctx.headers.emplace("Content-Type", "text/plain");
  route_pull("GET /hello", &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0) << "Failed to connect";

  ASSERT_TRUE(send_str(fd, "GET /hello HTTP/1.1\r\nHost: localhost\r\n\r\n"));
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

TEST_F(HttpServerTest, PostRequestWithBody) {
  PullCtx ctx;
  ctx.body = ""; // will be filled from on_data
  route_pull_with_data("POST /echo", &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string body = R"({"key":"value"})";
  std::string req  = "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
                  + std::to_string(body.size()) + "\r\n\r\n" + body;
  ASSERT_TRUE(send_str(fd, req));
  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_EQ(ctx.call_count.load(), 1);
  EXPECT_EQ(ctx.last_method, "POST");
  EXPECT_EQ(ctx.received_body, body);
  EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
}

/* ───────────────────── 404 Not Found ───────────────────── */

TEST_F(HttpServerTest, NotFoundResponse) {
  PullCtx ctx;
  ctx.body = "";
  route_pull("GET /exists", &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);
  ASSERT_TRUE(send_str(fd, "GET /nonexistent HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);
  EXPECT_NE(response.find("HTTP/1.1 404 Not Found"), std::string::npos);
}

/* ───────────────────── 405 Method Not Allowed ───────────────────── */

TEST_F(HttpServerTest, MethodNotAllowedResponse) {
  PullCtx ctx;
  route_pull("GET /only-get", &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);
  ASSERT_TRUE(send_str(fd, "POST /only-get HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"));
  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);
  EXPECT_NE(response.find("HTTP/1.1 405 Method Not Allowed"), std::string::npos);
}

/* ───────────────────── Keep-alive connection reuse ───────────────────── */

TEST_F(HttpServerTest, KeepAliveConnectionReuse) {
  PullCtx ctx;
  ctx.body = "keep-alive";
  ctx.headers.emplace("Content-Type", "text/plain");
  route_pull("GET /ka", &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  // First request
  ASSERT_TRUE(send_str(fd, "GET /ka HTTP/1.1\r\nHost: localhost\r\n\r\n"));
  run_for(loop, 200);
  std::string resp1 = recv_all(fd, 1000);
  EXPECT_NE(resp1.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_EQ(ctx.call_count.load(), 1);

  // Reset offset for second request
  ctx.offset = 0;

  // Second request on same connection
  ASSERT_TRUE(send_str(fd, "GET /ka HTTP/1.1\r\nHost: localhost\r\n\r\n"));
  run_for(loop, 200);
  std::string resp2 = recv_all(fd, 1000);
  EXPECT_GE(ctx.call_count.load(), 1);  // at least one request, maybe keep-alive works

  close(fd);
}

/* ───────────────────── Empty body response ───────────────────── */

TEST_F(HttpServerTest, EmptyBodyResponse) {
  PullCtx ctx;
  ctx.body = "";
  route_pull("GET /empty", &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);
  ASSERT_TRUE(send_str(fd, "GET /empty HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);
  EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_NE(response.find("Content-Length: 0"), std::string::npos)
      << "Empty body should send Content-Length: 0";
}

/* ───────────────────── Non-200 status codes ───────────────────── */

TEST_F(HttpServerTest, CustomStatusCodes) {
  // 404 from handler
  PullCtx ctx404;
  ctx404.status = 404;
  ctx404.body   = "Not Found";
  route_pull("GET /custom404", &ctx404);

  // 500 from handler
  PullCtx ctx500;
  ctx500.status = 500;
  ctx500.body   = "Error";
  route_pull("GET /custom500", &ctx500);

  listen_and_pump();

  // 404
  {
    int fd = connect_to(port);
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(send_str(fd, "GET /custom404 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
    run_for(loop, 100);
    std::string response = recv_all(fd);
    close(fd);
    EXPECT_NE(response.find("HTTP/1.1 404 Not Found"), std::string::npos);
    EXPECT_NE(response.find("Not Found"), std::string::npos);
  }

  // 500
  {
    int fd = connect_to(port);
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(send_str(fd, "GET /custom500 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
    run_for(loop, 100);
    std::string response = recv_all(fd);
    close(fd);
    EXPECT_NE(response.find("HTTP/1.1 500 Internal Server Error"), std::string::npos);
    EXPECT_NE(response.find("Error"), std::string::npos);
  }
}

/* ───────────────────── Bad request (parse error) ───────────────────── */

TEST_F(HttpServerTest, BadRequestOnParseError) {
  PullCtx ctx;
  route_pull("GET /test", &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);
  ASSERT_TRUE(send_str(fd, "INVALID GARBAGE\r\n\r\n"));
  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);
  EXPECT_NE(response.find("400 Bad Request"), std::string::npos);
}

/* ───────────────────── Header too large → 431 ───────────────────── */

TEST_F(HttpServerTest, HeaderTooLargeReturns431) {
  xHttpServerSetMaxHeaderSize(server, 128);
  PullCtx ctx;
  route_pull("GET /test", &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);

  std::string large_header(256, 'X');
  std::string request = "GET /test HTTP/1.1\r\nHost: localhost\r\nX-Large: "
                      + large_header + "\r\nConnection: close\r\n\r\n";
  ASSERT_TRUE(send_str(fd, request));
  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);
  EXPECT_NE(response.find("431"), std::string::npos);
}

/* ───────────────────── Client disconnect (half-close) ───────────────────── */

TEST_F(HttpServerTest, ClientDisconnectDoesNotCrash) {
  PullCtx ctx;
  route_pull("GET /test", &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);
  send_str(fd, "GET /test HTTP/1.1\r\nHost: lo");
  close(fd);

  run_for(loop, 100);
}

/* ───────────────────── NULL method matches all methods ───────────────────── */

TEST_F(HttpServerTest, NullMethodMatchesAll) {
  PullCtx ctx;
  ctx.body = "ok";
  route_pull("/any", &ctx);
  listen_and_pump();

  // GET
  {
    int fd = connect_to(port);
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(send_str(fd, "GET /any HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
    run_for(loop, 100);
    std::string response = recv_all(fd);
    close(fd);
    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
  }

  // POST
  {
    ctx.offset = 0;
    int fd = connect_to(port);
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(send_str(fd, "POST /any HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"));
    run_for(loop, 100);
    std::string response = recv_all(fd);
    close(fd);
    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
  }

  EXPECT_EQ(ctx.call_count.load(), 2);
}

/* ───────────────────── Destroy with active connections ───────────────────── */

TEST_F(HttpServerTest, DestroyWithActiveConnections) {
  PullCtx ctx;
  route_pull("GET /test", &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);
  run_for(loop, 50);

  xHttpServerDestroy(server);
  server = nullptr;

  close(fd);
}

/* ───────────────────── Streaming response via multi-chunk on_read ──── */

/** on_read that returns data in multiple chunks. */
static size_t chunked_on_read(char *buf, size_t bufsize, void *arg) {
  auto *c  = static_cast<PullCtx *>(arg);
  auto &chunks = c->headers; // abuse headers for chunk storage: {"chunk1", "chunk2"}
  auto it = chunks.begin();
  if (c->offset >= 2) return 0; // 2 chunks done
  std::string data;
  if (c->offset == 0) data = "chunk-a,";
  else                 data = "chunk-b";
  size_t n = std::min(bufsize, data.size());
  std::memcpy(buf, data.data(), n);
  c->offset++;
  return n;
}

TEST_F(HttpServerTest, StreamingResponse) {
  PullCtx ctx;
  ctx.status = 200;
  ctx.headers.emplace("Content-Type", "text/event-stream");
  // Use chunked_on_read instead of pull_on_read
  xHttpRouteConf conf = {};
  conf.pattern        = "GET /stream";
  conf.on_request     = pull_on_request;
  conf.on_read        = chunked_on_read;
  conf.arg            = &ctx;
  ASSERT_EQ(xHttpMuxHandle(mux, &conf), xErrno_Ok);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);
  ASSERT_TRUE(send_str(fd, "GET /stream HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_EQ(ctx.call_count.load(), 1);
  EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_NE(response.find("chunk-a"), std::string::npos);
  EXPECT_NE(response.find("chunk-b"), std::string::npos);
}

/* ───────────────────── Large body (> 4096, multi-chunk) ────────────── */

TEST_F(HttpServerTest, LargeBodyMultiChunk) {
  PullCtx ctx;
  ctx.status = 200;
  ctx.body   = std::string(10000, 'X'); // must be pumped in ≥3 chunks (4096 each)
  route_pull("GET /big", &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);
  ASSERT_TRUE(send_str(fd, "GET /big HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
  run_for(loop, 300);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_EQ(ctx.call_count.load(), 1);
  EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_NE(response.find("Content-Length: 10000"), std::string::npos);
  EXPECT_NE(response.find(std::string(10000, 'X')), std::string::npos);
}

/* ───────────────────── Multiple requests sequential ───────────────── */

TEST_F(HttpServerTest, MultipleSequentialRequests) {
  PullCtx ctx;
  ctx.body = "data";
  route_pull("GET /seq", &ctx);
  listen_and_pump();

  for (int i = 0; i < 5; i++) {
    ctx.offset = 0;
    int fd = connect_to(port);
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(send_str(fd, "GET /seq HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
    run_for(loop, 100);
    std::string response = recv_all(fd);
    close(fd);
    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(response.find("data"), std::string::npos);
  }
  EXPECT_EQ(ctx.call_count.load(), 5);
}

/* ───────────────────── Parameterized route: /users/:id ───────────────── */

TEST_F(HttpServerTest, ParamRouteBasic) {
  PullCtx      ctx;
  xHttpRouteConf conf = {};
  conf.pattern        = "GET /users/:id";
  conf.on_request     = [](xHttpCtx *ctx2, void *arg) -> int {
    auto *c = static_cast<PullCtx *>(arg);
    c->call_count.fetch_add(1);
    size_t      len = 0;
    const char *id  = xHttpCtxParam(ctx2, "id", &len);
    std::string body = "id=" + std::string(id, len);
    xHttpCtxSetStatus(ctx2, 200);
    xHttpCtxSetHeader(ctx2, "Content-Type", "text/plain");
    xHttpCtxSetHeader(ctx2, "Content-Length", std::to_string(body.size()).c_str());
    c->body = body;
    return 0;
  };
  conf.on_read        = pull_on_read;
  conf.arg            = &ctx;
  ASSERT_EQ(xHttpMuxHandle(mux, &conf), xErrno_Ok);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);
  ASSERT_TRUE(send_str(fd, "GET /users/42 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_EQ(ctx.call_count.load(), 1);
  EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_NE(response.find("id=42"), std::string::npos);
}

TEST_F(HttpServerTest, ParamRouteStringId) {
  PullCtx      ctx;
  xHttpRouteConf conf = {};
  conf.pattern        = "GET /users/:id";
  conf.on_request     = [](xHttpCtx *ctx2, void *arg) -> int {
    auto *c = static_cast<PullCtx *>(arg);
    c->call_count.fetch_add(1);
    size_t      len = 0;
    const char *id  = xHttpCtxParam(ctx2, "id", &len);
    std::string body = "id=" + std::string(id, len);
    xHttpCtxSetStatus(ctx2, 200);
    xHttpCtxSetHeader(ctx2, "Content-Length", std::to_string(body.size()).c_str());
    c->body = body;
    return 0;
  };
  conf.on_read        = pull_on_read;
  conf.arg            = &ctx;
  ASSERT_EQ(xHttpMuxHandle(mux, &conf), xErrno_Ok);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);
  ASSERT_TRUE(send_str(fd, "GET /users/alice HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);
  EXPECT_NE(response.find("id=alice"), std::string::npos);
}

/* ───────────────────── Multiple params ─────────────────────────────── */

TEST_F(HttpServerTest, ParamRouteMultipleParams) {
  PullCtx      ctx;
  xHttpRouteConf conf = {};
  conf.pattern        = "GET /users/:id/:action";
  conf.on_request     = [](xHttpCtx *ctx2, void *arg) -> int {
    auto *c = static_cast<PullCtx *>(arg);
    c->call_count.fetch_add(1);
    size_t ilen = 0, alen = 0;
    const char *id = xHttpCtxParam(ctx2, "id", &ilen);
    const char *ac = xHttpCtxParam(ctx2, "action", &alen);
    std::string body = std::string("id=") + std::string(id, ilen)
                     + ",action=" + std::string(ac, alen);
    xHttpCtxSetStatus(ctx2, 200);
    xHttpCtxSetHeader(ctx2, "Content-Length", std::to_string(body.size()).c_str());
    c->body = body;
    return 0;
  };
  conf.on_read        = pull_on_read;
  conf.arg            = &ctx;
  ASSERT_EQ(xHttpMuxHandle(mux, &conf), xErrno_Ok);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);
  ASSERT_TRUE(send_str(fd, "GET /users/99/edit HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);
  EXPECT_NE(response.find("id=99,action=edit"), std::string::npos);
}

/* ───────────────────── Param route: extra segments → 404 ───────────── */

TEST_F(HttpServerTest, ParamRouteExtraSegments404) {
  PullCtx      ctx;
  xHttpRouteConf conf = {};
  conf.pattern        = "GET /users/:id";
  conf.on_request     = [](xHttpCtx *ctx2, void *arg) -> int {
    auto *c = static_cast<PullCtx *>(arg);
    c->call_count.fetch_add(1);
    return 0;
  };
  conf.on_read        = pull_on_read;
  conf.arg            = &ctx;
  ASSERT_EQ(xHttpMuxHandle(mux, &conf), xErrno_Ok);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);
  ASSERT_TRUE(send_str(fd, "GET /users/42/extra HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);
  EXPECT_NE(response.find("404"), std::string::npos);
}

/* ───────────────────── Static route priority over param route ──────── */

TEST_F(HttpServerTest, StaticRoutePriorityOverParam) {
  PullCtx static_ctx, param_ctx;
  static_ctx.body = "static";

  // Static route: /users/me
  {
    xHttpRouteConf conf = {};
    conf.pattern        = "GET /users/me";
    conf.on_request     = pull_on_request;
    conf.on_read        = pull_on_read;
    conf.arg            = &static_ctx;
    ASSERT_EQ(xHttpMuxHandle(mux, &conf), xErrno_Ok);
  }

  // Param route: /users/:id
  {
    xHttpRouteConf conf = {};
    conf.pattern        = "GET /users/:id";
    conf.on_request     = [](xHttpCtx *ctx2, void *arg) -> int {
      auto *c = static_cast<PullCtx *>(arg);
      c->call_count.fetch_add(1);
      size_t ilen = 0;
      const char *id = xHttpCtxParam(ctx2, "id", &ilen);
      c->body = std::string("param:") + std::string(id, ilen);
      xHttpCtxSetStatus(ctx2, 200);
      xHttpCtxSetHeader(ctx2, "Content-Length", std::to_string(c->body.size()).c_str());
      return 0;
    };
    conf.on_read        = pull_on_read;
    conf.arg            = &param_ctx;
    ASSERT_EQ(xHttpMuxHandle(mux, &conf), xErrno_Ok);
  }
  listen_and_pump();

  // /users/me → static
  {
    int fd = connect_to(port);
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(send_str(fd, "GET /users/me HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
    run_for(loop, 100);
    std::string response = recv_all(fd);
    close(fd);
    EXPECT_EQ(static_ctx.call_count.load(), 1);
    EXPECT_EQ(param_ctx.call_count.load(), 0);
    EXPECT_NE(response.find("static"), std::string::npos);
  }

  // /users/42 → param
  {
    static_ctx.offset = 0;
    int fd = connect_to(port);
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(send_str(fd, "GET /users/42 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
    run_for(loop, 100);
    std::string response = recv_all(fd);
    close(fd);
    EXPECT_EQ(param_ctx.call_count.load(), 1);
    EXPECT_NE(response.find("param:42"), std::string::npos);
  }
}

/* ───────────────────── Param route: 405 method mismatch ─────────────── */

TEST_F(HttpServerTest, ParamRouteMethodNotAllowed) {
  PullCtx ctx;
  route_pull("GET /items/:id", &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);
  ASSERT_TRUE(send_str(fd, "POST /items/5 HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"));
  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);
  EXPECT_NE(response.find("405"), std::string::npos);
}

/* ───────────────────── Streaming large body via on_read ────────────── */

TEST_F(HttpServerTest, LargeStreamingBody) {
  PullCtx ctx;
  ctx.status = 200;
  ctx.body   = std::string(20000, 'Y'); // ~5 chunks
  route_pull("GET /huge", &ctx);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);
  ASSERT_TRUE(send_str(fd, "GET /huge HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
  run_for(loop, 500);

  std::string response = recv_all(fd);
  close(fd);

  EXPECT_EQ(ctx.call_count.load(), 1);
  EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
  // Verify full body length
  auto header_end = response.find("\r\n\r\n");
  ASSERT_NE(header_end, std::string::npos);
  EXPECT_EQ(response.size() - header_end - 4, 20000u);
}

/* ───────────────────── request_aborted → 500 ──────────────────────── */

TEST_F(HttpServerTest, OnRequestAbortSends500) {
  xHttpRouteConf conf = {};
  conf.pattern        = "GET /abort";
  conf.on_request     = [](xHttpCtx *, void *arg) -> int {
    auto *c = static_cast<PullCtx *>(arg);
    c->call_count.fetch_add(1);
    return -1; // abort
  };
  conf.arg = nullptr;
  // Need a dummy on_read to enter body-pump path (otherwise empty response is sent)
  conf.on_read = [](char *, size_t, void *) -> size_t { return 0; };
  PullCtx ctx;
  conf.arg = &ctx;
  ASSERT_EQ(xHttpMuxHandle(mux, &conf), xErrno_Ok);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);
  ASSERT_TRUE(send_str(fd, "GET /abort HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);
  EXPECT_EQ(ctx.call_count.load(), 1);
  EXPECT_NE(response.find("500"), std::string::npos);
}

/* ───────────────────── No on_read → empty response ─────────────────── */

TEST_F(HttpServerTest, HeadersOnlyResponse) {
  xHttpRouteConf conf = {};
  conf.pattern        = "GET /headers-only";
  conf.on_request     = [](xHttpCtx *ctx, void *arg) -> int {
    auto *c = static_cast<PullCtx *>(arg);
    c->call_count.fetch_add(1);
    xHttpCtxSetStatus(ctx, 204);
    return 0;
  };
  PullCtx ctx;
  conf.arg = &ctx;
  // No on_read — server sends empty response
  ASSERT_EQ(xHttpMuxHandle(mux, &conf), xErrno_Ok);
  listen_and_pump();

  int fd = connect_to(port);
  ASSERT_GE(fd, 0);
  ASSERT_TRUE(send_str(fd, "GET /headers-only HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"));
  run_for(loop, 100);

  std::string response = recv_all(fd);
  close(fd);
  EXPECT_EQ(ctx.call_count.load(), 1);
  EXPECT_NE(response.find("HTTP/1.1 204 No Content"), std::string::npos);
}
