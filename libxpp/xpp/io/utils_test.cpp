/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * io_util_coroutine_test.cpp — Tests for xpp::io::read_all and copy.
 */
#include <gtest/gtest.h>
#include <xpp/fs/file.h>
#include <xpp/io/utils.h>
#include <xpp/net/tcp.h>
#include <xpp/net/test_helpers.h>
#include <xpp/promise.h>
#include <xpp/promise_combinators.h>

using xpp::net::TcpListener;
using xpp::net::TcpStream;

/* ── read_all over TCP ────────────────────────────────────────────── */

xpp::Promise<void> do_read_all_tcp() {
  uint16_t port = get_free_port();

  auto listener_r =
    co_await xpp::net::TcpListener::bind(("127.0.0.1:" + std::to_string(port)).c_str());
  TcpListener listener = std::move(listener_r).unwrap();

  auto server = listener.accept();
  auto client = xpp::net::TcpStream::connect(("127.0.0.1:" + std::to_string(port)).c_str());

  auto [server_pair, conn_r] = co_await xpp::all(std::move(server), std::move(client));
  TcpStream server_conn      = std::move(server_pair.first);
  TcpStream client_conn      = std::move(conn_r).unwrap();

  co_await server_conn.write("hello", 5);
  server_conn.close();

  auto data = co_await xpp::io::read_all(client_conn);
  EXPECT_EQ(data.size(), 5u);
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(data.data()), 5), "hello");
  co_return;
}

TEST(IoUtilCoroutineTest, ReadAllTcp) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_read_all_tcp().await();
}

/* ── copy TCP → File ──────────────────────────────────────────────── */

xpp::Promise<void> do_copy_tcp_to_file() {
  uint16_t port = get_free_port();

  auto listener_r =
    co_await xpp::net::TcpListener::bind(("127.0.0.1:" + std::to_string(port)).c_str());
  TcpListener listener = std::move(listener_r).unwrap();

  auto server = listener.accept();
  auto client = xpp::net::TcpStream::connect(("127.0.0.1:" + std::to_string(port)).c_str());

  auto [server_pair, conn_r] = co_await xpp::all(std::move(server), std::move(client));
  TcpStream server_conn      = std::move(server_pair.first);
  TcpStream client_conn      = std::move(conn_r).unwrap();

  co_await server_conn.write("hello", 5);
  server_conn.close();

  auto tmpfile = "/tmp/xpp_io_copy_test_" + std::to_string(port);
  auto f       = co_await xpp::fs::File::create(tmpfile.c_str());
  co_await xpp::io::copy(client_conn, f);

  // Verify
  f         = co_await xpp::fs::File::open(tmpfile.c_str());
  auto data = co_await f.read_all();
  EXPECT_EQ(data.size(), 5u);
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(data.data()), 5), "hello");
  ::unlink(tmpfile.c_str());
  co_return;
}

TEST(IoUtilCoroutineTest, CopyTcpToFile) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_copy_tcp_to_file().await();
}

/* ── File cursor read ─────────────────────────────────────────────── */

xpp::Promise<void> do_file_cursor_read() {
  auto f = co_await xpp::fs::File::create("/tmp/xpp_cursor_test.bin");
  co_await f.write_all("hello", 5);

  f = co_await xpp::fs::File::open("/tmp/xpp_cursor_test.bin");

  char    buf[3];
  ssize_t n1 = co_await f.read(buf, 3);
  EXPECT_EQ(n1, 3);
  EXPECT_EQ(std::string(buf, 3), "hel");

  ssize_t n2 = co_await f.read(buf, 3);
  EXPECT_EQ(n2, 2);
  EXPECT_EQ(std::string(buf, 2), "lo");

  ssize_t n3 = co_await f.read(buf, 1);
  EXPECT_EQ(n3, 0);

  ::unlink("/tmp/xpp_cursor_test.bin");
  co_return;
}

TEST(IoUtilCoroutineTest, FileCursorRead) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);
  do_file_cursor_read().await();
}
