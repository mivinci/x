/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * mutex.h - xpp::loom::Mutex<T>: Rust-style Mutex that owns its data.
 *
 * Unlike a bare std::mutex, Mutex<T> wraps the protected data so the
 * compiler enforces the invariant: you can only access T through
 * MutexGuard<T>, which is obtained by lock().
 *
 *   Mutex<int> m(0);
 *   {
 *     auto g = m.lock();
 *     *g = 42;          // access through guard
 *   }                    // guard drops, mutex unlocked
 *
 * This is a prototype. Future modules should adopt it first; existing
 * code (Channel, Notify, etc.) can migrate incrementally.
 */

#ifndef XPP_LOOM_MUTEX_H
#define XPP_LOOM_MUTEX_H

#include <utility>

#include <xpp/loom/internal.h>
#include <xpp/option.h>

namespace xpp {
namespace loom {

/**
 * @brief RAII guard returned by Mutex<T>::lock().
 *
 * Provides operator* and operator-> to access the protected data.
 * When the guard is destroyed, the mutex is unlocked.
 */
template <typename T> class MutexGuard {
public:
  MutexGuard(MutexGuard &&) noexcept            = default;
  MutexGuard &operator=(MutexGuard &&) noexcept = default;
  MutexGuard(const MutexGuard &)                = delete;
  MutexGuard &operator=(const MutexGuard &)     = delete;

  T *operator->() noexcept {
    return m_data;
  }
  const T *operator->() const noexcept {
    return m_data;
  }
  T &operator*() noexcept {
    return *m_data;
  }
  const T &operator*() const noexcept {
    return *m_data;
  }

private:
  template <typename U> friend class Mutex;
  T                 *m_data;
  xpp::loom::_::Lock m_lock;

  MutexGuard(T *data, xpp::loom::_::Mutex &m) : m_data(data), m_lock(m) {}
};

/**
 * @brief A mutual exclusion primitive that owns its data.
 *
 * @tparam T The type of protected data. No special requirements beyond
 *           what MutexGuard needs (move-constructible for try_lock via
 *           Option).
 *
 * @see MutexGuard
 */
template <typename T> class Mutex {
public:
  template <typename... Args>
  explicit Mutex(Args &&...args) : m_data(std::forward<Args>(args)...) {}

  /**
   * @brief Acquire the mutex, blocking until available.
   * @return A MutexGuard providing access to the protected data.
   */
  MutexGuard<T> lock() {
    return MutexGuard<T>(&m_data, m_mutex);
  }

  /**
   * @brief Try to acquire the mutex without blocking.
   * @return Some(guard) if acquired, none otherwise.
   */
  xpp::Option<MutexGuard<T>> try_lock() {
    if (m_mutex.try_lock()) {
      return xpp::some(MutexGuard<T>(&m_data, m_mutex));
    }
    return xpp::none;
  }

private:
  T                   m_data;
  xpp::loom::_::Mutex m_mutex;
};

} // namespace loom
} // namespace xpp

#endif // XPP_LOOM_MUTEX_H
