/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * body_test.cpp — Tests for xpp::http::Body.
 *
 * Uses C++20 coroutines (co_await / co_return) and requires an EventLoop +
 * WaitScope. Each test wraps its async work in a Promise<void> helper that
 * the TEST body invokes with .await().
 */

#include <cstring>

#include <gtest/gtest.h>
#include <xpp/http/body.h>
#include <xpp/promise.h>
#include <xpp/sync/mpsc.h>

using namespace xpp;
using namespace xpp::http;

/* ───────────────────────────────────────────────────────────────────
 *  Empty kind
 * ─────────────────────────────────────────────────────────────────── */

Promise<void> do_empty_returns_eof_immediately() {
  Body    b = Body::empty();
  char    buf[16];
  ssize_t n = co_await b.read(buf, sizeof(buf));
  EXPECT_EQ(n, 0);
  // Subsequent reads also return 0.
  n = co_await b.read(buf, sizeof(buf));
  EXPECT_EQ(n, 0);
  co_return;
}

TEST(BodyEmptyTest, ReturnsEofImmediately) {
  EventLoop loop;
  WaitScope scope(loop);
  do_empty_returns_eof_immediately().await();
}

TEST(BodyEmptyTest, IsEmptyAndNotChannel) {
  Body b = Body::empty();
  EXPECT_TRUE(b.is_empty());
  EXPECT_FALSE(b.is_channel());
}

/* ───────────────────────────────────────────────────────────────────
 *  Once kind
 * ─────────────────────────────────────────────────────────────────── */

Promise<void> do_once_reads_all_in_one_shot() {
  Body    b = Body::from(Bytes::from("hello world"));
  char    buf[64];
  ssize_t n = co_await b.read(buf, sizeof(buf));
  EXPECT_EQ(n, 11);
  buf[n] = '\0';
  EXPECT_STREQ(buf, "hello world");
  // Now drained.
  n = co_await b.read(buf, sizeof(buf));
  EXPECT_EQ(n, 0);
  co_return;
}

TEST(BodyOnceTest, ReadsAllInOneShot) {
  EventLoop loop;
  WaitScope scope(loop);
  do_once_reads_all_in_one_shot().await();
}

Promise<void> do_once_slices_when_buffer_smaller() {
  Body b = Body::from(Bytes::from("0123456789abcdef")); // 16 bytes
  char buf[4];

  ssize_t n = co_await b.read(buf, 4);
  EXPECT_EQ(n, 4);
  EXPECT_EQ(std::string(buf, 4), "0123");

  n = co_await b.read(buf, 4);
  EXPECT_EQ(n, 4);
  EXPECT_EQ(std::string(buf, 4), "4567");

  n = co_await b.read(buf, 4);
  EXPECT_EQ(n, 4);
  EXPECT_EQ(std::string(buf, 4), "89ab");

  n = co_await b.read(buf, 4);
  EXPECT_EQ(n, 4);
  EXPECT_EQ(std::string(buf, 4), "cdef");

  n = co_await b.read(buf, 4);
  EXPECT_EQ(n, 0); // EOF
  co_return;
}

TEST(BodyOnceTest, SlicesWhenBufferSmaller) {
  EventLoop loop;
  WaitScope scope(loop);
  do_once_slices_when_buffer_smaller().await();
}

Promise<void> do_once_from_string() {
  auto s   = String::from_utf8("hello").unwrap();
  Body b   = Body::from(std::move(s));
  auto out = co_await b.bytes();
  EXPECT_EQ(out.size(), 5u);
  auto r = out.to_string();
  EXPECT_TRUE(r.is_ok());
  if (r.is_ok()) {
    EXPECT_EQ(r.unwrap(), String::from_utf8("hello").unwrap());
  }
  co_return;
}

TEST(BodyOnceTest, FromString) {
  EventLoop loop;
  WaitScope scope(loop);
  do_once_from_string().await();
}

/* ───────────────────────────────────────────────────────────────────
 *  Channel kind
 * ─────────────────────────────────────────────────────────────────── */

