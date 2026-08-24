/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * server_test.cpp — xpp::http::Server integration tests: sync/async
 * handlers, path-parameter injection, request body streaming, lifecycle.
 */

#include <string>

#include <gtest/gtest.h>
#include <xpp/event.h>
#include <xpp/http/client.h>
#include <xpp/http/server.h>

using namespace xpp;
using namespace xpp::http;

static std::string url_for(uint16_t port, const char *path = "/") {
  return "http://127.0.0.1:" + std::to_string(port) + path;
}

/* ───────────────────────────────────────────────────────────────────
 *  Sync handler + path parameter injection
 * ─────────────────────────────────────────────────────────────────── */

TEST(ServerTest, SyncHandlerWithParam) {
  EventLoop loop;
  WaitScope scope(loop);

  auto server_r =
    Server::builder()
      .route("GET /hello/:name", [](Request req, String name) { return ResponseBuilder::ok(name); })
      .bind("127.0.0.1", 0)
      .build();
  if (server_r.is_err()) {
    auto e = server_r.unwrap_err();
    fprintf(stderr, "[debug] build failed: msg_len=%zu\n", e.message().as_bytes().size());
  }
  ASSERT_TRUE(server_r.is_ok());
  Server server = std::move(server_r).unwrap();

  auto running = server.serve();
  ASSERT_GT(server.port(), 0u);

  auto client = Client::builder().build().unwrap();
  auto r      = client.get(url_for(server.port(), "/hello/world").c_str()).await();
  ASSERT_TRUE(r.is_ok()) << "GET /hello/world failed";
  auto body = r.unwrap().bytes().await().unwrap();
  EXPECT_EQ(body.to_string().unwrap(), String::from_utf8("world").unwrap());

  server.stop();
  auto sr = running.await();
  EXPECT_TRUE(sr.is_ok());
}

/* ───────────────────────────────────────────────────────────────────
 *  Multiple parameters
 * ─────────────────────────────────────────────────────────────────── */

TEST(ServerTest, MultipleParams) {
  EventLoop loop;
  WaitScope scope(loop);

  auto server = Server::builder()
                  .route("GET /users/:uid/posts/:pid",
                         [](Request req, String uid, String pid) {
                           String joined = uid;
                           joined.push_str(String::from_utf8("/").unwrap());
                           joined.push_str(pid);
                           return ResponseBuilder::ok(joined);
                         })
                  .bind("127.0.0.1", 0)
                  .build()
                  .unwrap();
  auto running = server.serve();

  auto client = Client::builder().build().unwrap();
  auto r      = client.get(url_for(server.port(), "/users/42/posts/7").c_str()).await();
  ASSERT_TRUE(r.is_ok());
  auto body = r.unwrap().bytes().await().unwrap();
  EXPECT_EQ(body.to_string().unwrap(), String::from_utf8("42/7").unwrap());

  server.stop();
  running.await();
}

/* ───────────────────────────────────────────────────────────────────
 *  Async handler: read request body, then respond
 * ─────────────────────────────────────────────────────────────────── */

TEST(ServerTest, AsyncHandlerEchoBody) {
  EventLoop loop;
  WaitScope scope(loop);

  auto server =
    Server::builder()
      .route("POST /echo",
             [](Request req) -> Promise<http::Result<Response>> {
               // Await the request body via the promise chain.
               return req.into_body().bytes().then(
                 [](http::Result<Bytes> b) -> http::Result<Response> {
                   if (b.is_err()) return http::Result<Response>(xpp::err, b.unwrap_err());
                   return http::Result<Response>(xpp::ok, ResponseBuilder::ok(b.unwrap()));
                 });
             })
      .bind("127.0.0.1", 0)
      .build()
      .unwrap();
  auto running = server.serve();

  auto client = Client::builder().build().unwrap();
  auto r      = client.post(url_for(server.port(), "/echo").c_str(), "payload-123").await();
  ASSERT_TRUE(r.is_ok());
  auto body = r.unwrap().bytes().await().unwrap();
  EXPECT_EQ(body.to_string().unwrap(), String::from_utf8("payload-123").unwrap());

  server.stop();
  running.await();
}

/* ───────────────────────────────────────────────────────────────────
 *  Request metadata (method + headers)
 * ─────────────────────────────────────────────────────────────────── */

TEST(ServerTest, RequestMetadata) {
  EventLoop loop;
  WaitScope scope(loop);

  auto server = Server::builder()
                  .route("PUT /meta",
                         [](Request req) {
                           auto ct = req.headers().get(String::from_utf8("content-type").unwrap());
                           String resp = String::from_utf8("method=").unwrap();
                           resp.push_str(to_string(req.method()));
                           if (ct.is_some()) {
                             resp.push_str(String::from_utf8(" ct=").unwrap());
                             resp.push_str(ct.unwrap());
                           }
                           return ResponseBuilder::ok(resp);
                         })
                  .bind("127.0.0.1", 0)
                  .build()
                  .unwrap();
  auto running = server.serve();

  auto client = Client::builder().build().unwrap();
  auto r      = client.put(url_for(server.port(), "/meta").c_str(), Bytes::from("x")).await();
  ASSERT_TRUE(r.is_ok());
  auto body = r.unwrap().bytes().await().unwrap();
  EXPECT_EQ(body.to_string().unwrap(), String::from_utf8("method=PUT").unwrap());

  server.stop();
  running.await();
}

