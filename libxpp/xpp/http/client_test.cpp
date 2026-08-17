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
 * Each send test wraps client.send(req).await() in xpp::fiber() so the
 * await yields back to the EventLoop cleanly. TestServer internally
 * uses a fiber for its accept loop; on Linux shared builds, calling
 * xFiberSwitch from inside xEventLoopRun (which the non-fiber await
 * path uses) can deadlock. Running the client in a fiber too avoids
 * xEventLoopRun entirely — both fibers suspend via xFiberYield and
 * wake each other through PromiseWaker.
 */

#include <string>

#include <gtest/gtest.h>
#include <xpp/event.h>
#include <xpp/fiber.h>
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
  // Without an EventLoop entered, xHttpClientCreate returns NULL.
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

  Response resp =
    xpp::fiber([&]() { return client.send(std::move(req)).await(); }).await().unwrap();

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

  Response resp =
    xpp::fiber([&]() { return client.send(std::move(req)).await(); }).await().unwrap();
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

  auto resp_r = xpp::fiber([&]() { return client.send(std::move(req)).await(); }).await();
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

  auto resp_r = xpp::fiber([&]() { return client.send(std::move(req)).await(); }).await();
  ASSERT_TRUE(resp_r.is_err());
  EXPECT_TRUE(resp_r.unwrap_err().is_timeout());
}

TEST(ClientSendTest, CustomHeaderSent) {
  EventLoop loop;
  WaitScope scope(loop);

  // TestServer currently doesn't echo request headers back, but it
  // does drain them. We just verify the request with a custom header
  // is accepted and a response comes back.
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

  Response resp =
    xpp::fiber([&]() { return client.send(std::move(req)).await(); }).await().unwrap();
  EXPECT_EQ(resp.status(), StatusCode::Ok);
}
