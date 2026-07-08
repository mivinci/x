/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * util.h - xpp::io utility functions: read_all, copy.
 *
 * Template-based, duck-typed on read(void*, size_t) → Promise<ssize_t>
 * and write(const void*, size_t) → Promise<ssize_t>. No traits, no CRTP.
 *
 * C++20: coroutine + concept versions (clear compile errors).
 * C++11: struct + std::move(*this) fallback (zero heap alloc in combinator).
 *
 * Header-only.
 */

#ifndef XPP_IO_UTIL_H
#define XPP_IO_UTIL_H

#include <sys/types.h>

#include <cstddef>
#include <vector>

#include <xpp/promise.h>
#include <xpp/shared.h>

namespace xpp {
namespace io {

namespace _ {

/** @brief Default buffer size for I/O utilities (8KB, matches Rust). */
constexpr size_t kBufSize = 8192;

} // namespace _

#if XPP_HAS_CONCEPT

/* ═══ C++20 concepts — template constraints ═══════════════════════ */

/**
 * @brief Concept: R has read(void*, size_t) returning an awaitable type.
 *
 * Satisfied by TcpStream, fs::File (cursor mode), and user-defined
 * types matching the signature. Used as a template constraint to
 * produce clear compile-time errors when the type is wrong.
 */
template <class R>
concept AsyncReader = requires(R &r, void *buf, size_t len) {
  { r.read(buf, len) };
};

/**
 * @brief Concept: W has write(const void*, size_t) returning an awaitable type.
 */
template <class W>
concept AsyncWriter = requires(W &w, const void *buf, size_t len) {
  { w.write(buf, len) };
};

/** @brief Concept: T satisfies both AsyncReader and AsyncWriter. */
template <class T>
concept AsyncReadWriter = AsyncReader<T> && AsyncWriter<T>;

#endif // XPP_HAS_CONCEPT

#if XPP_HAS_COROUTINES

/* ═══ C++20 coroutine versions ══════════════════════════════════════ */

/**
 * @brief Read the entire byte stream into a vector.
 *
 * @tparam R Reader type satisfying AsyncReader (e.g., TcpStream, fs::File)
 */
template <AsyncReader R> Promise<std::vector<uint8_t>> read_all(R &reader) {
  std::vector<uint8_t> result;
  uint8_t              buf[8192];
  while (true) {
    ssize_t n = co_await reader.read(buf, sizeof(buf));
    if (n <= 0) break;
    result.insert(result.end(), buf, buf + n);
  }
  co_return result;
}

/**
 * @brief Copy all bytes from reader to writer.
 *
 * @tparam R Reader type satisfying AsyncReader
 * @tparam W Writer type satisfying AsyncWriter
 */
template <AsyncReader R, AsyncWriter W> Promise<void> copy(R &reader, W &writer) {
  uint8_t buf[_::kBufSize];
  while (true) {
    ssize_t n = co_await reader.read(buf, sizeof(buf));
    if (n <= 0) co_return;
    co_await writer.write(buf, static_cast<size_t>(n));
  }
}

#else // !XPP_HAS_COROUTINES

#if XPP_FIBER

/* ═══ C++11 + fiber: linear .await() loop ════════════════════════════
 *
 * With fiber support, .await() handles both suspend (inside a fiber)
 * and blocking X_RUN_ONCE (outside). The implementation is a plain
 * while loop — structurally identical to the C++20 coroutine version
 * but using .await() instead of co_await.
 *
 * No struct, no Shared, no std::move(*this) — just linear code.
 * ─────────────────────────────────────────────────────────────────── */

template <class R> Promise<std::vector<uint8_t>> read_all(R &reader) {
  std::vector<uint8_t> result;
  uint8_t              buf[_::kBufSize];
  while (true) {
    ssize_t n = reader.read(buf, sizeof(buf)).await();
    if (n <= 0) break;
    result.insert(result.end(), buf, buf + n);
  }
  return xpp::resolve(std::move(result));
}

template <class R, class W> Promise<void> copy(R &reader, W &writer) {
  uint8_t buf[_::kBufSize];
  while (true) {
    ssize_t n = reader.read(buf, sizeof(buf)).await();
    if (n <= 0) break;
    writer.write(buf, static_cast<size_t>(n)).await();
  }
  return xpp::resolve();
}

#else // !XPP_FIBER

/* ═══ C++11: struct+move fallback ════════════════════════════════════
 *
 * Replaces coroutine `while (true) { co_await ... }` loops with
 * struct + std::move(*this) recursive chaining.
 *
 * read_all accumulates into a vector — a single while loop with
 * tail-recursive .then(). copy chains two .then() levels: read → write
 * → recurse (read again).
 *
 * Trade-offs:
 *   - xpp::Shared for the 8KB buffer and vector (read_all). One heap
 *     alloc per call — same as the coroutine version's implicit
 *     allocation for the coroutine frame's local variables.
 *   - Reader/Writer captured by reference (&). Must outlive the promise
 *     chain — guaranteed because the caller holds them on stack while
 *     awaiting the result.
 *   - All .then() nodes are arena bump-allocated (promise_allocator.h),
 *     so the only heap costs are the Shared<State/Buf> allocations.
 * ═══════════════════════════════════════════════════════════════════ */

/**
 * @brief Read the entire byte stream into a vector (C++11 fallback).
 *
 * ReadAllLoop holds a Shared<State> (8KB buf + result vector on heap)
 * and a reference to the reader. Each .then() iteration reads a chunk,
 * appends to the vector, and tail-recurses until n <= 0.
 *
 * @tparam R Duck-typed: R::read(void*, size_t) must return a then-able
 *           resolving to ssize_t. n <= 0 terminates the loop.
 */
template <class R> Promise<std::vector<uint8_t>> read_all(R &reader) {
  struct State {
    std::vector<uint8_t> data;
    uint8_t              buf[_::kBufSize];
  };
  auto state = xpp::Shared<State>::make();

  struct ReadAllLoop {
    R                 &reader;
    xpp::Shared<State> state;

    Promise<std::vector<uint8_t>> operator()() {
      return reader.read(state->buf, sizeof(state->buf))
        .then([self = std::move(*this)](ssize_t n) mutable {
          if (n <= 0) return xpp::resolve(std::move(self.state->data));
          self.state->data.insert(self.state->data.end(), self.state->buf, self.state->buf + n);
          return self(); // tail-recursive via Promise chain
        });
    }
  };

  return ReadAllLoop{reader, state}();
}

/**
 * @brief Copy all bytes from reader to writer (C++11 fallback).
 *
 * CopyLoop chains two .then() levels per iteration: read a chunk →
 * write it → recurse. Uses a Shared<Buf> for the transfer buffer to
 * avoid moving 8KB through each .then() node. Terminates when read
 * returns n <= 0, returning a resolved Promise<void>.
 *
 * @tparam R Duck-typed: R::read(void*, size_t) → then-able<ssize_t>
 * @tparam W Duck-typed: W::write(const void*, size_t) → then-able<ssize_t>
 */
template <class R, class W> Promise<void> copy(R &reader, W &writer) {
  struct Buf {
    uint8_t data[_::kBufSize];
  };
  auto buf = xpp::Shared<Buf>::make();

  struct CopyLoop {
    R               &reader;
    W               &writer;
    xpp::Shared<Buf> buf;

    Promise<void> operator()() {
      return reader.read(buf->data, sizeof(buf->data))
        .then([self = std::move(*this)](ssize_t n) mutable {
          if (n <= 0) return xpp::resolve();
          return self.writer.write(self.buf->data, static_cast<size_t>(n))
            .then([self = std::move(self)](ssize_t) mutable {
              return self(); // tail-recursive read loop
            });
        });
    }
  };

  return CopyLoop{reader, writer, buf}();
}

#endif // XPP_FIBER

#endif // XPP_HAS_COROUTINES

} // namespace io
} // namespace xpp

#endif // XPP_IO_UTIL_H