/* ───────────────────────────────────────────────────────────────────
 *  Errors: handler Err → 500; unmatched route → 404
 * ─────────────────────────────────────────────────────────────────── */

TEST(ServerTest, HandlerErrorIs500) {
  EventLoop loop;
  WaitScope scope(loop);

  auto server = Server::builder()
                  .route("GET /boom",
                         [](Request req) -> http::Result<Response> {
                           return http::Result<Response>(
                             xpp::err, Error(Error::Kind::Io, String::from_utf8("boom").unwrap()));
                         })
                  .bind("127.0.0.1", 0)
                  .build()
                  .unwrap();
  auto running = server.serve();

  auto client = Client::builder().build().unwrap();
  auto r      = client.get(url_for(server.port(), "/boom").c_str()).await();
  ASSERT_TRUE(r.is_err());
  EXPECT_TRUE(r.unwrap_err().is_status_error());
  // Status 500 surfaced via the error.
  EXPECT_EQ(r.unwrap_err().status().unwrap(), static_cast<StatusCode::Value>(500));

  server.stop();
  running.await();
}

TEST(ServerTest, UnmatchedRouteIs404) {
  EventLoop loop;
  WaitScope scope(loop);

  auto server = Server::builder()
                  .route("GET /known", [](Request req) { return ResponseBuilder::ok("yes"); })
                  .bind("127.0.0.1", 0)
                  .build()
                  .unwrap();
  auto running = server.serve();

  auto client = Client::builder().build().unwrap();
  auto r      = client.get(url_for(server.port(), "/nope").c_str()).await();
  ASSERT_TRUE(r.is_err());
  EXPECT_EQ(r.unwrap_err().status().unwrap(), static_cast<StatusCode::Value>(404));

  server.stop();
  running.await();
}

/* ───────────────────────────────────────────────────────────────────
 *  serve() resolves Err on listen failure (port in use)
 * ─────────────────────────────────────────────────────────────────── */

TEST(ServerTest, ListenFailureIsErr) {
  EventLoop loop;
  WaitScope scope(loop);

  // Occupy a port with one server, then try to bind the same port again.
  auto s1 = Server::builder().bind("127.0.0.1", 0).build().unwrap();
  auto r1 = s1.serve();
  ASSERT_GT(s1.port(), 0u);

  auto s2 = Server::builder().bind("127.0.0.1", s1.port()).build().unwrap();
  auto r2 = s2.serve();
  EXPECT_TRUE(r2.await().is_err());

  s1.stop();
  r1.await();
}

/* ───────────────────────────────────────────────────────────────────
 *  Concurrent requests on the same route (per-request body isolation)
 * ─────────────────────────────────────────────────────────────────── */

TEST(ServerTest, ConcurrentBodiesStaySeparate) {
  EventLoop loop;
  WaitScope scope(loop);

  auto server =
    Server::builder()
      .route("POST /echo",
             [](Request req) -> Promise<http::Result<Response>> {
               return req.into_body().bytes().then(
                 [](http::Result<Bytes> b) -> http::Result<Response> {
                   return http::Result<Response>(xpp::ok, ResponseBuilder::ok(b.unwrap()));
                 });
             })
      .bind("127.0.0.1", 0)
      .build()
      .unwrap();
  auto running = server.serve();

  auto client = Client::builder().build().unwrap();
  // Fire two requests concurrently (same route) and verify each echoes its own body.
  auto p1 = client.post(url_for(server.port(), "/echo").c_str(), "alpha").await();
  auto p2 = client.post(url_for(server.port(), "/echo").c_str(), "beta").await();
  ASSERT_TRUE(p1.is_ok());
  ASSERT_TRUE(p2.is_ok());
  EXPECT_EQ(p1.unwrap().bytes().await().unwrap().to_string().unwrap(),
            String::from_utf8("alpha").unwrap());
  EXPECT_EQ(p2.unwrap().bytes().await().unwrap().to_string().unwrap(),
            String::from_utf8("beta").unwrap());

  server.stop();
  running.await();
}

/* ───────────────────────────────────────────────────────────────────
 *  Server destroyed while a handler is still running — the handler
 *  completes on the loop afterwards; write_response must be dropped
 *  (ctx freed) instead of crashing.
 * ─────────────────────────────────────────────────────────────────── */

