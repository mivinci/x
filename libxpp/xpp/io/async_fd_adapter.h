/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * async_fd_adapter.h — Internal: AsyncReadAdapter / AsyncWriteAdapter.
 *
 * Bridges AsyncFd readiness to PromiseResolver. The adapter stores the
 * PromiseResolver in AsyncFd's waiter slot; on_event resolves it when
 * the fd becomes ready.
 *
 * Do not include directly — included by async_fd.h.
 */

#ifndef XPP_IO_ASYNC_FD_ADAPTER_H
#define XPP_IO_ASYNC_FD_ADAPTER_H

#include <xpp/promise.h>

namespace xpp {
namespace io {
namespace _ {

/* ── AsyncReadAdapter ──────────────────────────────────────────────── */

/** @brief Adapter that stores a PromiseResolver in AsyncFd and resolves it when the fd becomes readable. */
class AsyncReadAdapter {
private:
  AsyncFd *m_fd;

public:
  AsyncReadAdapter(PromiseResolver<void> r, AsyncFd *fd) : m_fd(fd) {
    // Store resolver in AsyncFd. If fd is closed, resolve immediately.
    if (m_fd->is_closed()) {
      r.resolve();
      return;
    }
    // Double-check: readiness might have been set between the bool check
    // in readable() and here. But since single-threaded, no race — the
    // bool check in readable() is authoritative.
    m_fd->m_read_waiter = std::move(r);
  }

  ~AsyncReadAdapter()                                   = default;
  AsyncReadAdapter(const AsyncReadAdapter &)            = delete;
  AsyncReadAdapter &operator=(const AsyncReadAdapter &) = delete;
  AsyncReadAdapter(AsyncReadAdapter &&)                 = delete;
  AsyncReadAdapter &operator=(AsyncReadAdapter &&)      = delete;
};

/* ── AsyncWriteAdapter ─────────────────────────────────────────────── */

/** @brief Adapter that stores a PromiseResolver in AsyncFd and resolves it when the fd becomes writable. */
class AsyncWriteAdapter {
private:
  AsyncFd *m_fd;

public:
  AsyncWriteAdapter(PromiseResolver<void> r, AsyncFd *fd) : m_fd(fd) {
    if (m_fd->is_closed()) {
      r.resolve();
      return;
    }
    m_fd->m_write_waiter = std::move(r);
  }

  ~AsyncWriteAdapter()                                    = default;
  AsyncWriteAdapter(const AsyncWriteAdapter &)            = delete;
  AsyncWriteAdapter &operator=(const AsyncWriteAdapter &) = delete;
  AsyncWriteAdapter(AsyncWriteAdapter &&)                 = delete;
  AsyncWriteAdapter &operator=(AsyncWriteAdapter &&)      = delete;
};

} // namespace _
} // namespace io
} // namespace xpp

#endif // XPP_IO_ASYNC_FD_ADAPTER_H
