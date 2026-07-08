/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * udp_test.cpp — Tests for xpp::net::UdpSocket.
 */

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <xpp/net/addr.h>
#include <xpp/net/test_helpers.h>
#include <xpp/net/udp.h>
#include <xpp/promise.h>

#include <x/base/event.h>

using xpp::net::UdpSocket;

TEST(UdpSocketTest, BindAndIsOpen) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  uint16_t port = get_free_port();
  ASSERT_GT(port, 0);

  auto sock_r = UdpSocket::bind(("127.0.0.1:" + std::to_string(port)).c_str()).await();
  ASSERT_TRUE(sock_r.is_ok());
  UdpSocket sock = std::move(sock_r).unwrap();
  EXPECT_TRUE(sock.is_open());
  EXPECT_GE(sock.fd(), 0);
}

TEST(UdpSocketTest, LoopbackSendRecv) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  uint16_t server_port = get_free_port();
  ASSERT_GT(server_port, 0);

  auto server_r = UdpSocket::bind(("127.0.0.1:" + std::to_string(server_port)).c_str()).await();
  ASSERT_TRUE(server_r.is_ok());
  UdpSocket server = std::move(server_r).unwrap();
  ASSERT_TRUE(server.is_open());

  auto client_r = UdpSocket::bind("127.0.0.1:0").await();
  ASSERT_TRUE(client_r.is_ok());
  UdpSocket client = std::move(client_r).unwrap();
  ASSERT_TRUE(client.is_open());

  auto server_addr = server.local_addr();
  auto client_addr = client.local_addr();
  ASSERT_TRUE(server_addr.is_some());
  ASSERT_TRUE(client_addr.is_some());

  auto server_buf = std::make_shared<std::vector<char>>(64);

  // Server: recv_from
  auto recv_p = server.recv_from(server_buf->data(), server_buf->size());

  // Client: send_to server
  const char *msg    = "udp-hello";
  auto        send_p = client.send_to(msg, 9, server_addr.unwrap());

  auto recv_result = recv_p.await();
  send_p.await();

  EXPECT_EQ(recv_result.first, 9);
  EXPECT_EQ(std::string(server_buf->data(), static_cast<size_t>(recv_result.first)), "udp-hello");
}

TEST(UdpSocketTest, RecvFromReturnsPeerAddr) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  uint16_t server_port = get_free_port();
  ASSERT_GT(server_port, 0);

  auto server_r = UdpSocket::bind(("127.0.0.1:" + std::to_string(server_port)).c_str()).await();
  ASSERT_TRUE(server_r.is_ok());
  UdpSocket server = std::move(server_r).unwrap();
  ASSERT_TRUE(server.is_open());

  auto client_r = UdpSocket::bind("127.0.0.1:0").await();
  ASSERT_TRUE(client_r.is_ok());
  UdpSocket client = std::move(client_r).unwrap();
  ASSERT_TRUE(client.is_open());

  auto server_addr = server.local_addr().unwrap();
  auto client_addr = client.local_addr().unwrap();

  auto server_buf = std::make_shared<std::vector<char>>(64);

  auto recv_p = server.recv_from(server_buf->data(), server_buf->size());
  auto send_p = client.send_to("ping", 4, server_addr);

  auto recv_result = recv_p.await();
  send_p.await();

  EXPECT_EQ(recv_result.first, 4);
  EXPECT_EQ(recv_result.second, client_addr);
}

/* ── Connected mode ────────────────────────────────────────────────── */

TEST(UdpSocketTest, ConnectAndRecvSend) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  uint16_t server_port = get_free_port();
  ASSERT_GT(server_port, 0);

  auto server_r = UdpSocket::bind(("127.0.0.1:" + std::to_string(server_port)).c_str()).await();
  ASSERT_TRUE(server_r.is_ok());
  UdpSocket server = std::move(server_r).unwrap();
  ASSERT_TRUE(server.is_open());

  auto client_r = UdpSocket::bind("127.0.0.1:0").await();
  ASSERT_TRUE(client_r.is_ok());
  UdpSocket client = std::move(client_r).unwrap();
  ASSERT_TRUE(client.is_open());

  client.connect(server.local_addr().unwrap());
  EXPECT_TRUE(client.peer_addr().is_some());

  // Send from client (connected) to server
  auto send_p = client.send("hello", 5);

  auto server_buf = std::make_shared<std::vector<char>>(64);
  auto recv_p     = server.recv_from(server_buf->data(), server_buf->size());

  auto recv_result = recv_p.await();
  send_p.await();

  EXPECT_EQ(recv_result.first, 5);
  EXPECT_EQ(std::string(server_buf->data(), static_cast<size_t>(recv_result.first)), "hello");
}

/* ── try_* methods ─────────────────────────────────────────────────── */

TEST(UdpSocketTest, TryRecvFromNoData) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto sock_r = UdpSocket::bind("127.0.0.1:0").await();
  ASSERT_TRUE(sock_r.is_ok());
  UdpSocket sock = std::move(sock_r).unwrap();

  char buf[16];
  auto [n, addr] = sock.try_recv_from(buf, sizeof(buf));
  EXPECT_LT(n, 0); // no data available
  EXPECT_TRUE(addr.is_none());
}

TEST(UdpSocketTest, TrySendTo) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto sock_r = UdpSocket::bind("127.0.0.1:0").await();
  ASSERT_TRUE(sock_r.is_ok());
  UdpSocket sock = std::move(sock_r).unwrap();

  auto    addr = sock.local_addr().unwrap();
  ssize_t n    = sock.try_send_to("hi", 2, addr);
  EXPECT_GT(n, 0);
}

/* ── Socket options ────────────────────────────────────────────────── */

TEST(UdpSocketTest, Broadcast) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto sock_r = UdpSocket::bind("127.0.0.1:0").await();
  ASSERT_TRUE(sock_r.is_ok());
  UdpSocket sock = std::move(sock_r).unwrap();

  auto r = sock.set_broadcast(true);
  EXPECT_TRUE(r.is_ok());

  auto b = sock.broadcast();
  EXPECT_TRUE(b.is_ok());
  EXPECT_TRUE(b.unwrap());
}

TEST(UdpSocketTest, Ttl) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto sock_r = UdpSocket::bind("127.0.0.1:0").await();
  ASSERT_TRUE(sock_r.is_ok());
  UdpSocket sock = std::move(sock_r).unwrap();

  auto r = sock.set_ttl(64);
  EXPECT_TRUE(r.is_ok());

  auto t = sock.ttl();
  EXPECT_TRUE(t.is_ok());
  EXPECT_EQ(t.unwrap(), 64u);
}

TEST(UdpSocketTest, TakeError) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto sock_r = UdpSocket::bind("127.0.0.1:0").await();
  ASSERT_TRUE(sock_r.is_ok());
  UdpSocket sock = std::move(sock_r).unwrap();

  int err = sock.take_error();
  EXPECT_EQ(err, 0);
}

/* ── Readiness ─────────────────────────────────────────────────────── */

TEST(UdpSocketTest, ReadableWritable) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  auto sock_r = UdpSocket::bind("127.0.0.1:0").await();
  ASSERT_TRUE(sock_r.is_ok());
  UdpSocket sock = std::move(sock_r).unwrap();

  // Writable should be immediately ready for a fresh socket
  sock.writable().await();
  SUCCEED();
}
