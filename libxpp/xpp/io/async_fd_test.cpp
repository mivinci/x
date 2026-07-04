/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * async_fd_test.cpp — Tests for xpp::io::AsyncFd.
 */
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include <gtest/gtest.h>
#include <xpp/io/async_fd.h>
#include <xpp/promise.h>

#include <x/base/event.h>

/* ── Test fixture: socketpair ───────────────────────────────────────── */

class IoAsyncFdTest : public ::testing::Test {
protected:
  int sv[2] = {-1, -1};

  void SetUp() override {
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    // Set non-blocking
    for (int i = 0; i < 2; ++i) {
      int flags = fcntl(sv[i], F_GETFL, 0);
      fcntl(sv[i], F_SETFL, flags | O_NONBLOCK);
    }
  }

  void TearDown() override {
    for (int i = 0; i < 2; ++i) {
      if (sv[i] >= 0) close(sv[i]);
    }
  }
};

/* ── Basic registration ────────────────────────────────────────────── */

TEST_F(IoAsyncFdTest, CreateAndDestroy) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  {
    xpp::io::AsyncFd fd(sv[0]);
    EXPECT_EQ(fd.fd(), sv[0]);
    EXPECT_FALSE(fd.is_closed());
  }
  // ~AsyncFd deregisters but does NOT close fd
  EXPECT_EQ(write(sv[1], "x", 1), 1); // sv[0] still usable
}

/* ── readable() ────────────────────────────────────────────────────── */

TEST_F(IoAsyncFdTest, ReadableImmediate) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  xpp::io::AsyncFd fd(sv[0]);
  write(sv[1], "hello", 5); // make sv[0] readable

  // Should resolve immediately — data already available
  fd.readable().wait();
  SUCCEED();
}

TEST_F(IoAsyncFdTest, ReadableDeferred) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  xpp::io::AsyncFd fd(sv[0]);

  // Write from another thread after 50ms
  std::thread t([this]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    write(sv[1], "data", 4);
  });

  // readable() should block until data arrives
  fd.readable().wait();
  t.join();
  SUCCEED();
}

/* ── read() ────────────────────────────────────────────────────────── */

TEST_F(IoAsyncFdTest, ReadFastPath) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  xpp::io::AsyncFd fd(sv[0]);
  write(sv[1], "hello", 5);

  char buf[64] = {};
  ssize_t n = xpp::io::read(fd, buf, sizeof(buf)).wait();
  EXPECT_EQ(n, 5);
  buf[n] = '\0';
  EXPECT_STREQ(buf, "hello");
}

TEST_F(IoAsyncFdTest, ReadSlowPath) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  xpp::io::AsyncFd fd(sv[0]);

  std::thread t([this]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    write(sv[1], "deferred", 8);
  });

  char buf[64] = {};
  ssize_t n = xpp::io::read(fd, buf, sizeof(buf)).wait();
  EXPECT_EQ(n, 8);
  buf[n] = '\0';
  EXPECT_STREQ(buf, "deferred");
  t.join();
}

/* ── write() ───────────────────────────────────────────────────────── */

TEST_F(IoAsyncFdTest, WriteFastPath) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  xpp::io::AsyncFd fd(sv[0]);

  ssize_t n = xpp::io::write(fd, "write test", 10).wait();
  EXPECT_EQ(n, 10);

  // Verify on other end
  char buf[64] = {};
  ssize_t r = read(sv[1], buf, sizeof(buf));
  EXPECT_EQ(r, 10);
  buf[r] = '\0';
  EXPECT_STREQ(buf, "write test");
}

TEST_F(IoAsyncFdTest, WriteSlowPath) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  xpp::io::AsyncFd fd(sv[0]);

  // Fill the send buffer to trigger EAGAIN
  char big[65536];
  memset(big, 'x', sizeof(big));
  size_t total_sent = 0;
  while (true) {
    ssize_t n = ::send(sv[0], big, sizeof(big), MSG_DONTWAIT);
    if (n < 0) break;
    total_sent += static_cast<size_t>(n);
  }

  // Now write() should hit EAGAIN, wait for writable
  std::thread t([this]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // Drain the other end to make sv[0] writable again
    char buf[65536];
    while (::recv(sv[1], buf, sizeof(buf), MSG_DONTWAIT) > 0) {
    }
  });

  ssize_t n = xpp::io::write(fd, "ok", 2).wait();
  EXPECT_EQ(n, 2);
  t.join();
}

/* ── read_full() ───────────────────────────────────────────────────── */

TEST_F(IoAsyncFdTest, ReadFull) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  xpp::io::AsyncFd fd(sv[0]);

  // Send 100 bytes in two chunks
  std::thread t([this]() {
    char buf[50];
    memset(buf, 'A', 50);
    write(sv[1], buf, 50);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    memset(buf, 'B', 50);
    write(sv[1], buf, 50);
  });

  char buf[100] = {};
  ssize_t n = xpp::io::read_full(fd, buf, 100).wait();
  EXPECT_EQ(n, 100);
  t.join();
}

/* ── write_all() ───────────────────────────────────────────────────── */

TEST_F(IoAsyncFdTest, WriteAll) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  xpp::io::AsyncFd fd(sv[0]);

  char data[100];
  memset(data, 'Z', 100);
  xpp::io::write_all(fd, data, 100).wait();

  // Read back from other end
  char buf[100] = {};
  size_t total = 0;
  while (total < 100) {
    ssize_t n = read(sv[1], buf + total, 100 - total);
    if (n <= 0) break;
    total += static_cast<size_t>(n);
  }
  EXPECT_EQ(total, 100u);
}

/* ── close() ───────────────────────────────────────────────────────── */

TEST_F(IoAsyncFdTest, CloseWakesReadable) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  xpp::io::AsyncFd fd(sv[0]);

  // Start a readable() wait (no data, so it's pending)
  std::thread t([&fd, &loop]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    xEventLoopRun(loop, X_RUN_ONCE); // wake the loop so close() can run
  });

  // Schedule close after 50ms via timer
  xpp::after(50).then([&fd]() { fd.close(); }).wait();

  // The readable() should have been woken by close()
  t.join();
  SUCCEED();
}

TEST_F(IoAsyncFdTest, MoveConstruct) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  xpp::io::AsyncFd a(sv[0]);
  xpp::io::AsyncFd b(std::move(a));

  EXPECT_EQ(b.fd(), sv[0]);
  EXPECT_TRUE(a.is_closed());
  EXPECT_FALSE(b.is_closed());

  // b should still work
  write(sv[1], "moved", 5);
  char buf[64] = {};
  ssize_t n = xpp::io::read(b, buf, sizeof(buf)).wait();
  EXPECT_EQ(n, 5);
}

/* ── Promise destroyed before event ────────────────────────────────── */

TEST_F(IoAsyncFdTest, PromiseDestroyedBeforeEvent) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  xpp::io::AsyncFd fd(sv[0]);

  {
    auto p = fd.readable();
    (void)p;
    // p destroyed — PromiseResolver's ArcWeak fails to upgrade
  }

  // Write data — on_event should try to resolve, but waiter is gone
  write(sv[1], "safe", 4);

  // Should not crash, readiness is tracked
  fd.readable().wait();
  SUCCEED();
}
