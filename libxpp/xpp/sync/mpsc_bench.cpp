/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * mpsc_bench.cpp — Microbenchmarks for xpp::sync::mpsc.
 */

#include <benchmark/benchmark.h>

#include <string>

#include <xpp/promise.h>
#include <xpp/sync/mpsc.h>

// ── bounded: try_send + try_recv (channel reuse) ───────────────────

static void BM_Mpsc_BoundedTry(benchmark::State &state) {
  auto [tx, rx] = xpp::sync::mpsc::channel<int>(16);
  benchmark::DoNotOptimize(rx);

  for (auto _ : state) {
    tx.try_send(42);
    auto r = rx.try_recv();
    benchmark::DoNotOptimize(r);
  }
}
BENCHMARK(BM_Mpsc_BoundedTry);

// ── bounded: try_send + try_recv (per-iteration create + op) ───────

static void BM_Mpsc_BoundedTry_Create(benchmark::State &state) {
  for (auto _ : state) {
    auto [tx, rx] = xpp::sync::mpsc::channel<int>(16);
    benchmark::DoNotOptimize(rx);
    tx.try_send(42);
    auto r = rx.try_recv();
    benchmark::DoNotOptimize(r);
  }
}
BENCHMARK(BM_Mpsc_BoundedTry_Create);

// ── bounded: async send + recv (pre-sent, no wakeup) ───────────────

static void BM_Mpsc_BoundedAsyncRoundTrip(benchmark::State &state) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  for (auto _ : state) {
    auto [tx, rx] = xpp::sync::mpsc::channel<int>(4);
    tx.try_send(42);
    int val = rx.recv().await().unwrap();
    benchmark::DoNotOptimize(val);
  }
}
BENCHMARK(BM_Mpsc_BoundedAsyncRoundTrip);

// ── unbounded: send + recv (per-iteration create + op) ─────────────

static void BM_Mpsc_UnboundedRoundTrip(benchmark::State &state) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  for (auto _ : state) {
    auto [tx, rx] = xpp::sync::mpsc::channel<int>();
    tx.send(42);
    int val = rx.recv().await().unwrap();
    benchmark::DoNotOptimize(val);
  }
}
BENCHMARK(BM_Mpsc_UnboundedRoundTrip);

// ── unbounded: string round-trip ───────────────────────────────────

static void BM_Mpsc_UnboundedString(benchmark::State &state) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  for (auto _ : state) {
    auto [tx, rx] = xpp::sync::mpsc::channel<std::string>();
    tx.send(std::string("hello world!"));
    auto val = rx.recv().await();
    benchmark::DoNotOptimize(val);
  }
}
BENCHMARK(BM_Mpsc_UnboundedString);

// ── Main (provided by benchmark::benchmark_main) ────────────────────
