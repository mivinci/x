/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_types.h — shared types without template machinery.
 *
 * Defines _::WakerCore (the inner waker state) and, when XPP_FIBER
 * is defined, _::fiber::Context (the fiber execution context header).
 *
 * Both promise_waker.h and promise_context.h include this header, so
 * they can share these struct definitions without circular includes.
 * No template code, no Promise<T> — just plain structs.
 */

#ifndef XPP_PROMISE_TYPES_H
#define XPP_PROMISE_TYPES_H

#include <xpp/arc.h>
#include <xpp/event.h>

#if XPP_FIBER
#include <x/base/fiber.h>
#endif

namespace xpp {

/* ── _::WakerCore ─────────────────────────────────────────────────── */

namespace _ {

struct WakerCore {
  xEventLoop loop;
  bool       done = false;

  WakerCore() = default;
  explicit WakerCore(xEventLoop l) : loop(l) {}

#if XPP_FIBER
  xFiber fiber = nullptr;
  WakerCore(xEventLoop l, xFiber f) : loop(l), fiber(f) {}
#endif
};

} // namespace _

/* ── _::fiber::Context ───────────────────────────────────────────── */

#if XPP_FIBER
namespace _ {
namespace fiber {

/**
 * @brief Per-fiber execution context.  Lives on the heap, allocated once
 *        by xpp::fiber().  The waker field carries the per-fiber
 *        PromiseWaker — every await() inside the fiber clones its Arc
 *        (1 fetch_add, 0 heap alloc).
 */
struct Context {
  void (*run)(void *state);
  void (*destroy)(void *state);
  xFiber            handle;
  Arc<_::WakerCore> waker;
};

} // namespace fiber
} // namespace _
#endif

} // namespace xpp

#endif // XPP_PROMISE_TYPES_H
