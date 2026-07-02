/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * opaque.h - OwnedOpaquePointer<D>: alias for Own<void, D>, the
 *            canonical RAII wrapper for libx's opaque handles.
 *
 * libx's opaque handles are all `typedef void *xFoo` (via XDEF_HANDLE).
 * Wrapping them with Own<void, CustomDeleter> is correct but exposes
 * `void` at every use site. This alias hides `void` behind a name that
 * communicates intent: "I own an opaque pointer."
 *
 * Usage:
 *   struct EventLoopDestroy {
 *     void operator()(void *h) const noexcept {
 *       xEventLoopDestroy(static_cast<xEventLoop>(h));
 *     }
 *   };
 *
 *   class EventLoop {
 *     OwnedOpaquePointer<EventLoopDestroy> loop_;
 *     // ...
 *   };
 *
 * C++11-compatible (no auto non-type template parameter needed).
 */

#ifndef XPP_OPAQUE_H
#define XPP_OPAQUE_H

#include <xpp/own.h>

namespace xpp {

/**
 * @brief RAII wrapper for libx opaque handles (void* typedefs).
 *
 * Equivalent to Own<void, D>. The `void` is hidden here so that
 * wrapper classes never expose it directly.
 *
 * @tparam D  Deleter type with `void operator()(void *) const noexcept`.
 *            Typically a small struct that casts to the correct handle
 *            type and calls the corresponding xXxxDestroy function.
 */
template <class D> using OwnedOpaquePointer = Own<void, D>;

} // namespace xpp

#endif // XPP_OPAQUE_H
