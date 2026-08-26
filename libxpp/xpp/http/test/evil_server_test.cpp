/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * evil_server_test.cpp — Tests for xpp::http::test::EvilServer (Layer 2).
 */

#include <string>

#include <gtest/gtest.h>
#include <xpp/event.h>
#include <xpp/http/client.h>
#include <xpp/http/test/evil_server.h>

using namespace xpp;
using namespace xpp::http;

static std::string url_of(const test::EvilServer &es, const char *path = "") {
  return "http://127.0.0.1:" + std::to_string(es.port()) + path;
}

TEST(EvilServerTest, DeclaredBodyTruncatedMidTransfer) {
  EventLoop loop;
  WaitScope scope(loop);

  // Declare 1MB, send only 64KB then hard-close. The client must surface
  // this as a body-read error, not a silent truncated EOF.
  test::EvilSpec spec;
  spec.body      = Bytes::copy(std::string(1024 * 1024, 'z').c_str(), 1024 * 1024);
  spec.send_only = 64 * 1024;
  test::EvilServer evil(spec);
  EXPECT_GT(evil.port(), 0);

  auto client_r = Client::builder().build();
  ASSERT_TRUE(client_r.is_ok());
  Client client = std::move(client_r).unwrap();

  auto r = client.get(url_of(evil).c_str()).await();
  ASSERT_TRUE(r.is_ok()) << "send should succeed (headers arrived)";

  auto body = r.unwrap().bytes().await();
  ASSERT_TRUE(body.is_err()) << "truncated transfer must not look like clean EOF";
}
