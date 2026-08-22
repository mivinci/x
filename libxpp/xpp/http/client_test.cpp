/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * client_test.cpp — Layer 3 integration tests for xpp::http::Client.
 *
 * Uses xpp::http::test::TestServer (loopback, no external network) to
 * exercise Client::send end-to-end: request submission, push→pull
 * body bridge, header parsing, error mapping, and timeout.
 *
 * TestServer uses libx C API (xTcpListener + synchronous accept
 * callback), so no fibers are involved. client.send(req).await()
 * runs on the main thread via the non-fiber park() path
 * (xEventLoopRun), which is safe because no fiber switches occur
 * inside the EventLoop callbacks.
 */

#include <string>

#include <gtest/gtest.h>
#include <xpp/event.h>
#include <xpp/http/client.h>
#include <xpp/http/test_server.h>

using namespace xpp;
using namespace xpp::http;

/* ── Helper: build a URL for the TestServer ─────────────────────── */

static std::string url_for(uint16_t port, const char *path = "/") {
  return "http://127.0.0.1:" + std::to_string(port) + path;
}

/* ───────────────────────────────────────────────────────────────────
 *  Client construction
 * ─────────────────────────────────────────────────────────────────── */

TEST(ClientTest, BuilderProducesWorkingClient) {
  EventLoop loop;
  WaitScope scope(loop);

  auto client_r = Client::builder().build();
  ASSERT_TRUE(client_r.is_ok());
  Client client = std::move(client_r).unwrap();
  (void)client;
}

TEST(ClientTest, BuilderRejectsNoEventLoop) {
  auto client_r = Client::builder().build();
  EXPECT_TRUE(client_r.is_err());
  EXPECT_TRUE(client_r.unwrap_err().is_connect());
}

/* ───────────────────────────────────────────────────────────────────
 *  End-to-end send via TestServer
 * ─────────────────────────────────────────────────────────────────── */

TEST(ClientSendTest, GetReturns200WithBody) {
  EventLoop loop;
  WaitScope scope(loop);

  test::TestResponseSpec spec;
  spec.status = StatusCode::Ok;
  spec.headers.push(
    {String::from_utf8("Content-Type").unwrap(), String::from_utf8("text/plain").unwrap()});
  spec.body = Bytes::from("hello");

  auto server = test::TestServer::start(spec);

  auto req =
    Request::builder().method(Method::Get).url(url_for(server.port()).c_str()).body().unwrap();

  auto client_r = Client::builder().build();
  ASSERT_TRUE(client_r.is_ok());
  Client client = std::move(client_r).unwrap();

  auto resp_r = client.send(std::move(req)).await();
  ASSERT_TRUE(resp_r.is_ok());
  Response resp = std::move(resp_r).unwrap();

  EXPECT_EQ(resp.status(), StatusCode::Ok);
  auto ct = resp.headers().get(String::from_utf8("content-type").unwrap());
  ASSERT_TRUE(ct.is_some());
  EXPECT_EQ(ct.unwrap(), String::from_utf8("text/plain").unwrap());

  auto body = resp.bytes().await().unwrap();
  EXPECT_EQ(body.to_string().unwrap(), String::from_utf8("hello").unwrap());
}

TEST(ClientSendTest, PostWithBodyRoundTrips) {
  EventLoop loop;
  WaitScope scope(loop);

  test::TestResponseSpec spec;
  spec.status = StatusCode::Ok;
  spec.body   = Bytes::from("ack");

  auto server = test::TestServer::start(spec);

  auto req = Request::builder()
               .method(Method::Post)
               .url(url_for(server.port()).c_str())
               .header("Content-Type", "text/plain")
               .body("payload")
               .unwrap();

  auto client_r = Client::builder().build();
  ASSERT_TRUE(client_r.is_ok());
  Client client = std::move(client_r).unwrap();

  auto resp_r = client.send(std::move(req)).await();
  ASSERT_TRUE(resp_r.is_ok());
  Response resp = std::move(resp_r).unwrap();
  EXPECT_EQ(resp.status(), StatusCode::Ok);

  auto body = resp.bytes().await().unwrap();
  EXPECT_EQ(body.to_string().unwrap(), String::from_utf8("ack").unwrap());
}

TEST(ClientSendTest, NotFoundReturns400LevelStatus) {
  EventLoop loop;
  WaitScope scope(loop);

  test::TestResponseSpec spec;
  spec.status = StatusCode::NotFound;
  spec.body   = Bytes::from("nope");

  auto server = test::TestServer::start(spec);

  auto req =
    Request::builder().method(Method::Get).url(url_for(server.port()).c_str()).body().unwrap();

  auto client_r = Client::builder().build();
  ASSERT_TRUE(client_r.is_ok());
  Client client = std::move(client_r).unwrap();

  auto resp_r = client.send(std::move(req)).await();
  ASSERT_TRUE(resp_r.is_err());
  auto err = resp_r.unwrap_err();
  EXPECT_TRUE(err.is_protocol());
  EXPECT_TRUE(err.is_status_error());
  ASSERT_TRUE(err.status().is_some());
  EXPECT_EQ(err.status().unwrap(), StatusCode::NotFound);
}

