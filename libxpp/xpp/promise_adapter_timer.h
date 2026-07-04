/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_adapter_timer.h - TimerAdapter for Promise.
 *
 * TimerAdapter owns an xTimer; callback calls resolver.resolve().
 * Destructor calls xTimerStop (synchronous on the event loop thread).
 *
 * Used by xpp::after(ms).
 *
 * C++11-compatible. Header-only.
 */
#ifndef XPP_PROMISE_ADAPTER_TIMER_H
#define XPP_PROMISE_ADAPTER_TIMER_H

#include <atomic>

#include <xpp/promise_adapter.h>

#include <x/base/event.h>

namespace xpp {

/* ── TimerAdapter ────────────────────────────────────────────────── */

class TimerAdapter {
public:
  TimerAdapter(PromiseResolver<void> &&resolver, uint64_t ms) : m_resolver(std::move(resolver)) {
    m_handle = xTimerStart(
      [](void *a) {
        auto *self = static_cast<TimerAdapter *>(a);
        self->m_fired.store(true, std::memory_order_release);
        self->m_resolver.resolve();
      },
      this,
      [](void *a) {
        auto *self     = static_cast<TimerAdapter *>(a);
        self->m_handle = nullptr;
        self->m_fired.store(true, std::memory_order_release);
      },
      ms, 0);
  }

  ~TimerAdapter() {
    if (!m_fired.load(std::memory_order_acquire) && m_handle) {
      xTimerStop(m_handle);
    }
  }

  TimerAdapter(const TimerAdapter &)            = delete;
  TimerAdapter &operator=(const TimerAdapter &) = delete;
  TimerAdapter(TimerAdapter &&)                 = delete;
  TimerAdapter &operator=(TimerAdapter &&)      = delete;

private:
  xTimer                m_handle;
  std::atomic<bool>     m_fired{false};
  PromiseResolver<void> m_resolver;
};

} // namespace xpp

#endif // XPP_PROMISE_ADAPTER_TIMER_H
