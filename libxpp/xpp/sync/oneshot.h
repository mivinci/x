/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * oneshot.h - xpp::sync::oneshot: single-value, single-use channel.
 *
 * Thin wrapper over xpp::async<T>(), matching tokio's API naming.
 *
 * Unlike mpsc, oneshot is inherently thread-safe because
 * PromiseResolver internally uses Arc<ArcWeak> + atomic CAS.
 * No mutex is needed.
 */

#ifndef XPP_SYNC_ONESHOT_H
#define XPP_SYNC_ONESHOT_H

#include <utility>

#include <xpp/promise.h>

namespace xpp {
namespace sync {
namespace oneshot {

/**
 * @brief The sending half of a oneshot channel.
 *
 * Can only be used once. `send()` is callable from any thread
 * because PromiseResolver is internally thread-safe.
 *
 * @tparam T The value type. Must be move-constructible.
 *
 * @see oneshot::channel(), oneshot::Receiver
 */
template <class T> class Sender {
  PromiseResolver<T> m_resolver;

public:
  /**
   * @brief Construct a Sender from a PromiseResolver.
   *
   * @param r The resolver obtained from the channel() factory.
   */
  explicit Sender(PromiseResolver<T> r) : m_resolver(std::move(r)) {}

  /**
   * @brief Send a value, completing the oneshot.
   *
   * If the `Receiver` has already been dropped, the value is discarded
   * silently. Calling `send()` more than once is safe — only the first
   * call takes effect (PromiseResolver uses atomic CAS).
   *
   * @param value The value to transfer to the receiver.
   */
  void send(T value) {
    m_resolver.resolve(std::move(value));
  }
};

/**
 * @brief The receiving half of a oneshot channel.
 *
 * `recv()` consumes the receiver (rvalue-qualified), so it can only
 * be called once.
 *
 * @tparam T The value type.
 *
 * @see oneshot::channel(), oneshot::Sender
 */
template <class T> class Receiver {
  Promise<T> m_p;

public:
  /**
   * @brief Construct a Receiver from a Promise.
   *
   * @param p The promise obtained from the channel() factory.
   */
  explicit Receiver(Promise<T> p) : m_p(std::move(p)) {}

  /**
   * @brief Await the value sent by the Sender.
   *
   * @return A Promise<T> that resolves when `Sender::send()` is called.
   *         Must be `co_await`-ed on an event loop thread.
   *
   * @code
   * auto [tx, rx] = xpp::sync::oneshot::channel<int>();
   * std::thread([tx = std::move(tx)]() mutable { tx.send(42); }).detach();
   * int val = co_await std::move(rx).recv();
   * @endcode
   */
  auto recv() && {
    return std::move(m_p);
  }
};

/**
 * @brief Create a new oneshot channel.
 *
 * @tparam T The value type.
 * @return A pair of Sender<T> (move-only) and Receiver<T> (move-only).
 *
 * Because PromiseResolver is internally thread-safe via Arc<ArcWeak>
 * + atomic CAS, the Sender can be moved to another thread and `send()`
 * can be called from there without additional synchronization.
 *
 * @code
 * auto [tx, rx] = xpp::sync::oneshot::channel<std::string>();
 * tx.send(std::string("hello"));
 * auto val = co_await std::move(rx).recv();
 * @endcode
 */
template <class T> std::pair<Sender<T>, Receiver<T>> channel() {
  auto [p, r] = xpp::async<T>();
  return {Sender<T>(std::move(r)), Receiver<T>(std::move(p))};
}

} // namespace oneshot
} // namespace sync
} // namespace xpp

#endif
