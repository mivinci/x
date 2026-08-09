/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_combinators.h - xpp::all() and xpp::race() combinators.
 *
 * all(Promise<Ts>...) waits for all input promises and collects results
 * into a std::tuple. All-void inputs return Promise<void>.
 *
 * race(Promise<T>, Promise<T>...) resolves with the first ready promise,
 * discarding all others. Homogeneous types only.
 *
 * C++11-compatible. Header-only.
 */
#ifndef XPP_PROMISE_COMBINATORS_H
#define XPP_PROMISE_COMBINATORS_H

#include <array>
#include <tuple>
#include <type_traits>
#include <utility>

#include <xpp/option.h>
#include <xpp/own.h>
#include <xpp/panic.h>
#include <xpp/promise_node.h>
#include <xpp/void.h>

namespace xpp {

/* ── Internal: extract node from a moved Promise ──────────────────── */

namespace _ {

template <class U> _::OwnPromiseNode<U> _extract_node(Promise<U> &&p) {
  return std::move(p.m_node);
}

/* ── AllTuplePromiseNode<Ts...> ───────────────────────────────────── */
/*
 * Heterogeneous all. Stores one PromiseNode per input type. Polls all
 * not-done children with the same waker. When all are done, collects
 * results into a tuple.
 */
template <class... Ts>
class AllTuplePromiseNode : public PromiseNode<std::tuple<typename FixVoid<Ts>::Type...>> {
public:
  using ResultType          = std::tuple<typename FixVoid<Ts>::Type...>;
  static constexpr size_t N = sizeof...(Ts);

  explicit AllTuplePromiseNode(Promise<Ts> &&...promises)
      : m_children{_extract_node(std::move(promises))...} {}

  Option<ResultType> poll(const PromiseContext &cx) override {
    poll_all(cx, std::index_sequence_for<Ts...>{});
    if (m_remaining == 0) {
      return Option<ResultType>(collect(std::index_sequence_for<Ts...>{}));
    }
    return none;
  }

private:
  std::tuple<_::OwnPromiseNode<Ts>...>              m_children;
  std::tuple<Option<typename FixVoid<Ts>::Type>...> m_results;
  size_t                                            m_remaining = N;

  template <size_t... Is> void poll_all(const PromiseContext &w, std::index_sequence<Is...>) {
    (poll_one<Is>(w), ...);
  }

  template <size_t I> void poll_one(const PromiseContext &w) {
    if (!std::get<I>(m_results).is_some()) {
      auto r = std::get<I>(m_children)->poll(w);
      if (r.is_some()) {
        std::get<I>(m_results) = std::move(r);
        m_remaining--;
      }
    }
  }

