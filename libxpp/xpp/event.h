/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * event.h - C++ RAII wrapper for xEventLoop.
 *
 * EventLoop owns the handle (create/destroy). WaitScope manages the
 * thread-local binding (enter/leave). Separation matches libx's C API
 * where xEventLoopEnter/Leave are independent of create/destroy.
 *
 * Usage:
 *   xpp::EventLoop loop;
 *   {
 *     xpp::WaitScope scope(loop);
 *     loop.run();           // blocks until stop() is called
 *   }                       // WaitScope leaves the loop
 *
 *   // Interop with C API:
 *   xTimer t = xTimerStart(my_cb, loop.handle(), NULL, 100, 0);
 */

#ifndef XPP_EVENT_H
#define XPP_EVENT_H

#include <xpp/opaque.h>

#include <x/base/event.h>

namespace xpp {

/// Run mode for EventLoop::run().
enum class RunMode {
  Default = X_RUN_DEFAULT, ///< Block until stop() or no more active handles.
  Once    = X_RUN_ONCE,    ///< Single iteration, block until at least one event.
  NoWait  = X_RUN_NOWAIT,  ///< Single iteration, non-blocking poll.
};

/**
 * @brief RAII wrapper for xEventLoop handle (create/destroy only).
 *
 * Move-only: copy constructor and copy assignment are deleted. Move
 * constructor and move assignment are defaulted — ownership transfers
 * to the destination, leaving the source in an empty (falsy) state.
 * The moved-from object is safe to destroy or reassign.
 *
 * This class does NOT call xEventLoopEnter/Leave — thread-local
 * binding is managed by WaitScope. This separation mirrors the C API
 * where xEventLoopCreate/Destroy and xEventLoopEnter/Leave are
 * independent operations.
 *
 * Thread safety: the handle itself may be passed to thread-safe APIs
 * (stop, wake, Post) from any thread. Non-thread-safe APIs (run,
 * xTimerStart, xEventAdd) must be called from the thread that entered
 * the loop via WaitScope.
 */
class EventLoop {
public:
  /// Create an event loop. Check `operator bool` for failure.
  EventLoop() : m_loop(xEventLoopCreate()) {}

  /// Destroy the loop (does not Leave — use WaitScope for that).
  ~EventLoop() = default;

  EventLoop(const EventLoop &)                = delete;
  EventLoop &operator=(const EventLoop &)     = delete;
  EventLoop(EventLoop &&) noexcept            = default;
  EventLoop &operator=(EventLoop &&) noexcept = default;

  /// Run the event loop (blocks until stop or no more handles).
  void run(RunMode mode = RunMode::Default) {
    xEventLoopRun(handle(), static_cast<int>(mode));
  }

  /// Stop a running event loop (thread-safe).
  void stop() {
    xEventLoopStop(handle());
  }

  /// Wake the event loop from poll (thread-safe, no-op if not polling).
  void wake() {
    xEventLoopWake(handle());
  }

  /// Access the underlying xEventLoop handle for interop with C API.
  xEventLoop handle() const {
    return static_cast<xEventLoop>(m_loop.get());
  }

  /// True if the event loop was created successfully.
  explicit operator bool() const {
    return static_cast<bool>(m_loop);
  }

  /// Return the event loop bound to the current thread via WaitScope.
  /// Panics if called outside a WaitScope.
  static xEventLoop current() {
    xEventLoop loop = xEventLoopCurrent();
    XPP_ASSERT(loop != nullptr, "EventLoop::current() called outside WaitScope");
    return loop;
  }

private:
  /// Allocator: calls xEventLoopDestroy on deallocate.
  /// (allocate is never called — handles come from xEventLoopCreate.)
  struct Destroy {
    void deallocate(void *p, xpp::Layout) const noexcept {
      xEventLoopDestroy(static_cast<xEventLoop>(p));
    }
  };

  OwnedOpaquePointer<Destroy> m_loop;
};

/**
 * @brief RAII scope for xEventLoopEnter / xEventLoopLeave.
 *
 * Non-copyable, non-movable: the enter/leave pair is tied to the
 * scope in which it was created. Moving would leave the original
 * scope without a corresponding Leave, violating the thread-local
 * binding contract.
 *
 * Must be created on the same thread that calls EventLoop::run().
 * The loop is entered on construction and left on destruction.
 *
 * Usage:
 *   xpp::EventLoop loop;
 *   {
 *     xpp::WaitScope scope(loop);
 *     loop.run();
 *   }
 */
class WaitScope {
public:
  explicit WaitScope(const EventLoop &loop) : m_loop(loop.handle()) {
    if (m_loop) {
      xEventLoopEnter(m_loop);
    }
  }

  ~WaitScope() {
    if (m_loop) {
      xEventLoopLeave();
    }
  }

  WaitScope(const WaitScope &)            = delete;
  WaitScope &operator=(const WaitScope &) = delete;
  WaitScope(WaitScope &&)                 = delete;
  WaitScope &operator=(WaitScope &&)      = delete;

private:
  xEventLoop m_loop;
};

} // namespace xpp

#endif // XPP_EVENT_H
