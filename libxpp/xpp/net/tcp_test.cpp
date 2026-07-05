/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * tcp_test.cpp — Tests for xpp::net::TcpStream and TcpListener.
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

#include <x/base/event.h>

using xpp::net::SocketAddr;
using xpp::net::TcpListener;
using xpp::net::TcpStream;

/* ── Helpers ──────────────────────────────────────────────────────── */

/* ── TcpStream ──────────────────────────────────────────────────────── */

TEST(TcpConnTest, ConnectFailure) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  // Port 1 on loopback: ECONNREFUSED (no listener, port < 1024 needs root)
  auto result = TcpStream::connect("127.0.0.1", 1).wait();
  ASSERT_TRUE(result.is_err());
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

  /* Phase 1 — Connect + accept independently.
   * Sequential .wait() calls avoid the all() combinator, whose
   * interaction with complex send/recv chains is known to hang on
   * Linux shared-build + ASAN CI (edge-triggered epoll ordering). */
  TcpStream server_conn;
  auto      accept_p =
    listener.accept().then([&server_conn](auto p) { server_conn = std::move(p.first); });

  auto connect_r = TcpStream::connect("127.0.0.1", port).wait();
  ASSERT_TRUE(connect_r.is_ok());
  TcpStream client_conn = std::move(connect_r).unwrap();

  accept_p.wait();
  ASSERT_TRUE(server_conn.is_open());
  ASSERT_TRUE(client_conn.is_open());

  /* Phase 2 — Send / recv (sequential, no concurrent chains). */
  auto        client_buf = std::make_shared<std::vector<char>>(64);
  auto        server_buf = std::make_shared<std::vector<char>>(64);
  const char *msg        = "hello";

  // Client send
  ssize_t sent = client_conn.write(msg, 5).wait();
  EXPECT_EQ(sent, 5);

  // Server recv
  ssize_t recvd = server_conn.read(server_buf->data(), server_buf->size()).wait();
  EXPECT_EQ(recvd, 5);
  EXPECT_EQ(std::string(server_buf->data(), static_cast<size_t>(recvd)), "hello");

  // Server echo back
  ssize_t echoed = server_conn.write(server_buf->data(), static_cast<size_t>(recvd)).wait();
  EXPECT_EQ(echoed, 5);

  // Client recv echo
  ssize_t echoed_recvd = client_conn.read(client_buf->data(), client_buf->size()).wait();
  EXPECT_EQ(echoed_recvd, 5);
  EXPECT_EQ(std::string(client_buf->data(), static_cast<size_t>(echoed_recvd)), "hello");
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
  auto accept_p = listener.accept().then([&server_peer](auto p) { server_peer = p.second; });

  TcpStream client;
  auto      connect_p = TcpStream::connect("127.0.0.1", port).then([&client](auto r) {
    ASSERT_TRUE(r.is_ok());
    client = std::move(r).unwrap();
  });

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
  auto accept_p = listener.accept().then([&accepted](auto p) { accepted = p.first.is_open(); });

  auto connect_p = TcpStream::connect("127.0.0.1", port).then([](auto) {});

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
  auto p1 = listener.accept().then([&accept_count](auto) { accept_count++; });
  auto c1 = TcpStream::connect("127.0.0.1", port);
  c1.then([](auto) {}).wait();
  p1.wait();
  ASSERT_EQ(accept_count, 1);

  // Second accept + connect
  auto p2 = listener.accept().then([&accept_count](auto) { accept_count++; });
  auto c2 = TcpStream::connect("127.0.0.1", port);
  c2.then([](auto) {}).wait();
  p2.wait();
  EXPECT_EQ(accept_count, 2);
}
