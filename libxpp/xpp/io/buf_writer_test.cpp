/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * buf_writer_coroutine_test.cpp — Tests for xpp::io::BufWriter.
 */
#include <gtest/gtest.h>
#include <xpp/io/buf_writer.h>
#include <xpp/io/utils.h>
#include <xpp/net/tcp.h>
#include <xpp/net/test_helpers.h>
#include <xpp/promise.h>
#include <xpp/promise_combinators.h>

using xpp::io::BufWriter;
using xpp::net::TcpListener;
using xpp::net::TcpStream;

xpp::Promise<void> do_buf_writer_buffered() {
  uint16_t port = get_free_port();

  auto lr = co_await xpp::net::TcpListener::bind(("127.0.0.1:" + std::to_string(port)).c_str());
  TcpListener listener = std::move(lr).unwrap();

  auto server = listener.accept();
  auto client = xpp::net::TcpStream::connect(("127.0.0.1:" + std::to_string(port)).c_str());

  auto [sp, cr] = co_await xpp::all(std::move(server), std::move(client));
  TcpStream sc  = std::move(sp.first);
  TcpStream cc  = std::move(cr).unwrap();

  BufWriter<TcpStream> buf(std::move(cc));
  co_await buf.write("he", 2);
  co_await buf.write("llo", 3);
  co_await buf.flush();

  char    result[8] = {};
  ssize_t n1        = co_await sc.read(result, 5);
  EXPECT_EQ(n1, 5);
  EXPECT_EQ(std::string(result, 5), "hello");
  co_return;
}

TEST(BufWriterTest, Buffered) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_buf_writer_buffered().wait();
}

xpp::Promise<void> do_buf_writer_large_bypass() {
  uint16_t port = get_free_port();

  auto lr = co_await xpp::net::TcpListener::bind(("127.0.0.1:" + std::to_string(port)).c_str());
  TcpListener listener = std::move(lr).unwrap();

  auto server = listener.accept();
  auto client = xpp::net::TcpStream::connect(("127.0.0.1:" + std::to_string(port)).c_str());

  auto [sp, cr] = co_await xpp::all(std::move(server), std::move(client));
  TcpStream sc  = std::move(sp.first);
  TcpStream cc  = std::move(cr).unwrap();

  std::string          big(10000, 'y');
  BufWriter<TcpStream> buf(std::move(cc));
  co_await buf.write(big.data(), big.size());
  co_await buf.flush();
  buf.inner().close(); // signal EOF to reader

  auto data = co_await xpp::io::read_all(sc);
  EXPECT_EQ(data.size(), big.size());
  co_return;
}

TEST(BufWriterTest, LargeBypass) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_buf_writer_large_bypass().wait();
}

xpp::Promise<void> do_buf_writer_auto_flush() {
  uint16_t port = get_free_port();

  auto lr = co_await xpp::net::TcpListener::bind(("127.0.0.1:" + std::to_string(port)).c_str());
  TcpListener listener = std::move(lr).unwrap();

  auto server = listener.accept();
  auto client = xpp::net::TcpStream::connect(("127.0.0.1:" + std::to_string(port)).c_str());

  auto [sp, cr] = co_await xpp::all(std::move(server), std::move(client));
  TcpStream sc  = std::move(sp.first);
  TcpStream cc  = std::move(cr).unwrap();

  // Fill buffer until auto-flush triggers
  std::string          data(9000, 'A');
  BufWriter<TcpStream> buf(std::move(cc));
  co_await buf.write(data.data(), data.size());
  // Buffer is full (9000 > 8192), auto-flushed during write
  co_await buf.flush(); // flush remaining
  buf.inner().close();

  auto result = co_await xpp::io::read_all(sc);
  EXPECT_EQ(result.size(), data.size());
  co_return;
}

TEST(BufWriterTest, AutoFlush) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_buf_writer_auto_flush().wait();
}
