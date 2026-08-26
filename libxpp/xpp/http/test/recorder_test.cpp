/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * recorder_test.cpp — Tests for xpp::http::test::Recorder.
 */

#include <string>

#include <gtest/gtest.h>
#include <xpp/event.h>
#include <xpp/http/client.h>
#include <xpp/http/test/recorder.h>
#include <xpp/http/test/server.h>

using namespace xpp;
using namespace xpp::http;

static std::string url_of(const test::Server &ts, const char *path = "") {
  return "http://127.0.0.1:" + std::to_string(ts.port()) + path;
}

TEST(RecorderTest, RecordsMethodAndPath) {
  EventLoop loop;
  WaitScope scope(loop);

  test::Recorder rec([](Request) { return Response::ok("ok"); });
  test::Server   ts(rec.handler());

  auto client = Client::builder().build().unwrap();
  client.get(url_of(ts, "/a").c_str()).await();
  client.post(url_of(ts, "/b").c_str(), "x").await();

  EXPECT_EQ(rec.count(), 2);
  auto first = rec.at(0);
  ASSERT_TRUE(first.is_some());
  EXPECT_EQ(first.unwrap().method, String::from_utf8("GET").unwrap());
  EXPECT_EQ(first.unwrap().path, String::from_utf8("/a").unwrap());

  auto second = rec.at(1);
  ASSERT_TRUE(second.is_some());
  EXPECT_EQ(second.unwrap().method, String::from_utf8("POST").unwrap());
  EXPECT_EQ(second.unwrap().path, String::from_utf8("/b").unwrap());
}

TEST(RecorderTest, QueryStrippedFromPath) {
  EventLoop loop;
  WaitScope scope(loop);

  test::Recorder rec([](Request) { return Response::ok("ok"); });
  test::Server   ts(rec.handler());

  auto client = Client::builder().build().unwrap();
  client.get(url_of(ts, "/search?q=hello&x=1").c_str()).await();

  EXPECT_EQ(rec.count(), 1);
  auto req = rec.at(0);
  ASSERT_TRUE(req.is_some());
  EXPECT_EQ(req.unwrap().path, String::from_utf8("/search").unwrap());
}

TEST(RecorderTest, OutOfRangeReturnsNone) {
  EventLoop loop;
  WaitScope scope(loop);

  test::Recorder rec([](Request) { return Response::ok("ok"); });
  test::Server   ts(rec.handler());

  auto client = Client::builder().build().unwrap();
  client.get(url_of(ts).c_str()).await();

  EXPECT_EQ(rec.count(), 1);
  EXPECT_TRUE(rec.at(1).is_none());
  EXPECT_TRUE(rec.at(99).is_none());
}

TEST(RecorderTest, ClearResets) {
  EventLoop loop;
  WaitScope scope(loop);

  test::Recorder rec([](Request) { return Response::ok("ok"); });
  test::Server   ts(rec.handler());

  auto client = Client::builder().build().unwrap();
  client.get(url_of(ts, "/first").c_str()).await();
  rec.clear();
  EXPECT_EQ(rec.count(), 0);
}
