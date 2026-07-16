/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * function.h - FnOnce<Signature>: a move-only callable invoked at most
 *              once (Rust FnOnce semantics in C++11).
 *
 * sizeof(FnOnce) = 3 × sizeof(void*) — inline storage for three
 * function pointers.  No vtable, no SBO, no dynamic dispatch.
 * Move is trivially movable.  Copy is deleted.
 *
 *   构造 — 堆分配 callable，存两个类型擦除函数指针
 *   调用 — operator() && 调用后自毁（invoke_impl 内部 delete）
 *   未调 — 析构 assert 告警 + 清理
 *
 * C++11-compatible.  Header-only.
 */

#ifndef XPP_FUNCTION_H
#define XPP_FUNCTION_H

#include <type_traits>
#include <utility>

#include <xpp/panic.h>

namespace xpp {

template <class Signature>
class FnOnce;

template <class Ret, class... Args>
class FnOnce<Ret(Args...)> {
  using Invoker = Ret (*)(void *, Args...);
  using Deleter = void (*)(void *);

public:
  FnOnce() = default;

  /**
   * @brief Construct from a callable.
   *
   * The callable is heap-allocated and exclusively owned.  No SBO —
   * simpler type-erasure at the cost of one allocation per construction.
   */
  template <class F,
            class = typename std::enable_if<
              !std::is_same<typename std::decay<F>::type, FnOnce>::value>::type>
  explicit FnOnce(F &&f) {
    using DF   = typename std::decay<F>::type;
    m_callable = new DF(std::forward<F>(f));
    m_invoke   = &invoke_impl<DF>;
    m_delete   = &delete_impl<DF>;
  }

  FnOnce(FnOnce &&other) noexcept
    : m_callable(other.m_callable),
      m_invoke(other.m_invoke),
      m_delete(other.m_delete) {
    other.m_callable = nullptr;
    other.m_invoke   = nullptr;
    other.m_delete   = nullptr;
  }

  FnOnce &operator=(FnOnce &&other) noexcept {
    if (this != &other) {
      if (m_delete) m_delete(m_callable);
      m_callable       = other.m_callable;
      m_invoke         = other.m_invoke;
      m_delete         = other.m_delete;
      other.m_callable = nullptr;
      other.m_invoke   = nullptr;
      other.m_delete   = nullptr;
    }
    return *this;
  }

  FnOnce(const FnOnce &)            = delete;
  FnOnce &operator=(const FnOnce &) = delete;

  /**
   * @brief Invoke and consume the callable.
   *
   * @pre m_invoke != nullptr (not default-constructed or already consumed).
   * @post All three internal pointers are null — the object is empty.
   */
  Ret operator()(Args... args) && {
    XPP_ASSERT(m_invoke, "FnOnce already consumed or default-constructed");
    void   *c   = m_callable;
    Invoker inv = m_invoke;
    m_callable  = nullptr;
    m_delete    = nullptr;
    m_invoke    = nullptr;
    return inv(c, std::forward<Args>(args)...);
  }

  /** True while a callable is held and has not been invoked yet. */
  bool is_some() const { return m_invoke != nullptr; }

  /** True when default-constructed, moved-out, or already consumed. */
  bool is_none() const { return m_invoke == nullptr; }

  ~FnOnce() {
    if (m_delete) {
      XPP_ASSERT(false, "FnOnce destroyed without being called");
      m_delete(m_callable);
    }
  }

private:
  /** Dispatch — call, delete, and return. */
  template <class DF>
  static Ret invoke_impl(void *self, Args... args) {
    return invoke_dispatch<DF>(self, typename std::is_void<Ret>::type{},
                               std::forward<Args>(args)...);
  }

  template <class DF>
  static void delete_impl(void *self) {
    delete static_cast<DF *>(self);
  }

  /* R = void — call + delete, no return. */
  template <class DF>
  static void invoke_dispatch(void *self, std::true_type /* is_void */, Args... args) {
    auto *pf = static_cast<DF *>(self);
    std::move(*pf)(std::forward<Args>(args)...);
    delete pf;
  }

  /* R != void — call, capture, delete, return. */
  template <class DF>
  static Ret invoke_dispatch(void *self, std::false_type /* is_void */, Args... args) {
    auto *pf = static_cast<DF *>(self);
    auto  r  = std::move(*pf)(std::forward<Args>(args)...);
    delete pf;
    return r;
  }

  void    *m_callable = nullptr;
  Invoker  m_invoke   = nullptr;
  Deleter  m_delete   = nullptr;
};

} // namespace xpp

#endif // XPP_FUNCTION_H
