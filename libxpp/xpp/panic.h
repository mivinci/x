/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * panic.h - Fatal-error mechanism for libx++.
 *
 * Provides XPP_PANIC / XPP_ASSERT / XPP_DEBUG_ASSERT macros for reporting
 * unrecoverable contract violations (e.g. unwrap() on a None Option).
 *
 * Panics print the message to stderr and terminate the process. For
 * recoverable errors, use Result<T, E> instead — panics are for bugs,
 * not for runtime conditions the caller is expected to handle.
 *
 * C++11-compatible.
 */

#ifndef XPP_PANIC_H
#define XPP_PANIC_H

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include <xpp/compiler.h>

namespace xpp {
namespace _ {

/**
 * @brief Dispatch a panic message and terminate the process.
 *
 * Defined inline in the header so xpp stays header-only. Routes to
 * vfprintf(stderr) + std::abort() — no external logging dependency.
 * The XPP_PANIC / XPP_ASSERT macros prepend "panic at __FILE__:__LINE__: "
 * so file/line capture happens for free at every call site.
 *
 * Prefer the XPP_PANIC / XPP_ASSERT macros over calling this directly.
 * The printf-format attribute (where supported) lets the compiler
 * type-check the fmt/args pairing at every macro use site.
 */
XPP_NORETURN
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
inline void do_panic(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  std::abort();
}

} // namespace _
} // namespace xpp

/**
 * @brief Unconditionally panic with a printf-style message.
 *
 * The format string is prefixed with "panic at <file>:<line>: " at the
 * call site, so the final output looks like:
 *   panic at foo.cpp:42: idx 7 out of range (size=4)
 *
 * Usage:
 *   if (bad_state) XPP_PANIC("invariant X violated");
 *   XPP_PANIC("idx %zu out of range (size=%zu)", idx, size);
 */
#define XPP_PANIC(fmt, ...) \
  ::xpp::_::do_panic("panic at %s:%d: " fmt, __FILE__, __LINE__, ##__VA_ARGS__)

/**
 * @brief Runtime assertion. Panics if @p cond is false.
 *
 * Always evaluated, even in release builds. Use for public-API contract
 * checks whose failure must not be silently ignored (e.g. Option::unwrap).
 *
 * Accepts printf-style fmt + args; the expanded message includes the
 * stringified condition, so panic output looks like:
 *   panic at foo.cpp:42: assertion failed: idx < size — idx=7 size=4
 *
 * Usage:
 *   XPP_ASSERT(idx < size, "index out of range");
 *   XPP_ASSERT(idx < size, "idx=%zu size=%zu", idx, size);
 */
#define XPP_ASSERT(cond, fmt, ...)                                                            \
  do {                                                                                        \
    if (XPP_UNLIKELY(!(cond)))                                                                \
      ::xpp::_::do_panic("panic at %s:%d: assertion failed: " #cond " \u2014 " fmt, __FILE__, \
                         __LINE__, ##__VA_ARGS__);                                            \
  } while (0)

/**
 * @brief Debug-only assertion. Compiled out when XPP_DEBUG is 0.
 *
 * Use for internal invariants on hot paths, or for *Unchecked() APIs
 * where the caller has already promised the precondition.
 *
 * Usage:
 *   XPP_DEBUG_ASSERT(m_has_value, "internal: Option storage uninitialized");
 *   XPP_DEBUG_ASSERT(idx < size, "idx=%zu size=%zu", idx, size);
 */
#if XPP_DEBUG
#define XPP_DEBUG_ASSERT(cond, fmt, ...) XPP_ASSERT(cond, fmt, ##__VA_ARGS__)
#else
#define XPP_DEBUG_ASSERT(cond, fmt, ...) ((void)0)
#endif

#endif // XPP_PANIC_H
