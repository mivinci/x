/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * internal.h - xpp::loom::_: compile-time threading primitives.
 *
 * Aliases std::mutex + std::atomic. Threading is always on — the old
 * single-threaded no-op build (XPP_MT=OFF) was removed, so all shared
 * state uses atomic refcounting (Arc) and real locks.
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

#include <atomic>
#include <mutex>

namespace xpp {
namespace loom {
namespace _ {

using Mutex                        = std::mutex;
template <typename T> using Atomic = std::atomic<T>;
using Lock                         = std::unique_lock<std::mutex>;

} // namespace _
} // namespace loom
} // namespace xpp

#endif // XPP_LOOM_INTERNAL_H
