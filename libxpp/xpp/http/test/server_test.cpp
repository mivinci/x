/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * server_test.cpp — Tests for xpp::http::test::Server (Layer 1 fixture).
 */

#include <string>

#include <gtest/gtest.h>
#include <xpp/event.h>
#include <xpp/http/client.h>
#include <xpp/http/test/server.h>

using namespace xpp;
using namespace xpp::http;

static std::string url_of(const test::Server &ts, const char *path = "") {
  return "http://127.0.0.1:" + std::to_string(ts.port()) + path;
}

TEST(TestServerTest, HandlerConstructor) {
  EventLoop loop;
  WaitScope scope(loop);

  test::Server ts([](Request) { return Response::ok("hello"); });
  EXPECT_GT(ts.port(), 0);

  auto client = Client::builder().build().unwrap();
  auto r      = client.get(url_of(ts).c_str()).await();
  ASSERT_TRUE(r.is_ok());
  auto body = r.unwrap().bytes().await().unwrap().to_string().unwrap();
  EXPECT_EQ(body, "hello");
}

TEST(TestServerTest, SpecConstructor) {
  EventLoop loop;
  WaitScope scope(loop);

  test::TestResponseSpec spec;
  spec.body = Bytes::from("spec-body");
  test::Server ts(spec);

  auto client = Client::builder().build().unwrap();
  auto r      = client.get(url_of(ts).c_str()).await();
  ASSERT_TRUE(r.is_ok());
  auto body = r.unwrap().bytes().await().unwrap().to_string().unwrap();
  EXPECT_EQ(body, "spec-body");
}

TEST(TestServerTest, SpecRedirect) {
  EventLoop loop;
  WaitScope scope(loop);

  test::TestResponseSpec spec;
  spec.redirect_to = String::from_utf8("/b").unwrap();
  spec.body        = Bytes::from("redirected!");
  test::Server ts(spec);

  auto client = Client::builder().redirect(3).build().unwrap();
  auto r      = client.get(url_of(ts, "/a").c_str()).await();
  ASSERT_TRUE(r.is_ok());
  auto body = r.unwrap().bytes().await().unwrap().to_string().unwrap();
  EXPECT_EQ(body, "redirected!");
}

TEST(TestServerTest, SpecEchoBody) {
  EventLoop loop;
  WaitScope scope(loop);

  test::TestResponseSpec spec;
  spec.echo_request_body = true;
  test::Server ts(spec);

  auto client = Client::builder().build().unwrap();
  auto r      = client.post(url_of(ts).c_str(), "my-payload").await();
  ASSERT_TRUE(r.is_ok());
  auto body = r.unwrap().bytes().await().unwrap().to_string().unwrap();
  EXPECT_EQ(body, "my-payload");
}

TEST(TestServerTest, SpecEchoMethod) {
  EventLoop loop;
  WaitScope scope(loop);

  test::TestResponseSpec spec;
  spec.echo_request_method = true;
  spec.echo_request_body   = true;
  test::Server ts(spec);

  auto client = Client::builder().build().unwrap();
  auto r      = client.post(url_of(ts).c_str(), "data").await();
  ASSERT_TRUE(r.is_ok());
  auto m = r.unwrap().headers().get(String::from_utf8("x-echo-method").unwrap());
  ASSERT_TRUE(m.is_some());
  EXPECT_EQ(m.unwrap(), String::from_utf8("POST").unwrap());
}

TEST(TestServerTest, BuilderConfigureConstructor) {
  EventLoop loop;
  WaitScope scope(loop);

  test::Server ts([](ServerBuilder &b) {
    b.route("GET /users/:id", [](Request, String id) { return Response::ok(id); });
  });

  auto client = Client::builder().build().unwrap();
  auto r      = client.get(url_of(ts, "/users/42").c_str()).await();
  ASSERT_TRUE(r.is_ok());
  auto body = r.unwrap().bytes().await().unwrap().to_string().unwrap();
  EXPECT_EQ(body, "42");
}

TEST(TestServerTest, SpecDelay) {
  EventLoop loop;
  WaitScope scope(loop);

  test::TestResponseSpec spec;
  spec.body     = Bytes::from("slow");
  spec.delay_ms = 50; // short enough for CI, enough to verify the timer path
  test::Server ts(spec);

  auto client = Client::builder().timeout(5000).build().unwrap();
  auto r      = client.get(url_of(ts).c_str()).await();
  ASSERT_TRUE(r.is_ok());
  auto body = r.unwrap().bytes().await().unwrap().to_string().unwrap();
  EXPECT_EQ(body, "slow");
}