TEST(ClientSendTest, TimeoutTriggersError) {
  EventLoop loop;
  WaitScope scope(loop);

  // Server delays 200ms before responding; client times out at 50ms.
  test::TestResponseSpec spec;
  spec.status   = StatusCode::Ok;
  spec.body     = Bytes::from("slow");
  spec.delay_ms = 200;

  auto server = test::TestServer::start(spec);

  auto req =
    Request::builder().method(Method::Get).url(url_for(server.port()).c_str()).body().unwrap();

  auto client_r = Client::builder().timeout(50).build();
  ASSERT_TRUE(client_r.is_ok());
  Client client = std::move(client_r).unwrap();

  auto resp_r = client.send(std::move(req)).await();
  ASSERT_TRUE(resp_r.is_err());
  EXPECT_TRUE(resp_r.unwrap_err().is_timeout());
}

TEST(ClientSendTest, PostBodyIsTransmitted) {
  EventLoop loop;
  WaitScope scope(loop);

  // Echo the request body back — proves the POST payload was actually
  // transmitted (not silently dropped).
  test::TestResponseSpec spec;
  spec.status            = StatusCode::Ok;
  spec.echo_request_body = true;

  auto server = test::TestServer::start(spec);

  auto req = Request::builder()
               .method(Method::Post)
               .url(url_for(server.port()).c_str())
               .body("hello payload")
               .unwrap();

  auto client_r = Client::builder().build();
  ASSERT_TRUE(client_r.is_ok());
  Client client = std::move(client_r).unwrap();

  auto resp_r = client.send(std::move(req)).await();
  ASSERT_TRUE(resp_r.is_ok());
  Response resp = std::move(resp_r).unwrap();
  EXPECT_EQ(resp.status(), StatusCode::Ok);

  auto body = resp.bytes().await().unwrap();
  EXPECT_EQ(body.to_string().unwrap(), String::from_utf8("hello payload").unwrap());
}

TEST(ClientSendTest, LargeBodyRoundTrips) {
  EventLoop loop;
  WaitScope scope(loop);

  // 2MB request + 2MB echoed response. Exercises:
  //  - request upload via on_read (multi-chunk)
  //  - server-side partial reads / partial writes (level-triggered events)
  //  - response channel headroom (128 chunks > 64 would have overflowed)
  test::TestResponseSpec spec;
  spec.status            = StatusCode::Ok;
  spec.echo_request_body = true;

  auto server = test::TestServer::start(spec);

  std::string payload(2 * 1024 * 1024, 'x');
  auto        req = Request::builder()
               .method(Method::Post)
               .url(url_for(server.port()).c_str())
               .body(payload.c_str())
               .unwrap();

  auto client_r = Client::builder().build();
  ASSERT_TRUE(client_r.is_ok());
  Client client = std::move(client_r).unwrap();

  auto resp_r = client.send(std::move(req)).await();
  ASSERT_TRUE(resp_r.is_ok()) << "send failed";
  Response resp = std::move(resp_r).unwrap();
  EXPECT_EQ(resp.status(), StatusCode::Ok);

  auto body = resp.bytes().await();
  ASSERT_TRUE(body.is_ok()) << "body read failed";
  EXPECT_EQ(body.unwrap().size(), payload.size());
}

TEST(ClientSendTest, RedirectFollowed) {
  EventLoop loop;
  WaitScope scope(loop);

  // /a → 302 Location: /b → 200 with the preset spec. Asserts both the
  // final URL (Response::url) and the final response's headers (fixes:
  // redirect tracking dead code + first-hop-header parsing).
  test::TestResponseSpec spec;
  spec.status      = StatusCode::Ok;
  spec.redirect_to = String::from_utf8("/b").unwrap();
  spec.body        = Bytes::from("redirected!");
  spec.headers.push({String::from_utf8("X-Final-Hop").unwrap(), String::from_utf8("yes").unwrap()});

  auto server = test::TestServer::start(spec);

  auto req = Request::builder()
               .method(Method::Get)
               .url(url_for(server.port(), "/a").c_str())
               .body()
               .unwrap();

  auto client_r = Client::builder().build();
  ASSERT_TRUE(client_r.is_ok());
  Client client = std::move(client_r).unwrap();

  auto resp_r = client.send(std::move(req)).await();
  ASSERT_TRUE(resp_r.is_ok());
  Response resp = std::move(resp_r).unwrap();
  EXPECT_EQ(resp.status(), StatusCode::Ok);

  // Final URL after redirects (was always None — dead code).
  ASSERT_TRUE(resp.url().is_some());
  EXPECT_EQ(resp.url().unwrap(), String::from_utf8(url_for(server.port(), "/b").c_str()).unwrap());

  // Headers from the FINAL response, not the 302's (was first-hop).
  auto x = resp.headers().get(String::from_utf8("x-final-hop").unwrap());
  ASSERT_TRUE(x.is_some());
  EXPECT_EQ(x.unwrap(), String::from_utf8("yes").unwrap());
  // The 302's Location header must not leak into the final response.
  EXPECT_FALSE(resp.headers().contains(String::from_utf8("location").unwrap()));

  auto body = resp.bytes().await().unwrap();
  EXPECT_EQ(body.to_string().unwrap(), String::from_utf8("redirected!").unwrap());
}

