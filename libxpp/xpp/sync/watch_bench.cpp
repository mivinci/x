/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * watch_bench.cpp — Microbenchmarks for xpp::sync::watch.
 */

#include <benchmark/benchmark.h>

#include <string>

#include <xpp/promise.h>
#include <xpp/sync/watch.h>

// ── send (sync) throughput ─────────────────────────────────────────

static void BM_Watch_Send(benchmark::State &state) {
  auto [tx, rx] = xpp::sync::watch::channel(int{0});

  for (auto _ : state) {
    tx.send(42);
  }
  benchmark::DoNotOptimize(rx);
}
BENCHMARK(BM_Watch_Send);

// ── send + changed() round-trip ────────────────────────────────────

static void BM_Watch_RoundTrip(benchmark::State &state) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  for (auto _ : state) {
    auto [tx, mut_rx] = xpp::sync::watch::channel(int{0});
    tx.send(42);
    auto r = mut_rx.changed().await();
    benchmark::DoNotOptimize(r);
  }
}
BENCHMARK(BM_Watch_RoundTrip);

// ── has_changed() (no version change, sync fast path) ──────────────

static void BM_Watch_HasChanged(benchmark::State &state) {
  auto [tx, rx] = xpp::sync::watch::channel(int{0});
  tx.send(42);
  rx.borrow_and_update(); // sync seen version

  for (auto _ : state) {
    auto r = rx.has_changed();
    benchmark::DoNotOptimize(r);
  }
}
BENCHMARK(BM_Watch_HasChanged);

// ── borrow (peek without version tracking) ─────────────────────────

static void BM_Watch_Borrow(benchmark::State &state) {
  auto [tx, rx] = xpp::sync::watch::channel(int{42});

  for (auto _ : state) {
    auto ref = rx.borrow();
    benchmark::DoNotOptimize(ref);
  }
  benchmark::DoNotOptimize(tx);
}
BENCHMARK(BM_Watch_Borrow);

// ── string round-trip ──────────────────────────────────────────────

static void BM_Watch_String(benchmark::State &state) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  for (auto _ : state) {
    auto [tx, mut_rx] = xpp::sync::watch::channel(std::string("hello"));
    tx.send(std::string("world!"));
    auto r = mut_rx.changed().await();
    benchmark::DoNotOptimize(r);
  }
}
BENCHMARK(BM_Watch_String);

// ── Main (provided by benchmark::benchmark_main) ────────────────────
