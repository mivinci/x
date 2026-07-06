/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * oneshot_bench.cpp — Microbenchmarks for xpp::sync::oneshot.
 */

#include <benchmark/benchmark.h>

#include <string>

#include <xpp/promise.h>
#include <xpp/sync/oneshot.h>

// ── create + send (Sender throughput) ────────────────────────────────

static void BM_Oneshot_CreateSend(benchmark::State &state) {
  for (auto _ : state) {
    auto [tx, rx] = xpp::sync::oneshot::channel<int>();
    benchmark::DoNotOptimize(rx);
    tx.send(42);
  }
}
BENCHMARK(BM_Oneshot_CreateSend);

// ── full send+recv round-trip ────────────────────────────────────────

static void BM_Oneshot_RoundTrip(benchmark::State &state) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  for (auto _ : state) {
    auto [tx, rx] = xpp::sync::oneshot::channel<int>();
    tx.send(state.iterations());
    int val = std::move(rx).recv().wait();
    benchmark::DoNotOptimize(val);
  }
}
BENCHMARK(BM_Oneshot_RoundTrip);

// ── pre-resolved recv (value already sent, recv resolves immediately) ─

static void BM_Oneshot_PreResolvedRecv(benchmark::State &state) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  for (auto _ : state) {
    auto [tx, rx] = xpp::sync::oneshot::channel<int>();
    tx.send(42);
    int val = std::move(rx).recv().wait();
    benchmark::DoNotOptimize(val);
  }
}
BENCHMARK(BM_Oneshot_PreResolvedRecv);

// ── string round-trip ────────────────────────────────────────────────

static void BM_Oneshot_String(benchmark::State &state) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  for (auto _ : state) {
    auto [tx, rx] = xpp::sync::oneshot::channel<std::string>();
    tx.send(std::string("hello world!"));
    auto val = std::move(rx).recv().wait();
    benchmark::DoNotOptimize(val);
  }
}
BENCHMARK(BM_Oneshot_String);

// ── Main (provided by benchmark::benchmark_main) ────────────────────