TEST(ClientSendTest, LargeBodyBackpressure) {
  EventLoop loop;
  WaitScope scope(loop);

  // 8MB response (~512 × 16KB chunks) far exceeds the 256-slot channel.
  // Without on_data pause/resume this aborts with a body-buffer-overflow
  // error; with it the transfer flows under backpressure.
  test::TestResponseSpec spec;
  spec.status = StatusCode::Ok;
  spec.body   = Bytes::from(std::string(8 * 1024 * 1024, 'y').c_str());

  auto server = test::TestServer::start(spec);

  auto req =
    Request::builder().method(Method::Get).url(url_for(server.port()).c_str()).body().unwrap();

  auto client_r = Client::builder().build();
  ASSERT_TRUE(client_r.is_ok());
  Client client = std::move(client_r).unwrap();

  auto resp_r = client.send(std::move(req)).await();
  ASSERT_TRUE(resp_r.is_ok()) << "send failed";
  Response resp = std::move(resp_r).unwrap();
  EXPECT_EQ(resp.status(), StatusCode::Ok);

  auto body = resp.bytes().await();
  ASSERT_TRUE(body.is_ok()) << "body read failed";
  EXPECT_EQ(body.unwrap().size(), 8u * 1024u * 1024u);
}

TEST(ClientSendTest, MidBodyDisconnectReportsError) {
  EventLoop loop;
  WaitScope scope(loop);

  // The server declares a 1MB body but sends only 64KB then closes.
  // send() still resolves Ok (headers arrived — reqwest semantics), but
  // reading the body must surface an error instead of a silent truncated
  // EOF.
  test::TestResponseSpec spec;
  spec.status              = StatusCode::Ok;
  spec.body                = Bytes::from(std::string(1024 * 1024, 'z').c_str());
  spec.truncate_body_after = 64 * 1024;

  auto server = test::TestServer::start(spec);

  auto req =
    Request::builder().method(Method::Get).url(url_for(server.port()).c_str()).body().unwrap();

  auto client_r = Client::builder().build();
  ASSERT_TRUE(client_r.is_ok());
  Client client = std::move(client_r).unwrap();

  auto resp_r = client.send(std::move(req)).await();
  ASSERT_TRUE(resp_r.is_ok()) << "send failed"; // headers arrived
  Response resp = std::move(resp_r).unwrap();
  EXPECT_EQ(resp.status(), StatusCode::Ok);

  auto body = resp.bytes().await();
  ASSERT_TRUE(body.is_err()) << "truncated transfer must not look like clean EOF";
  EXPECT_TRUE(body.unwrap_err().is_io());
}

TEST(ClientSendTest, ReadTimeoutTriggersError) {
  EventLoop loop;
  WaitScope scope(loop);

  // Server sends half the body then stalls 2s; client read_timeout 1s
  // (low-speed detection). send() resolves Ok (headers arrived), but the
  // body read must fail with a timeout.
  test::TestResponseSpec spec;
  spec.status            = StatusCode::Ok;
  spec.body              = Bytes::from(std::string(1024 * 1024, 'r').c_str());
  spec.mid_body_delay_ms = 2000;

  auto server = test::TestServer::start(spec);

  auto req =
    Request::builder().method(Method::Get).url(url_for(server.port()).c_str()).body().unwrap();

  auto client_r = Client::builder().read_timeout(1000).build();
  ASSERT_TRUE(client_r.is_ok());
  Client client = std::move(client_r).unwrap();

  auto resp_r = client.send(std::move(req)).await();
  ASSERT_TRUE(resp_r.is_ok()) << "send failed"; // headers arrived
  Response resp = std::move(resp_r).unwrap();

  auto body = resp.bytes().await();
  ASSERT_TRUE(body.is_err()) << "stalled body must time out, not succeed";
  EXPECT_TRUE(body.unwrap_err().is_timeout());
}

TEST(ClientSendTest, CustomHeaderSent) {
  EventLoop loop;
  WaitScope scope(loop);

  test::TestResponseSpec spec;
  spec.status = StatusCode::Ok;

  auto server = test::TestServer::start(spec);

  auto req = Request::builder()
               .method(Method::Get)
               .url(url_for(server.port()).c_str())
               .header("X-Custom", "value123")
               .body()
               .unwrap();

  auto client_r = Client::builder().build();
  ASSERT_TRUE(client_r.is_ok());
  Client client = std::move(client_r).unwrap();

  auto resp_r = client.send(std::move(req)).await();
  ASSERT_TRUE(resp_r.is_ok());
  EXPECT_EQ(resp_r.unwrap().status(), StatusCode::Ok);
}
