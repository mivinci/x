/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * async_fd.h - AsyncFd: reactive async I/O for non-blocking file descriptors.
 *
 * Registers an fd with the event loop once (edge-triggered, Read|Write),
 * tracks readiness via plain bools (single-threaded), and provides
 * readable()/writable() as Promise<void> via the Adapter pattern.
 *
 * Free functions read()/write() combine a fast-path syscall (zero
 * Promise overhead) with readiness wait on EAGAIN.
 *
 * Single-threaded. When multi-threaded scheduler is added, upgrade to
 * atomic<uint8_t> + mutex<Waiters> + double-check-under-lock.
 *
 * C++17-compatible. Header-only.
 */

#ifndef XPP_IO_ASYNC_FD_H
#define XPP_IO_ASYNC_FD_H

#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <utility>

#include <xpp/arc.h>
#include <xpp/option.h>
#include <xpp/promise.h>

#include <x/base/base.h>
#include <x/base/event.h>

namespace xpp {
namespace io {

/* ── AsyncFd ───────────────────────────────────────────────────────── */

namespace _ {
class AsyncReadAdapter;
class AsyncWriteAdapter;
} // namespace _

/**
 * @brief Reactive I/O wrapper for a non-blocking file descriptor.
 *
 * Registers the fd with the event loop once (edge-triggered, Read|Write).
 * Tracks readiness internally. Provides readable()/writable() returning
 * Promise<void> that resolves when the fd becomes ready.
 *
 * Does NOT own the fd — caller is responsible for ::close(fd).
 * close() deregisters from the event loop and wakes pending waiters.
 *
 * Move-only. Moved-from state is a tombstone (fd == -1).
 *
 * Single-threaded: all operations must run on the event loop thread.
 */
class AsyncFd {
public:
  explicit AsyncFd(int fd);
  ~AsyncFd();

  AsyncFd(AsyncFd &&o) noexcept;
  AsyncFd &operator=(AsyncFd &&o) noexcept;
  AsyncFd(const AsyncFd &)            = delete;
  AsyncFd &operator=(const AsyncFd &) = delete;

  /** @brief Wait until the fd is readable. Resolves immediately if already ready. */
  Promise<void> readable() const;

  /** @brief Wait until the fd is writable. Resolves immediately if already ready. */
  Promise<void> writable() const;

  /** @brief Deregister from event loop, wake all pending waiters. Does NOT close fd. */
  void close();

  int fd() const {
    return m_fd;
  }
  bool is_closed() const {
    return m_fd < 0;
  }

private:
  friend class _::AsyncReadAdapter;
  friend class _::AsyncWriteAdapter;

  int                   m_fd       = -1;
  xEventSource          m_src      = nullptr;
  bool                  m_readable = false;
  bool                  m_writable = false;
  PromiseResolver<void> m_read_waiter;
  PromiseResolver<void> m_write_waiter;

  static void on_event(int fd, xEventMask mask, void *arg);

  void wake_read();
  void wake_write();
};

/* ── Free functions ────────────────────────────────────────────────── */

/** @brief Async read: try recv, on EAGAIN wait for readable then retry. */
Promise<ssize_t> read(AsyncFd &io, void *buf, size_t len);

/** @brief Async write: try send, on EAGAIN wait for writable then retry. */
Promise<ssize_t> write(AsyncFd &io, const void *buf, size_t len);

} // namespace io
} // namespace xpp

/* ── Inline implementations ────────────────────────────────────────── */

#include <xpp/io/async_fd_adapter.h>