  template <size_t... Is> ResultType collect(std::index_sequence<Is...>) {
    return ResultType(std::move(std::get<Is>(m_results).unwrap())...);
  }
};

/* ── AllVoidPromiseNode<N> ────────────────────────────────────────── */
/*
 * All-void special case. No results to collect — just count remaining.
 * Uses std::array instead of std::tuple for simplicity.
 */
template <size_t N> class AllVoidPromiseNode : public PromiseNode<void> {
public:
  template <class... Promises>
  explicit AllVoidPromiseNode(Promises &&...promises)
      : m_children{{_extract_node(std::forward<Promise<void>>(promises))...}}, m_done{},
        m_remaining(N) {
    static_assert(sizeof...(Promises) == N, "promise count mismatch");
  }

  Option<Void> poll(const PromiseContext &cx) override {
    for (size_t i = 0; i < N; i++) {
      if (!m_done[i]) {
        if (m_children[i]->poll(cx).is_some()) {
          m_done[i] = true;
          m_remaining--;
        }
      }
    }
    if (m_remaining == 0) return Option<Void>(Void{});
    return none;
  }

private:
  std::array<_::OwnPromiseNode<void>, N> m_children;
  std::array<bool, N>                    m_done;
  size_t                                 m_remaining;
};

/* ── RacePromiseNode<T, N> ────────────────────────────────────────── */
/*
 * Polls all children with the same waker. Returns the first Some result.
 * When one wins, the node (and all losing children) is destroyed by
 * the owning Promise.
 */
template <class T, size_t N> class RacePromiseNode : public PromiseNode<T> {
public:
  using ValueType = typename PromiseNode<T>::ValueType;

  template <class... Promises>
  explicit RacePromiseNode(Promises &&...promises)
      : m_children{{_extract_node(std::forward<Promise<T>>(promises))...}} {
    static_assert(sizeof...(Promises) == N, "promise count mismatch");
  }

  Option<ValueType> poll(const PromiseContext &cx) override {
    for (size_t i = 0; i < N; i++) {
      auto r = m_children[i]->poll(cx);
      if (r.is_some()) {
        return r;
      }
    }
    return none;
  }

private:
  std::array<_::OwnPromiseNode<T>, N> m_children;
};

/* ── RacePromiseNode<Void, N> ─────────────────────────────────────── */

template <size_t N> class RacePromiseNode<Void, N> : public PromiseNode<void> {
public:
  template <class... Promises>
  explicit RacePromiseNode(Promises &&...promises)
      : m_children{{_extract_node(std::forward<Promise<void>>(promises))...}} {
    static_assert(sizeof...(Promises) == N, "promise count mismatch");
  }

  Option<Void> poll(const PromiseContext &cx) override {
    for (size_t i = 0; i < N; i++) {
      if (m_children[i]->poll(cx).is_some()) {
        return Option<Void>(Void{});
      }
    }
    return none;
  }

private:
  std::array<_::OwnPromiseNode<void>, N> m_children;
};

} // namespace _

/* ── Public API: all ──────────────────────────────────────────────── */

/**
 * @brief Wait for all promises to resolve, collecting results into a tuple.
 *
 * Heterogeneous: each promise may have a different type.
 * - all(Promise<int>, Promise<string>) → Promise<tuple<int, string>>
 * - all(Promise<void>, Promise<int>)   → Promise<tuple<Void, int>>
 *
 * When ALL inputs are Promise<void>, returns Promise<void> instead of
 * Promise<tuple<Void, Void, ...>>.
 *
 * @code
 *   auto [a, b] = xpp::all(fetch_int(), fetch_str()).await();
 *   xpp::all(prefetch(0), prefetch(1)).await();  // all-void → void
 * @endcode
 */
template <class... Ts>
auto all(Promise<Ts>... promises)
  -> std::enable_if_t<(std::is_same<Ts, void>::value && ...), Promise<void>> {
  static_assert(sizeof...(Ts) > 0, "all() requires at least one promise");
  constexpr size_t N = sizeof...(Ts);
  auto *node = _::promise::allocate<_::AllVoidPromiseNode<N>>(nullptr, std::move(promises)...);
  return Promise<void>(_::OwnPromiseNode<void>(node));
}

template <class... Ts>
auto all(Promise<Ts>... promises)
  -> std::enable_if_t<!((std::is_same<Ts, void>::value && ...)),
                      Promise<std::tuple<typename FixVoid<Ts>::Type...>>> {
  static_assert(sizeof...(Ts) > 0, "all() requires at least one promise");
  auto *node = _::promise::allocate<_::AllTuplePromiseNode<Ts...>>(nullptr, std::move(promises)...);
  return Promise<std::tuple<typename FixVoid<Ts>::Type...>>(
    _::OwnPromiseNode<std::tuple<typename FixVoid<Ts>::Type...>>(node));
}

/* ── Public API: race ─────────────────────────────────────────────── */

/**
 * @brief Resolve with the first ready promise, discarding the rest.
 *
 * Homogeneous: all promises must have the same type T.
 * When one promise resolves, the others are destroyed (their destructors
 * run — e.g., TimerPromiseNode stops its timer).
 *
 * @code
 *   auto r = xpp::race(fetch(url), timeout(5000)).await();
 * @endcode
 */
template <class T, class... Rest> Promise<T> race(Promise<T> first, Promise<Rest>... rest) {
  static_assert(sizeof...(Rest) >= 0, "race() requires at least one promise");
  static_assert((std::is_same<Rest, T>::value && ...),
                "race() requires all promises to have the same type");
  constexpr size_t N    = 1 + sizeof...(Rest);
  auto            *node = _::promise::allocate<_::RacePromiseNode<typename FixVoid<T>::Type, N>>(
    nullptr, std::move(first), std::move(rest)...);
  return Promise<T>(_::OwnPromiseNode<T>(node));
}

} // namespace xpp

#endif // XPP_PROMISE_COMBINATORS_H
