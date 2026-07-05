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

  auto sock_r = UdpSocket::bind(("127.0.0.1:" + std::to_string(port)).c_str()).wait();
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

  auto server_r = UdpSocket::bind(("127.0.0.1:" + std::to_string(server_port)).c_str()).wait();
  ASSERT_TRUE(server_r.is_ok());
  UdpSocket server = std::move(server_r).unwrap();
  ASSERT_TRUE(server.is_open());

  auto client_r = UdpSocket::bind("127.0.0.1:0").wait();
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

  auto recv_result = recv_p.wait();
  send_p.wait();

  EXPECT_EQ(recv_result.first, 9);
  EXPECT_EQ(std::string(server_buf->data(), static_cast<size_t>(recv_result.first)), "udp-hello");
}

TEST(UdpSocketTest, RecvFromReturnsPeerAddr) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  uint16_t server_port = get_free_port();
  ASSERT_GT(server_port, 0);

  auto server_r = UdpSocket::bind(("127.0.0.1:" + std::to_string(server_port)).c_str()).wait();
  ASSERT_TRUE(server_r.is_ok());
  UdpSocket server = std::move(server_r).unwrap();
  ASSERT_TRUE(server.is_open());

  auto client_r = UdpSocket::bind("127.0.0.1:0").wait();
  ASSERT_TRUE(client_r.is_ok());
  UdpSocket client = std::move(client_r).unwrap();
  ASSERT_TRUE(client.is_open());

  auto server_addr = server.local_addr().unwrap();
  auto client_addr = client.local_addr().unwrap();

  auto server_buf = std::make_shared<std::vector<char>>(64);

  auto recv_p = server.recv_from(server_buf->data(), server_buf->size());
  auto send_p = client.send_to("ping", 4, server_addr);

  auto recv_result = recv_p.wait();
  send_p.wait();

  EXPECT_EQ(recv_result.first, 4);
  EXPECT_EQ(recv_result.second, client_addr);
}
