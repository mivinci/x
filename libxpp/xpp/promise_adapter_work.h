/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * promise_adapter_work.h - WorkAdapter for Promise.
 *
 * WorkAdapter<T, Func> bridges thread-pool work (xWorkSubmit) into
 * the Promise system. func() runs on a worker thread; resolve() is
 * called from there (thread-safe via ArcWeak).
 *
 * Used by xpp::work(fn).
 *
 * C++17-compatible. Header-only.
 */
#ifndef XPP_PROMISE_ADAPTER_WORK_H
#define XPP_PROMISE_ADAPTER_WORK_H

#include <utility>

#include <xpp/promise_adapter.h>

#include <x/base/event.h>

namespace xpp {

namespace _ {

/* ── WorkAdapter<T, Func> ────────────────────────────────────────── */

template <class T, class Func> class WorkAdapter {
public:
  WorkAdapter(PromiseResolver<T> &&r, Func &&fn) : m_ctx(new Ctx{std::move(r), std::move(fn)}) {
    m_work = xWorkSubmit(
      nullptr,
      [](void *a) -> void * {
        auto *ctx = static_cast<Ctx *>(a);
        ctx->resolver.resolve(ctx->func());
        return nullptr;
      },
      [](void *a, void *) { delete static_cast<Ctx *>(a); },
      [](void *a, void *) { delete static_cast<Ctx *>(a); }, m_ctx);
  }

  ~WorkAdapter() {
    if (m_work) xWorkCancel(m_work);
  }

  WorkAdapter(const WorkAdapter &)            = delete;
  WorkAdapter &operator=(const WorkAdapter &) = delete;
  WorkAdapter(WorkAdapter &&)                 = delete;
  WorkAdapter &operator=(WorkAdapter &&)      = delete;

private:
  struct Ctx {
    PromiseResolver<T> resolver;
    Func               func;
  };
  Ctx  *m_ctx;
  xWork m_work;
};

/* ── WorkAdapter<void, Func> — void specialization ─────────────────── */

template <class Func> class WorkAdapter<void, Func> {
public:
  WorkAdapter(PromiseResolver<void> &&r, Func &&fn) : m_ctx(new Ctx{std::move(r), std::move(fn)}) {
    m_work = xWorkSubmit(
      nullptr,
      [](void *a) -> void * {
        auto *ctx = static_cast<Ctx *>(a);
        ctx->func();
        ctx->resolver.resolve();
        return nullptr;
      },
      [](void *a, void *) { delete static_cast<Ctx *>(a); },
      [](void *a, void *) { delete static_cast<Ctx *>(a); }, m_ctx);
  }

  ~WorkAdapter() {
    if (m_work) xWorkCancel(m_work);
  }

  WorkAdapter(const WorkAdapter &)            = delete;
  WorkAdapter &operator=(const WorkAdapter &) = delete;
  WorkAdapter(WorkAdapter &&)                 = delete;
  WorkAdapter &operator=(WorkAdapter &&)      = delete;

private:
  struct Ctx {
    PromiseResolver<void> resolver;
    Func                  func;
  };
  Ctx  *m_ctx;
  xWork m_work;
};

} // namespace _

} // namespace xpp

#endif // XPP_PROMISE_ADAPTER_WORK_H
