/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_test_helper.h — Shared helpers for Promise tests.
 */
#ifndef XPP_PROMISE_TEST_HELPER_H_
#define XPP_PROMISE_TEST_HELPER_H_

#include <utility>

#include <xpp/promise.h>
#include <xpp/timer.h>

namespace xpp {

/** Schedule a one-shot timer that resolves a PromiseResolver<T> with a value. */
template <class T> Timer schedule_resolve(PromiseResolver<T> &r, T value, uint64_t delay_ms) {
  return Timer(delay_ms, 0, [&r, v = std::move(value)]() mutable { r.resolve(std::move(v)); });
}

/** Void overload — no value parameter. */
inline Timer schedule_resolve(PromiseResolver<void> &r, uint64_t delay_ms) {
  return Timer(delay_ms, 0, [&r]() { r.resolve(); });
}

} // namespace xpp

#endif // XPP_PROMISE_TEST_HELPER_H_