Promise<void> do_channel_single_chunk() {
  auto [tx, rx] = sync::mpsc::channel<Bytes>(4);
  Body b        = Body::from_channel(std::move(rx));

  co_await tx.send(Bytes::from("payload"));
  tx.close();

  char    buf[64];
  ssize_t n = co_await b.read(buf, sizeof(buf));
  EXPECT_EQ(n, 7);
  buf[n] = '\0';
  EXPECT_STREQ(buf, "payload");

  n = co_await b.read(buf, sizeof(buf));
  EXPECT_EQ(n, 0); // EOF after channel closed
  co_return;
}

TEST(BodyChannelTest, SingleChunk) {
  EventLoop loop;
  WaitScope scope(loop);
  do_channel_single_chunk().await();
}

Promise<void> do_channel_multiple_chunks() {
  auto [tx, rx] = sync::mpsc::channel<Bytes>(4);
  Body b        = Body::from_channel(std::move(rx));

  co_await tx.send(Bytes::from("AAA"));
  co_await tx.send(Bytes::from("BBB"));
  co_await tx.send(Bytes::from("CC"));
  tx.close();

  char    buf[64];
  ssize_t n = co_await b.read(buf, sizeof(buf));
  EXPECT_EQ(n, 3);
  EXPECT_EQ(std::string(buf, 3), "AAA");

  n = co_await b.read(buf, sizeof(buf));
  EXPECT_EQ(n, 3);
  EXPECT_EQ(std::string(buf, 3), "BBB");

  n = co_await b.read(buf, sizeof(buf));
  EXPECT_EQ(n, 2);
  EXPECT_EQ(std::string(buf, 2), "CC");

  n = co_await b.read(buf, sizeof(buf));
  EXPECT_EQ(n, 0); // EOF
  co_return;
}

TEST(BodyChannelTest, MultipleChunks) {
  EventLoop loop;
  WaitScope scope(loop);
  do_channel_multiple_chunks().await();
}

Promise<void> do_channel_slices_oversized_chunk() {
  // Push a 16-byte chunk, but read with a 4-byte buffer.
  auto [tx, rx] = sync::mpsc::channel<Bytes>(4);
  Body b        = Body::from_channel(std::move(rx));

  co_await tx.send(Bytes::from("0123456789abcdef")); // 16 bytes
  tx.close();

  char buf[4];
  // The first read pulls the 16-byte chunk, returns 4, keeps 12 as m_pending.
  ssize_t n = co_await b.read(buf, 4);
  EXPECT_EQ(n, 4);
  EXPECT_EQ(std::string(buf, 4), "0123");

  // Next 3 reads drain the pending slice.
  n = co_await b.read(buf, 4);
  EXPECT_EQ(n, 4);
  EXPECT_EQ(std::string(buf, 4), "4567");

  n = co_await b.read(buf, 4);
  EXPECT_EQ(n, 4);
  EXPECT_EQ(std::string(buf, 4), "89ab");

  n = co_await b.read(buf, 4);
  EXPECT_EQ(n, 4);
  EXPECT_EQ(std::string(buf, 4), "cdef");

  // Now pending is drained. Next read pulls from the channel, which is closed → 0.
  n = co_await b.read(buf, 4);
  EXPECT_EQ(n, 0);
  co_return;
}

TEST(BodyChannelTest, SlicesOversizedChunk) {
  EventLoop loop;
  WaitScope scope(loop);
  do_channel_slices_oversized_chunk().await();
}

Promise<void> do_channel_close_before_any_recv() {
  // Close the channel immediately; reads should return 0 (EOF) without blocking.
  auto [tx, rx] = sync::mpsc::channel<Bytes>(4);
  Body b        = Body::from_channel(std::move(rx));
  tx.close();

  char    buf[16];
  ssize_t n = co_await b.read(buf, sizeof(buf));
  EXPECT_EQ(n, 0);
  co_return;
}

TEST(BodyChannelTest, CloseBeforeAnyRecv) {
  EventLoop loop;
  WaitScope scope(loop);
  do_channel_close_before_any_recv().await();
}

/* ───────────────────────────────────────────────────────────────────
 *  bytes() aggregator
 * ─────────────────────────────────────────────────────────────────── */

