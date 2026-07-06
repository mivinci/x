/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_utils.h — Promise helper utilities (iterators, retry, timeout, etc.)
 *
 * C++11-compatible. Header-only.
 */

#ifndef XPP_PROMISE_UTILS_H
#define XPP_PROMISE_UTILS_H

#include <utility>

#include <xpp/promise.h>

namespace xpp {

/* ── try_next ──────────────────────────────────────────────────────── */

/**
 * @brief Try items one by one with an async function; return first success.
 *
 * fn(item) must return `Promise<Result<T,E>>` where Result has `.is_ok()`.
 * Returns the first ok result, or the last error if all fail.
 *
 * Uses `std::move(*this)` ownership transfer through the .then() chain.
 * Items and fn are stored by value — zero heap allocation in the combinator
 * itself (only what the Promise internals need).
 *
 * @code
 *   // Try each resolved DNS address until one connects
 *   auto r = xpp::try_next(std::move(addrs), [&](const SocketAddr& a) {
 *     return connect_with_conf(a.ip().c_str(), a.port(), conf.get());
 *   })();
 * @endcode
 */
template <class Items, class Func> struct TryNext {
  using Item = decltype(std::declval<Items &>()[0]);
  using P    = decltype(std::declval<Func &>()(std::declval<Item>()));

  Items  items;
  size_t idx;
  Func   fn;

  // .then() callback — template operator() avoids spelling Result<T,E>
  struct Then {
    TryNext next; ///< std::move(*this) — carries ownership through chain

    template <class R> P operator()(R &&r) {
      if (r.is_ok()) return xpp::resolve(std::forward<R>(r));
      if (next.idx >= next.items.size()) {
        return xpp::resolve(std::forward<R>(r)); // last one failed
      }
      return next(); // tail-recursive via Promise chain
    }
  };

  P operator()() {
    if (idx >= items.size()) {
      // Shouldn't reach here if caller checks for non-empty; return last item's
      // result as error fallback
      return fn(items[items.size() - 1]);
    }
    return fn(items[idx++]).then(Then{std::move(*this)});
  }
};

/// Factory for automatic type deduction.
template <class Items, class Func>
TryNext<typename std::decay<Items>::type, typename std::decay<Func>::type> try_next(Items &&items,
                                                                                    Func  &&fn) {
  using I = typename std::decay<Items>::type;
  using F = typename std::decay<Func>::type;
  return {std::forward<I>(items), 0, std::forward<F>(fn)};
}

} // namespace xpp

#endif // XPP_PROMISE_UTILS_H