namespace xpp {
namespace io {

/* ── AsyncFd methods ───────────────────────────────────────────────── */

inline AsyncFd::AsyncFd(int fd) : m_fd(fd) {
  if (fd >= 0) {
    m_src = xEventAdd(fd, static_cast<xEventMask>(xEvent_Read | xEvent_Write), on_event, this);
  }
}

inline AsyncFd::~AsyncFd() {
  close();
}

inline AsyncFd::AsyncFd(AsyncFd &&o) noexcept
    : m_fd(o.m_fd), m_src(o.m_src), m_readable(o.m_readable), m_writable(o.m_writable),
      m_read_waiter(std::move(o.m_read_waiter)), m_write_waiter(std::move(o.m_write_waiter)) {
  o.m_fd  = -1;
  o.m_src = nullptr;
  // Update the event callback's arg pointer to point to us
  // (xEventMod can't change arg, so we need to re-register)
  if (m_src && m_fd >= 0) {
    xEventDel(m_src);
    m_src = xEventAdd(m_fd, static_cast<xEventMask>(xEvent_Read | xEvent_Write), on_event, this);
  }
}

inline AsyncFd &AsyncFd::operator=(AsyncFd &&o) noexcept {
  if (this != &o) {
    close();
    m_fd           = o.m_fd;
    m_src          = o.m_src;
    m_readable     = o.m_readable;
    m_writable     = o.m_writable;
    m_read_waiter  = std::move(o.m_read_waiter);
    m_write_waiter = std::move(o.m_write_waiter);
    o.m_fd         = -1;
    o.m_src        = nullptr;
    if (m_src && m_fd >= 0) {
      xEventDel(m_src);
      m_src = xEventAdd(m_fd, static_cast<xEventMask>(xEvent_Read | xEvent_Write), on_event, this);
    }
  }
  return *this;
}

inline void AsyncFd::close() {
  if (m_src) {
    xEventDel(m_src);
    m_src = nullptr;
  }
  if (m_fd >= 0) {
    m_fd = -1;
  }
  // Wake pending waiters — they'll see is_closed() or get resolved
  wake_read();
  wake_write();
}

inline void AsyncFd::on_event(int fd, xEventMask mask, void *arg) {
  auto *self = static_cast<AsyncFd *>(arg);
  if (mask & xEvent_Read) {
    if (self->m_read_waiter.is_pending()) {
      // Have a waiter — resolve it directly, don't set readiness
      self->wake_read();
    } else {
      self->m_readable = true;
    }
  }
  if (mask & xEvent_Write) {
    if (self->m_write_waiter.is_pending()) {
      self->wake_write();
    } else {
      self->m_writable = true;
    }
  }
}

inline void AsyncFd::wake_read() {
  if (m_read_waiter.is_pending()) {
    auto r     = std::move(m_read_waiter);
    m_readable = false;
    r.resolve();
  }
}

inline void AsyncFd::wake_write() {
  if (m_write_waiter.is_pending()) {
    auto r     = std::move(m_write_waiter);
    m_writable = false;
    r.resolve();
  }
}

inline Promise<void> AsyncFd::readable() const {
  // Check fast path: already readable
  if (m_readable) {
    const_cast<AsyncFd *>(this)->m_readable = false;
    return xpp::yield(); // immediately resolved
  }
  // Slow path: store resolver via adapter
  // We need a shared reference for the adapter. Since AsyncFd is not
  // shared via Arc, we pass a raw pointer. The adapter's lifetime is
  // tied to AdapterPromiseNode, which is tied to the Promise.
  // The PromiseResolver uses ArcWeak, so if the Promise is destroyed,
  // resolve() is a no-op.
  return xpp::adapt<void, _::AsyncReadAdapter>(const_cast<AsyncFd *>(this));
}

inline Promise<void> AsyncFd::writable() const {
  if (m_writable) {
    const_cast<AsyncFd *>(this)->m_writable = false;
    return xpp::yield();
  }
  return xpp::adapt<void, _::AsyncWriteAdapter>(const_cast<AsyncFd *>(this));
}

/* ── Free functions ────────────────────────────────────────────────── */

inline Promise<ssize_t> read(AsyncFd &io, void *buf, size_t len) {
  // Fast path: try recv immediately
  ssize_t n = ::read(io.fd(), buf, len);
  if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
    return xpp::resolve(n); // success or error
  }
  // Slow path: EAGAIN — wait for readable, then retry
  int fd = io.fd();
  return io.readable().then([fd, buf, len] { return ::read(fd, buf, len); });
}

inline Promise<ssize_t> write(AsyncFd &io, const void *buf, size_t len) {
  ssize_t n = ::write(io.fd(), buf, len);
  if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
    return xpp::resolve(n);
  }
  int fd = io.fd();
  return io.writable().then([fd, buf, len] { return ::write(fd, buf, len); });
}

} // namespace io
} // namespace xpp

#endif // XPP_IO_ASYNC_FD_H