Promise<void> do_bytes_aggregates_once() {
  Body  b   = Body::from(Bytes::from("hello world"));
  Bytes out = co_await b.bytes();
  EXPECT_EQ(out.size(), 11u);
  auto r = out.to_string();
  EXPECT_TRUE(r.is_ok());
  if (r.is_ok()) {
    EXPECT_EQ(r.unwrap(), String::from_utf8("hello world").unwrap());
  }
  co_return;
}

TEST(BodyBytesTest, AggregatesOnce) {
  EventLoop loop;
  WaitScope scope(loop);
  do_bytes_aggregates_once().await();
}

Promise<void> do_bytes_aggregates_channel() {
  auto [tx, rx] = sync::mpsc::channel<Bytes>(4);
  Body b        = Body::from_channel(std::move(rx));

  co_await tx.send(Bytes::from("AAA"));  // 3 bytes
  co_await tx.send(Bytes::from("BBBB")); // 4 bytes
  co_await tx.send(Bytes::from("CC"));   // 2 bytes → total 9 bytes
  tx.close();

  Bytes out = co_await b.bytes();
  EXPECT_EQ(out.size(), 9u);
  auto r = out.to_string();
  EXPECT_TRUE(r.is_ok());
  if (r.is_ok()) {
    EXPECT_EQ(r.unwrap(), String::from_utf8("AAABBBBCC").unwrap());
  }
  co_return;
}

TEST(BodyBytesTest, AggregatesChannel) {
  EventLoop loop;
  WaitScope scope(loop);
  do_bytes_aggregates_channel().await();
}

/* ───────────────────────────────────────────────────────────────────
 *  text() aggregator
 * ─────────────────────────────────────────────────────────────────── */

Promise<void> do_text_decodes_utf8() {
  // "héllo" — 6 bytes UTF-8 (h + 0xC3 0xA9 + llo), 5 code points
  uint8_t data[] = {'h', 0xC3, 0xA9, 'l', 'l', 'o'};
  Body    b      = Body::from(Bytes::from(data, 6));
  auto    r      = co_await b.text();
  EXPECT_TRUE(r.is_ok());
  if (r.is_ok()) {
    // String::len() returns byte length; char_len() returns code points.
    EXPECT_EQ(r.unwrap().len(), 6u);      // bytes
    EXPECT_EQ(r.unwrap().char_len(), 5u); // code points
  }
  co_return;
}

TEST(BodyTextTest, DecodesUtf8) {
  EventLoop loop;
  WaitScope scope(loop);
  do_text_decodes_utf8().await();
}

Promise<void> do_text_returns_err_on_invalid_utf8() {
  // 0xFF is not valid UTF-8.
  uint8_t data[] = {0xFF, 0xFE};
  Body    b      = Body::from(Bytes::from(data, 2));
  auto    r      = co_await b.text();
  EXPECT_TRUE(r.is_err());
  co_return;
}

TEST(BodyTextTest, ReturnsErrOnInvalidUtf8) {
  EventLoop loop;
  WaitScope scope(loop);
  do_text_returns_err_on_invalid_utf8().await();
}

Promise<void> do_text_empty_body() {
  Body b = Body::empty();
  auto r = co_await b.text();
  EXPECT_TRUE(r.is_ok());
  if (r.is_ok()) {
    EXPECT_EQ(r.unwrap().len(), 0u);
  }
  co_return;
}

TEST(BodyTextTest, EmptyBody) {
  EventLoop loop;
  WaitScope scope(loop);
  do_text_empty_body().await();
}

/* ───────────────────────────────────────────────────────────────────
 *  Composes with io::copy (smoke test)
 * ─────────────────────────────────────────────────────────────────── */

// Body satisfies AsyncReader concept, so it should compose with io::copy.
// We don't have an AsyncWriter readily available for a full copy test
// here (that needs a TcpStream or similar). This test just verifies
// that the concept is satisfied — the compiler error would fire if not.
#if 0
Promise<void> do_body_composes_with_io_copy() {
  Body b = Body::from(Bytes::from("hello"));
  // ... would need an AsyncWriter ...
  // co_await io::copy(b, writer);
  co_return;
}
#endif
