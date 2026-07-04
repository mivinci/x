/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tcp_test.cpp — Tests for xpp::net::TcpConn and TcpListener.
 */

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <xpp/net/addr.h>
#include <xpp/net/tcp.h>
#include <xpp/net/test_helpers.h>
#include <xpp/promise.h>
#include <xpp/promise_combinators.h>

#include <x/base/event.h>

using xpp::net::SocketAddr;
using xpp::net::TcpConn;
using xpp::net::TcpListener;

/* ── TcpConn ──────────────────────────────────────────────────────── */

TEST(TcpConnTest, ConnectFailure) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  // Port 1 on loopback: ECONNREFUSED (no listener, port < 1024 needs root)
  auto conn = TcpConn::connect("127.0.0.1", 1).wait();
  EXPECT_FALSE(conn.is_open());
}

TEST(TcpConnTest, ConnectAndSendRecv) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  uint16_t port = get_free_port();
  ASSERT_GT(port, 0);

  auto listener_r = TcpListener::bind(("127.0.0.1:" + std::to_string(port)).c_str()).wait();
  ASSERT_TRUE(listener_r.is_ok());
  TcpListener listener = std::move(listener_r).unwrap();
  ASSERT_TRUE(listener.is_open());

  // Single chain: accept → recv → echo, then connect → send → recv → verify.
  // The connect is started first (async), then we wait on the server chain
  // which drives both sides via the shared event loop.
  auto client_buf = std::make_shared<std::vector<char>>(64);
  auto server_buf = std::make_shared<std::vector<char>>(64);

  // Start the client connect (in-flight, not waited on yet).
  auto client_p = TcpConn::connect("127.0.0.1", port).then([client_buf, server_buf](TcpConn c) {
    auto        conn = std::make_shared<TcpConn>(std::move(c));
    const char *msg  = "hello";
    return conn->send(msg, 5)
      .then([conn, client_buf](ssize_t) mutable {
        return conn->recv(client_buf->data(), client_buf->size());
      })
      .then([conn, client_buf](ssize_t n) mutable {
        EXPECT_EQ(n, 5);
        EXPECT_EQ(std::string(client_buf->data(), static_cast<size_t>(n)), "hello");
      });
  });

  // Server chain: accept → recv → echo.
  auto server_p = listener.accept().then([server_buf](TcpConn c) {
    auto conn = std::make_shared<TcpConn>(std::move(c));
    return conn->recv(server_buf->data(), server_buf->size())
      .then([conn, server_buf](ssize_t n) mutable {
        return conn->send(server_buf->data(), static_cast<size_t>(n));
      })
      .then([conn](ssize_t) mutable {});
  });

  // Poll both chains concurrently — all() drives the event loop for both.
  xpp::all(std::move(server_p), std::move(client_p)).wait();
}

TEST(TcpConnTest, PeerAndLocalAddr) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  uint16_t port = get_free_port();
  ASSERT_GT(port, 0);

  auto listener_r = TcpListener::bind(("127.0.0.1:" + std::to_string(port)).c_str()).wait();
  ASSERT_TRUE(listener_r.is_ok());
  TcpListener listener = std::move(listener_r).unwrap();
  ASSERT_TRUE(listener.is_open());

  xpp::Option<SocketAddr> server_peer;
  auto                    accept_p =
    listener.accept().then([&server_peer](TcpConn c) { server_peer = c.peer_addr(); });

  TcpConn client;
  auto    connect_p =
    TcpConn::connect("127.0.0.1", port).then([&client](TcpConn c) { client = std::move(c); });

  connect_p.wait();
  accept_p.wait();

  ASSERT_TRUE(client.is_open());
  auto client_local = client.local_addr();
  auto client_peer  = client.peer_addr();
  ASSERT_TRUE(client_local.is_some());
  ASSERT_TRUE(client_peer.is_some());
  ASSERT_TRUE(server_peer.is_some());

  EXPECT_EQ(client_peer.unwrap().port(), port);
  EXPECT_EQ(server_peer.unwrap().port(), client_local.unwrap().port());
}

/* ── TcpListener ──────────────────────────────────────────────────── */

TEST(TcpListenerTest, BindAndAccept) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  uint16_t port = get_free_port();
  ASSERT_GT(port, 0);

  auto listener_r = TcpListener::bind(("127.0.0.1:" + std::to_string(port)).c_str()).wait();
  ASSERT_TRUE(listener_r.is_ok());
  TcpListener listener = std::move(listener_r).unwrap();
  ASSERT_TRUE(listener.is_open());

  bool accepted = false;
  auto accept_p = listener.accept().then([&accepted](TcpConn c) { accepted = c.is_open(); });

  auto connect_p = TcpConn::connect("127.0.0.1", port).then([](TcpConn) {});

  connect_p.wait();
  accept_p.wait();
  EXPECT_TRUE(accepted);
}

TEST(TcpListenerTest, SequentialAccept) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  uint16_t port = get_free_port();
  ASSERT_GT(port, 0);

  auto listener_r = TcpListener::bind(("127.0.0.1:" + std::to_string(port)).c_str()).wait();
  ASSERT_TRUE(listener_r.is_ok());
  TcpListener listener = std::move(listener_r).unwrap();
  ASSERT_TRUE(listener.is_open());

  int accept_count = 0;

  // First accept + connect
  auto p1 = listener.accept().then([&accept_count](TcpConn) { accept_count++; });
  auto c1 = TcpConn::connect("127.0.0.1", port);
  c1.wait();
  p1.wait();
  ASSERT_EQ(accept_count, 1);

  // Second accept + connect
  auto p2 = listener.accept().then([&accept_count](TcpConn) { accept_count++; });
  auto c2 = TcpConn::connect("127.0.0.1", port);
  c2.wait();
  p2.wait();
  EXPECT_EQ(accept_count, 2);
}
