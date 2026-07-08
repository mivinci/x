/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * notify_bench.cpp — Microbenchmarks for xpp::sync::Notify.
 */

#include <benchmark/benchmark.h>

#include <vector>

#include <xpp/promise.h>
#include <xpp/sync/notify.h>

// ── notify_one → notified (wake: 1 waiter) ──────────────────────────

static void BM_Notify_NotifyOneNotified(benchmark::State &state) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  for (auto _ : state) {
    xpp::sync::Notify n;
    auto p = n.notified();
    n.notify_one();
    p.await();
  }
}
BENCHMARK(BM_Notify_NotifyOneNotified);

// ── notified fast path (pre-notified, no lock) ─────────────────────

static void BM_Notify_PreNotified(benchmark::State &state) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  for (auto _ : state) {
    xpp::sync::Notify n;
    n.notify_one();           // accumulate pending count
    auto p = n.notified();    // fast path: consume without lock
    p.await();
  }
}
BENCHMARK(BM_Notify_PreNotified);

// ── notify_waiters → N notified (wake: N waiters) ──────────────────

static void BM_Notify_NotifyWaiters(benchmark::State &state) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  static constexpr int kN = 8;
  for (auto _ : state) {
    xpp::sync::Notify n;
    std::vector<xpp::Promise<void>> waiters;
    waiters.reserve(kN);
    for (int i = 0; i < kN; ++i) waiters.push_back(n.notified());
    n.notify_waiters();
    for (auto &p : waiters) p.await();
  }
}
BENCHMARK(BM_Notify_NotifyWaiters);

// ── Main (provided by benchmark::benchmark_main) ────────────────────
