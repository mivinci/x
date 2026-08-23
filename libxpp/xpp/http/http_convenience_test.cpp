/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * http_convenience_test.cpp — Phase 6: Client convenience methods +
 * top-level xpp::http::get/post/... against the local TestServer.
 */

#include <string>

#include <gtest/gtest.h>
#include <xpp/event.h>
#include <xpp/http/client.h>
#include <xpp/http/test_server.h>

using namespace xpp;
using namespace xpp::http;

static std::string url_for(uint16_t port, const char *path = "/") {
  return "http://127.0.0.1:" + std::to_string(port) + path;
}

/* ───────────────────────────────────────────────────────────────────
 *  Client convenience methods
 * ─────────────────────────────────────────────────────────────────── */

TEST(ClientConvenienceTest, VerbsRoundTrip) {
  EventLoop loop;
  WaitScope scope(loop);

  // Echo the request method + body back so we can verify the verb.
  test::TestResponseSpec spec;
  spec.status              = StatusCode::Ok;
  spec.echo_request_method = true;
  spec.echo_request_body   = true;

  auto server = test::TestServer::start(spec);
  auto client = Client::builder().build().unwrap();

  // GET
  {
    auto r = client.get(url_for(server.port(), "/a").c_str()).await();
    ASSERT_TRUE(r.is_ok());
    auto resp = std::move(r).unwrap();
    auto m    = resp.headers().get(String::from_utf8("x-echo-method").unwrap());
    ASSERT_TRUE(m.is_some());
    EXPECT_EQ(m.unwrap(), String::from_utf8("GET").unwrap());
  }
  // POST with body
  {
    auto r = client.post(url_for(server.port(), "/b").c_str(), "hello").await();
    ASSERT_TRUE(r.is_ok());
    auto resp = std::move(r).unwrap();
    auto m    = resp.headers().get(String::from_utf8("x-echo-method").unwrap());
    ASSERT_TRUE(m.is_some());
    EXPECT_EQ(m.unwrap(), String::from_utf8("POST").unwrap());
    auto body = resp.bytes().await().unwrap();
    EXPECT_EQ(body.to_string().unwrap(), String::from_utf8("hello").unwrap());
  }
  // PUT with body
  {
    auto r = client.put(url_for(server.port(), "/c").c_str(), Bytes::from("p")).await();
    ASSERT_TRUE(r.is_ok());
    auto m = r.unwrap().headers().get(String::from_utf8("x-echo-method").unwrap());
    ASSERT_TRUE(m.is_some());
    EXPECT_EQ(m.unwrap(), String::from_utf8("PUT").unwrap());
  }
  // DELETE (del)
  {
    auto r = client.del(url_for(server.port(), "/d").c_str()).await();
    ASSERT_TRUE(r.is_ok());
    auto m = r.unwrap().headers().get(String::from_utf8("x-echo-method").unwrap());
    ASSERT_TRUE(m.is_some());
    EXPECT_EQ(m.unwrap(), String::from_utf8("DELETE").unwrap());
  }
  // PATCH with body
  {
    auto r = client.patch(url_for(server.port(), "/e").c_str(), "x").await();
    ASSERT_TRUE(r.is_ok());
    auto m = r.unwrap().headers().get(String::from_utf8("x-echo-method").unwrap());
    ASSERT_TRUE(m.is_some());
    EXPECT_EQ(m.unwrap(), String::from_utf8("PATCH").unwrap());
  }
  // HEAD
  {
    auto r = client.head(url_for(server.port(), "/f").c_str()).await();
    ASSERT_TRUE(r.is_ok());
    auto m = r.unwrap().headers().get(String::from_utf8("x-echo-method").unwrap());
    ASSERT_TRUE(m.is_some());
    EXPECT_EQ(m.unwrap(), String::from_utf8("HEAD").unwrap());
  }
}

TEST(ClientConvenienceTest, UrlOverloadsCompileAndRoundTrip) {
  EventLoop loop;
  WaitScope scope(loop);

  test::TestResponseSpec spec;
  spec.status = StatusCode::Ok;
  auto server = test::TestServer::start(spec);
  auto client = Client::builder().build().unwrap();

  // const char*
  ASSERT_TRUE(client.get(url_for(server.port()).c_str()).await().is_ok());
  // String
  ASSERT_TRUE(
    client.get(String::from_utf8(url_for(server.port()).c_str()).unwrap()).await().is_ok());
  // post with Body / String / Vec / const char*
  ASSERT_TRUE(client.post(url_for(server.port()).c_str(), Body::from("b")).await().is_ok());
  ASSERT_TRUE(
    client.post(url_for(server.port()).c_str(), String::from_utf8("s").unwrap()).await().is_ok());
  Vec<uint8_t> v;
  v.push('v');
  ASSERT_TRUE(client.post(url_for(server.port()).c_str(), std::move(v)).await().is_ok());
#if __cpp_lib_string_view
  // std::string_view (must outlive the call — keep the backing string)
  std::string      backing = url_for(server.port());
  std::string_view sv      = backing;
  ASSERT_TRUE(client.get(sv).await().is_ok());
#endif
}