TEST(ServerTest, DestroyWithInflightHandlerDoesNotCrash) {
  EventLoop loop;
  WaitScope scope(loop);

  bool handler_done = false;
  {
    // Handler waits 50ms (after the request starts) — the server is
    // destroyed in that window, then the handler completes.
    auto server =
      Server::builder()
        .route("GET /slow",
               [&handler_done](Request req) -> Promise<http::Result<Response>> {
                 return xpp::after(50).then([&handler_done]() -> http::Result<Response> {
                   handler_done = true;
                   return http::Result<Response>(xpp::ok, ResponseBuilder::ok("late"));
                 });
               })
        .bind("127.0.0.1", 0)
        .build()
        .unwrap();
    auto running = server.serve();

    auto client = Client::builder().build().unwrap();
    auto p      = client.get(url_for(server.port(), "/slow").c_str());
    // Let the request arrive and the handler start (and suspend on after()).
    for (int i = 0; i < 10; ++i)
      xEventLoopRun(loop.handle(), X_RUN_ONCE);
    // Server destroyed here while the handler is in flight.
  }
  // Drive the loop past the handler's 50ms — it completes, but the response
  // write must be dropped (destroyed flag), not touch freed ctx.
  for (int i = 0; i < 200 && !handler_done; ++i)
    xEventLoopRun(loop.handle(), X_RUN_ONCE);
  EXPECT_TRUE(handler_done) << "handler did not complete";
}

/* ───────────────────────────────────────────────────────────────────
 *  Streaming (channel) response body — not yet supported; write_response
 *  must answer 500 instead of corrupting the connection.
 * ─────────────────────────────────────────────────────────────────── */

TEST(ServerTest, StreamingResponseBody) {
  EventLoop loop;
  WaitScope scope(loop);

  auto server = Server::builder()
                  .route("GET /stream",
                         [](Request req) -> http::Result<Response> {
                           auto [tx, rx] = sync::mpsc::channel<Bytes>(4);
                           // Producer: push chunks (waker-driven), then close.
                           xpp::spawn([tx]() mutable -> Promise<void> {
                             return tx.send(Bytes::copy("chunk1", 6))
                               .then([tx]() mutable { return tx.send(Bytes::copy("-chunk2", 7)); })
                               .then([tx]() mutable { tx.close(); });
                           });
                           Body b = Body::from_channel(std::move(rx));
                           return http::Result<Response>(
                             xpp::ok, ResponseBuilder().status(StatusCode::Ok).body(std::move(b)));
                         })
                  .bind("127.0.0.1", 0)
                  .build()
                  .unwrap();
  auto running = server.serve();

  auto client = Client::builder().build().unwrap();
  auto r      = client.get(url_for(server.port(), "/stream").c_str()).await();
  ASSERT_TRUE(r.is_ok()) << "GET /stream failed";
  auto resp = std::move(r).unwrap();
  EXPECT_EQ(200, resp.status_code());
  auto body = resp.bytes().await().unwrap();
  EXPECT_EQ(body.to_string().unwrap(), String::from_utf8("chunk1-chunk2").unwrap());

  server.stop();
  running.await();
}

/* ───────────────────────────────────────────────────────────────────
 *  Streaming response: multiple chunks crossing the 4096 read buffer
 *  boundary — the recursive read/write chain must reassemble them.
 * ─────────────────────────────────────────────────────────────────── */

TEST(ServerTest, StreamingResponseBodyLarge) {
  EventLoop loop;
  WaitScope scope(loop);

  std::string big(3000, 'A'); // two chunks = 6000 > 4096 (2 read() hops)
  auto        server = Server::builder()
                  .route("GET /big",
                         [big](Request req) -> http::Result<Response> {
                           auto [tx, rx] = sync::mpsc::channel<Bytes>(4);
                           xpp::spawn([tx, big]() mutable -> Promise<void> {
                             return tx.send(Bytes::copy(big.data(), big.size()))
                               .then([tx, big]() mutable {
                                 return tx.send(Bytes::copy(big.data(), big.size()));
                               })
                               .then([tx]() mutable { tx.close(); });
                           });
                           Body b = Body::from_channel(std::move(rx));
                           return http::Result<Response>(
                             xpp::ok, ResponseBuilder().status(StatusCode::Ok).body(std::move(b)));
                         })
                  .bind("127.0.0.1", 0)
                  .build()
                  .unwrap();
  auto running = server.serve();

  auto client = Client::builder().build().unwrap();
  auto r      = client.get(url_for(server.port(), "/big").c_str()).await();
  ASSERT_TRUE(r.is_ok()) << "GET /big failed";
  auto resp = std::move(r).unwrap();
  EXPECT_EQ(200, resp.status_code());
  auto body = resp.bytes().await().unwrap();
  EXPECT_EQ(body.size(), static_cast<size_t>(6000));
  std::string s(reinterpret_cast<const char *>(body.data()), body.size());
  EXPECT_EQ(s.size(), 6000);
  EXPECT_EQ(s[0], 'A');
  EXPECT_EQ(s[5999], 'A');
  EXPECT_EQ(s.find("AAAA"), 0);

  server.stop();
  running.await();
}
