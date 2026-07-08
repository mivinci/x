/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * broadcast_bench.cpp — Microbenchmarks for xpp::sync::broadcast.
 */

#include <benchmark/benchmark.h>

#include <string>

#include <xpp/promise.h>
#include <xpp/sync/broadcast.h>

// ── try_send + try_recv (channel reuse) ────────────────────────────

static void BM_Broadcast_TrySendRecv(benchmark::State &state) {
  auto [tx, mut_rx] = xpp::sync::broadcast::channel<int>(16);
  benchmark::DoNotOptimize(mut_rx);

  for (auto _ : state) {
    tx.try_send(42);
    auto r = mut_rx.try_recv();
    benchmark::DoNotOptimize(r);
  }
}
BENCHMARK(BM_Broadcast_TrySendRecv);

// ── try_send + try_recv (per-iteration create + op) ────────────────

static void BM_Broadcast_TrySendRecv_Create(benchmark::State &state) {
  for (auto _ : state) {
    auto [tx, mut_rx] = xpp::sync::broadcast::channel<int>(16);
    benchmark::DoNotOptimize(mut_rx);
    tx.try_send(42);
    auto r = mut_rx.try_recv();
    benchmark::DoNotOptimize(r);
  }
}
BENCHMARK(BM_Broadcast_TrySendRecv_Create);

// ── async send + recv round-trip ───────────────────────────────────

static void BM_Broadcast_RoundTrip(benchmark::State &state) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  for (auto _ : state) {
    auto [tx, mut_rx] = xpp::sync::broadcast::channel<int>(4);
    tx.try_send(state.iterations());
    auto r = mut_rx.recv().await();
    benchmark::DoNotOptimize(r);
  }
}
BENCHMARK(BM_Broadcast_RoundTrip);

// ── subscribe + try_recv (channel reuse, subscribe per-iter) ──────

static void BM_Broadcast_SubscribeRecv(benchmark::State &state) {
  auto [tx, _] = xpp::sync::broadcast::channel<int>(8);
  tx.try_send(1);

  for (auto _ : state) {
    auto rx = tx.subscribe();
    auto r = rx.try_recv();
    benchmark::DoNotOptimize(r);
  }
}
BENCHMARK(BM_Broadcast_SubscribeRecv);

// ── string round-trip ──────────────────────────────────────────────

static void BM_Broadcast_String(benchmark::State &state) {
  xpp::EventLoop loop;
  xpp::WaitScope scope(loop);

  for (auto _ : state) {
    auto [tx, mut_rx] = xpp::sync::broadcast::channel<std::string>(4);
    tx.try_send(std::string("hello world!"));
    auto r = mut_rx.recv().await();
    benchmark::DoNotOptimize(r);
  }
}
BENCHMARK(BM_Broadcast_String);

// ── Main (provided by benchmark::benchmark_main) ────────────────────
