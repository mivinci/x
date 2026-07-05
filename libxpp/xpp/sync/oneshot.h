/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * oneshot.h - xpp::sync::oneshot: single-value channel.
 *
 * Thin wrapper over xpp::async<T>(), matching tokio's API naming.
 */

#ifndef XPP_SYNC_ONESHOT_H
#define XPP_SYNC_ONESHOT_H

#include <utility>

#include <xpp/promise.h>

namespace xpp {
namespace sync {
namespace oneshot {

template <class T> class Sender {
  PromiseResolver<T> m_resolver;

public:
  explicit Sender(PromiseResolver<T> r) : m_resolver(std::move(r)) {}
  void send(T value) {
    m_resolver.resolve(std::move(value));
  }
};

template <class T> class Receiver {
  Promise<T> m_p;

public:
  explicit Receiver(Promise<T> p) : m_p(std::move(p)) {}
  auto recv() && {
    return std::move(m_p);
  }
};

template <class T> std::pair<Sender<T>, Receiver<T>> channel() {
  auto [p, r] = xpp::async<T>();
  return {Sender<T>(std::move(r)), Receiver<T>(std::move(p))};
}

} // namespace oneshot
} // namespace sync
} // namespace xpp

#endif
