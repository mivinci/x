/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * handle.h - OwnedHandle<Deleter> + BorrowedHandle<T>: RAII and
 *            non-owning wrappers for libx opaque handles.
 *
 * libx's opaque handles are all `typedef void *xFoo` (via XDEF_HANDLE).
 * OwnedHandle wraps them with RAII destruction. BorrowedHandle wraps them
 * as non-owning references with type intent.
 *
 * The deleter only needs a `deallocate(void*, Layout)` method —
 * `allocate` is never called because handles come from the C API
 * (e.g. `xEventLoopCreate`), not from the deleter.
 *
 * Usage:
 *   struct EventLoopDestroy {
 *     void deallocate(void *p, xpp::Layout) const noexcept {
 *       xEventLoopDestroy(static_cast<xEventLoop>(p));
 *     }
 *   };
 *
 *   class EventLoop {
 *     OwnedHandle<EventLoopDestroy> m_loop;
 *     BorrowedHandle<xEventLoop> handle() const { ... }
 *   };
 *
 * C++11-compatible.
 */

#ifndef XPP_HANDLE_H
#define XPP_HANDLE_H

#include <cstddef>

#include <xpp/own.h>

namespace xpp {

/**
 * @brief RAII wrapper for libx opaque handles (void* typedefs).
 *
 * Equivalent to Own<void, Deleter>. The `void` is hidden here so that
 * wrapper classes never expose it directly.
 *
 * @tparam Deleter  Type with `void deallocate(void*, Layout) const noexcept`.
 *                  Typically a small struct that casts to the correct
 *                  handle type and calls the corresponding xXxxDestroy.
 */
template <class Deleter> using OwnedHandle = Own<void, Deleter>;

} // namespace xpp
