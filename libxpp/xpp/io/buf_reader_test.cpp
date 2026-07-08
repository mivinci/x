/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * buf_reader_coroutine_test.cpp — Tests for xpp::io::BufReader.
 */
#include <gtest/gtest.h>
#include <xpp/io/buf_reader.h>
#include <xpp/io/utils.h>
#include <xpp/net/tcp.h>
#include <xpp/net/test_helpers.h>
#include <xpp/promise.h>
#include <xpp/promise_combinators.h>

using xpp::io::BufReader;
using xpp::net::TcpListener;
using xpp::net::TcpStream;

xpp::Promise<void> do_buf_reader_small() {
  uint16_t port = get_free_port();

  auto lr = co_await xpp::net::TcpListener::bind(("127.0.0.1:" + std::to_string(port)).c_str());
  TcpListener listener = std::move(lr).unwrap();

  auto server = listener.accept();
  auto client = xpp::net::TcpStream::connect(("127.0.0.1:" + std::to_string(port)).c_str());

  auto [sp, cr] = co_await xpp::all(std::move(server), std::move(client));
  TcpStream sc  = std::move(sp.first);
  TcpStream cc  = std::move(cr).unwrap();

  co_await sc.write("hello", 5);
  sc.close();

  BufReader<TcpStream> buf(std::move(cc));
  char                 c[5];
  ssize_t              n1 = co_await buf.read(c, 2);
  EXPECT_EQ(n1, 2);
  EXPECT_EQ(std::string(c, 2), "he");

  ssize_t n2 = co_await buf.read(c + 2, 3);
  EXPECT_EQ(n2, 3);
  EXPECT_EQ(std::string(c, 5), "hello");

  ssize_t n3 = co_await buf.read(c, 1);
  EXPECT_EQ(n3, 0);
  co_return;
}

TEST(BufReaderTest, SmallReads) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_buf_reader_small().await();
}

xpp::Promise<void> do_buf_reader_large_bypass() {
  uint16_t port = get_free_port();

  auto lr = co_await xpp::net::TcpListener::bind(("127.0.0.1:" + std::to_string(port)).c_str());
  TcpListener listener = std::move(lr).unwrap();

  auto server = listener.accept();
  auto client = xpp::net::TcpStream::connect(("127.0.0.1:" + std::to_string(port)).c_str());

  auto [sp, cr] = co_await xpp::all(std::move(server), std::move(client));
  TcpStream sc  = std::move(sp.first);
  TcpStream cc  = std::move(cr).unwrap();

  std::string big(10000, 'x');
  co_await sc.write(big.data(), big.size());
  sc.close();

  BufReader<TcpStream> buf(std::move(cc));
  auto                 data = co_await xpp::io::read_all(buf);

  EXPECT_EQ(data.size(), big.size());
  co_return;
}

TEST(BufReaderTest, LargeBypass) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_buf_reader_large_bypass().await();
}

xpp::Promise<void> do_buf_reader_with_read_all() {
  uint16_t port = get_free_port();

  auto lr = co_await xpp::net::TcpListener::bind(("127.0.0.1:" + std::to_string(port)).c_str());
  TcpListener listener = std::move(lr).unwrap();

  auto server = listener.accept();
  auto client = xpp::net::TcpStream::connect(("127.0.0.1:" + std::to_string(port)).c_str());

  auto [sp, cr] = co_await xpp::all(std::move(server), std::move(client));
  TcpStream sc  = std::move(sp.first);
  TcpStream cc  = std::move(cr).unwrap();

  co_await sc.write("hello world", 11);
  sc.close();

  BufReader<TcpStream> buf(std::move(cc));
  auto                 data = co_await xpp::io::read_all(buf);

  EXPECT_EQ(data.size(), 11u);
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(data.data()), 11), "hello world");
  co_return;
}

TEST(BufReaderTest, WithReadAll) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_buf_reader_with_read_all().await();
}
