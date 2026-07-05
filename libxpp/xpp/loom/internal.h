/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * internal.h - xpp::loom::_: compile-time threading primitives.
 *
 * When XPP_MT is defined: aliases std::mutex + std::atomic.
 * Otherwise: no-ops that compile away to zero overhead.
 *
 * Shared by xpp::loom::Mutex<T>, xpp::sync::mpsc/notify/broadcast,
 * and future threading primitives.
 *
 * TODO: Loom-style concurrency testing.
 * When the testing harness is ready, these type aliases will be
 * swapped for instrumented versions (tracking lock acquires,
 * atomic operations, and thread interleavings) while preserving
 * the same interface, enabling exhaustive race-condition detection.
 */

#ifndef XPP_LOOM_INTERNAL_H
#define XPP_LOOM_INTERNAL_H

#if XPP_MT
#include <atomic>
#include <mutex>
#endif

namespace xpp {
namespace loom {
namespace _ {

#ifdef XPP_MT
using Mutex = std::mutex;
template <typename T>
using Atomic = std::atomic<T>;
using Lock   = std::unique_lock<std::mutex>;
#else

/// No-op mutex — compiles away in single-threaded builds.
struct Mutex {
  void lock() {}
  void unlock() {}
  bool try_lock() { return true; }
};

/// No-op atomic wrapper — same interface as std::atomic, zero overhead.
template <typename T> struct Atomic {
  T value{};
  T load(std::memory_order = std::memory_order_seq_cst) const noexcept {
    return value;
  }
  void store(T v, std::memory_order = std::memory_order_seq_cst) noexcept {
    value = v;
  }
  T fetch_add(T v, std::memory_order = std::memory_order_seq_cst) noexcept {
    T old = value;
    value += v;
    return old;
  }
  T fetch_sub(T v, std::memory_order = std::memory_order_seq_cst) noexcept {
    T old = value;
    value -= v;
    return old;
  }
  T  operator++(int) noexcept { return value++; }
  T  operator--(int) noexcept { return value--; }
  T &operator++() noexcept { return ++value; }
  T &operator--() noexcept { return --value; }
  operator T() const noexcept { return value; }
  void operator=(T v) noexcept { value = v; }
};

/// No-op lock guard — compiles away in single-threaded builds.
struct Lock {
  template <typename M> Lock(M &) {}
  void unlock() {}
};

#endif

} // namespace _
} // namespace loom
} // namespace xpp

#endif // XPP_LOOM_INTERNAL_H
