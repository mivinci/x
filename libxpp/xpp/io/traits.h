/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * traits.h — I/O trait concepts: TryRead, AsyncRead, AsyncWrite.
 *
 * TryRead — synchronous, non-blocking read via try_read(char*, size_t).
 *   Returns ssize_t: > 0 = data, 0 = EOF, < 0 = EAGAIN (try later).
 *   Read(2) semantics, used by curl read callbacks and channel receivers.
 *
 * AsyncRead / AsyncWrite — asynchronous read/write via read(void*, size_t)
 *   / write(const void*, size_t) returning an awaitable type (Promise).
 *   Used by io::read_all, io::copy, BufReader, BufWriter, etc.
 *
 * C++20 concepts guard with XPP_HAS_CONCEPT. C++11 path relies on
 * duck-typing — the template error messages are worse but the code
 * compiles and works identically.
 */

#ifndef XPP_IO_TRAITS_H
#define XPP_IO_TRAITS_H

#include <sys/types.h>

#include <cstddef>

#include <xpp/compiler.h>

namespace xpp {
namespace io {

#if XPP_HAS_CONCEPT

#include <concepts>

/* ── TryRead ───────────────────────────────────────────────────── */

template <class R>
concept TryRead = requires(R &r, char *buf, size_t cap) {
  { r.try_read(buf, cap) } -> std::same_as<ssize_t>;
};

/* ── AsyncRead / AsyncWrite ─────────────────────────────────────── */

template <class R>
concept AsyncRead = requires(R &r, void *buf, size_t len) {
  { r.read(buf, len) };
};

template <class W>
concept AsyncWrite = requires(W &w, const void *buf, size_t len) {
  { w.write(buf, len) };
};

template <class T>
concept AsyncReadWrite = AsyncRead<T> && AsyncWrite<T>;

#endif // XPP_HAS_CONCEPT

/* ═══════════════════════════════════════════════════════════════════
 *  XPP_REQUIRES_TRYREAD(R)
 *
 *  Template parameter constraint requiring try_read(char*, size_t).
 *  In C++20 mode this expands to a concept (io::TryRead R); in
 *  C++11..17 mode it is a plain class R — the compiler will produce
 *  a readable error at instantiation time when try_read is missing.
 *
 *  Usage:
 *    template <XPP_REQUIRES_TRYREAD(R)>
 *    void foo(R &&reader) { ... }
 * ══════════════════════════════════════════════════════════════════ */

#if defined(XPP_HAS_CONCEPT) && XPP_HAS_CONCEPT
#define XPP_REQUIRES_TRYREAD(R)       io::TryRead R
#define XPP_REQUIRES_ASYNC_READ(R)    io::AsyncRead R
#define XPP_REQUIRES_ASYNC_WRITE(W)   io::AsyncWrite W
#define XPP_REQUIRES_READ_WRITE(T)    io::AsyncReadWrite T
#else
#define XPP_REQUIRES_TRYREAD(R)       class R
#define XPP_REQUIRES_ASYNC_READ(R)    class R
#define XPP_REQUIRES_ASYNC_WRITE(W)   class W
#define XPP_REQUIRES_READ_WRITE(T)    class T
#endif

} // namespace io
} // namespace xpp

#endif // XPP_IO_TRAITS_H
