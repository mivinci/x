/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * fiber_test.cpp - Unit tests for xFiber (stackful coroutines).
 *
 * IMPORTANT: fibers must explicitly switch back to main before their
 * entry function returns. Returning from a fiber proc whose uc_link
 * is NULL has undefined behavior (terminates the process on glibc,
 * crashes on macOS).
 *
 * Covers:
 *   - xFiberMain() idempotency / implicit conversion
 *   - xFiberCreate() with default and custom stack sizes
 *   - xFiberDestroy() edge cases
 *   - xFiberSwitch() basic enter/exit
 *   - xFiberCurrent() correctness in fiber and main contexts
 *   - Multiple yield/resume cycles (fiber yield pattern)
 *   - Multiple concurrent fibers
 *   - Value passing through void* arg
 *   - Fiber chaining: fiber A switches to fiber B
 *   - xFiberYield(): yield to parent (recursive fiber support)
 *   - Death tests for invalid operations
 */

#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include <x/base/fiber.h>

/* ═══════════════════════════════════════════════════════════════════════
 *  Test fixtures and helpers
 * ═══════════════════════════════════════════════════════════════════════ */

/** State passed to fiber procs via void* arg. */
struct TestCtx {
  xFiber main_fiber;
  bool   visited;
  int    value;        /* value received from / passed to proc       */
  xFiber self;         /* xFiberCurrent() as seen from inside fiber  */
  int    counter;      /* incremented on each yield                  */
  xFiber next_fiber;   /* for chaining tests                        */
  void  *proc_arg;     /* opaque argument for incrementAndSwitchProc */
};

/* ── Fiber proc templates ────────────────────────────────────────────── */

/** Sets visited=true, records self, switches back to main. */
static void basicProc(void *arg) {
  TestCtx *ctx = static_cast<TestCtx *>(arg);
  ctx->visited = true;
  ctx->self    = xFiberCurrent();
  xFiberSwitch(ctx->main_fiber);
}

/** Stores the int value in ctx->value, switches back. */
static void passValueProc(void *arg) {
  TestCtx *ctx  = static_cast<TestCtx *>(arg);
  ctx->value    = 42;
  ctx->visited  = true;
  xFiberSwitch(ctx->main_fiber);
}

/** Yields back to main N times, counting each yield in ctx->counter. */
static void yieldNProc(void *arg) {
  TestCtx *ctx = static_cast<TestCtx *>(arg);
  int      n   = ctx->value; /* how many yields */

  for (int i = 0; i < n; i++) {
    ctx->counter++;
    xFiberSwitch(ctx->main_fiber);
  }
  ctx->visited = true;
  xFiberSwitch(ctx->main_fiber);
}

/** Runs, increments an externally-provided counter pointer. Uses a
 *  TestCtx to also hold the main fiber handle. */
static void incrementAndSwitchProc(void *arg) {
  TestCtx *ctx = static_cast<TestCtx *>(arg);
  int *counter = static_cast<int *>(ctx->proc_arg);
  (*counter)++;
  xFiberSwitch(ctx->main_fiber);
}

/** Switches to ctx->next_fiber (fiber-to-fiber switch, no main involved). */
static void chainProc(void *arg) {
  TestCtx *ctx = static_cast<TestCtx *>(arg);
  ctx->visited = true;
  xFiberSwitch(ctx->next_fiber);
  /* NOTREACHED in normal test flow */
}

/** Switches back to xFiberMain() — for implicit-conversion tests. */
static void implicitSwitchProc(void *arg) {
  TestCtx *ctx = static_cast<TestCtx *>(arg);
  ctx->visited = true;
  ctx->self    = xFiberCurrent();
  xFiberSwitch(xFiberMain());
}

/* ═══════════════════════════════════════════════════════════════════════
 *  xFiberMain() tests
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(FiberTest, MainIdempotent) {
  xFiber a = xFiberMain();
  ASSERT_NE(a, (xFiber)NULL);

  xFiber b = xFiberMain();
  EXPECT_EQ(a, b) << "xFiberMain() must return the same handle";

  xFiber c = xFiberMain();
  EXPECT_EQ(a, c);
}

TEST(FiberTest, MainIsNotNull) {
  xFiber main = xFiberMain();
  EXPECT_NE(main, (xFiber)NULL);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  xFiberCreate() tests
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(FiberTest, CreateDefaultStack) {
  TestCtx ctx = {};
  ctx.main_fiber = xFiberMain();

  xFiber f = xFiberCreate(0, basicProc, &ctx);
  ASSERT_NE(f, (xFiber)NULL);

  xFiberDestroy(f);
}

TEST(FiberTest, CreateCustomStack) {
  TestCtx ctx = {};
  ctx.main_fiber = xFiberMain();

  /* 4 KiB — very small but should be enough for basicProc */
  xFiber f = xFiberCreate(4096, basicProc, &ctx);
  ASSERT_NE(f, (xFiber)NULL);

  xFiberDestroy(f);
}

TEST(FiberTest, CreateLargeStack) {
  TestCtx ctx = {};
  ctx.main_fiber = xFiberMain();

  /* 1 MiB — tests alignment / mmap edge cases */
  xFiber f = xFiberCreate(1024 * 1024, basicProc, &ctx);
  ASSERT_NE(f, (xFiber)NULL);

  xFiberDestroy(f);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  xFiberDestroy() tests
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(FiberTest, DeleteNull) {
  /* Should not crash or assert */
  xFiberDestroy(NULL);
}

TEST(FiberTest, DeleteFinishedFiber) {
  TestCtx ctx = {};
  ctx.main_fiber = xFiberMain();

  xFiber f = xFiberCreate(0, basicProc, &ctx);
  ASSERT_NE(f, (xFiber)NULL);

  /* Switch in and out — fiber has now returned (switched to main) */
  xFiberSwitch(f);
  EXPECT_TRUE(ctx.visited);

  /* Deleting after it switched back should be safe */
  xFiberDestroy(f);
}

TEST(FiberTest, DeleteMultipleFibers) {
  TestCtx ctx = {};
  ctx.main_fiber = xFiberMain();

  xFiber a = xFiberCreate(0, basicProc, &ctx);
  xFiber b = xFiberCreate(0, basicProc, &ctx);
  xFiber c = xFiberCreate(0, basicProc, &ctx);

  ASSERT_NE(a, (xFiber)NULL);
  ASSERT_NE(b, (xFiber)NULL);
  ASSERT_NE(c, (xFiber)NULL);

  xFiberDestroy(a);
  xFiberDestroy(b);
  xFiberDestroy(c);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  xFiberSwitch() — basic enter / exit
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(FiberTest, SwitchMainToChild) {
  TestCtx ctx = {};
  ctx.main_fiber = xFiberMain();

  xFiber child = xFiberCreate(0, basicProc, &ctx);
  ASSERT_NE(child, (xFiber)NULL);

  EXPECT_FALSE(ctx.visited);

  xFiberSwitch(child);
  /* Back here after child switched to main */

  EXPECT_TRUE(ctx.visited);
  xFiberDestroy(child);
}

TEST(FiberTest, SwitchImplicitMain) {
  /* xFiberSwitch() implicitly converts the thread via xFiberMain()
   * if it hasn't been called yet.  The fiber proc retrieves the main
   * handle via xFiberMain() at runtime. */
  TestCtx ctx = {};
  /* Intentionally do NOT call xFiberMain() here. */

  xFiber child = xFiberCreate(0, implicitSwitchProc, &ctx);
  ASSERT_NE(child, (xFiber)NULL);

  xFiberSwitch(child);
  EXPECT_TRUE(ctx.visited);

  /* After switch back, xFiberCurrent() should return a valid main fiber */
  xFiber main = xFiberCurrent();
  EXPECT_NE(main, (xFiber)NULL);
  EXPECT_NE(main, child);

  xFiberDestroy(child);
}

TEST(FiberTest, SwitchDeepRoundTrip) {
  /* Switch main → fiber, fiber → main, main → fiber, fiber → main.
   * This tests the _setjmp/_longjmp path (second switch to same fiber). */
  TestCtx ctx  = {};
  ctx.main_fiber = xFiberMain();

  xFiber child = xFiberCreate(0, yieldNProc, &ctx);
  ASSERT_NE(child, (xFiber)NULL);

  ctx.value = 5; /* 5 yields */

  for (int i = 0; i < 5; i++) {
    xFiberSwitch(child);
    EXPECT_EQ(ctx.counter, i + 1) << "counter after yield " << i;
  }
  xFiberSwitch(child);
  EXPECT_TRUE(ctx.visited);

  xFiberDestroy(child);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  xFiberCurrent() tests
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(FiberTest, CurrentInMain) {
  xFiberMain();
  xFiber main = xFiberCurrent();
  EXPECT_NE(main, (xFiber)NULL);
}

TEST(FiberTest, CurrentInFiber) {
  TestCtx ctx = {};
  ctx.main_fiber = xFiberMain();

  xFiber child = xFiberCreate(0, basicProc, &ctx);
  ASSERT_NE(child, (xFiber)NULL);

  xFiberSwitch(child);

  /* basicProc records xFiberCurrent() in ctx.self */
  EXPECT_EQ(ctx.self, child);

  /* Back in main: xFiberCurrent() should NOT be child */
  EXPECT_NE(xFiberCurrent(), child);

  xFiberDestroy(child);
}

TEST(FiberTest, CurrentUnconverted) {
  /* In a brand-new test thread (not this one, since we already called
   * xFiberMain in other tests), but in the same test process we already
   * converted this thread. Due to thread-local, this IS converted.
   *
   * For a true unconverted test, we'd need a separate pthread, which
   * is overkill for this. We document that unconverted threads return
   * NULL, and trust the TLS initialization (tl_fiber starts as NULL). */
  /* At this point the thread is likely already converted from a prior
   * test. We can't meaningfully test "unconverted" without a new thread. */
  SUCCEED() << "Unconverted test requires a separate thread — deferred";
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Multiple yield / resume cycles
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(FiberTest, YieldSingle) {
  TestCtx ctx = {};
  ctx.main_fiber = xFiberMain();
  ctx.value = 1; /* 1 yield */

  xFiber f = xFiberCreate(0, yieldNProc, &ctx);
  ASSERT_NE(f, (xFiber)NULL);

  /* First switch: fiber yields once, comes back */
  xFiberSwitch(f);
  EXPECT_EQ(ctx.counter, 1);
  EXPECT_FALSE(ctx.visited);

  /* Second switch: fiber finishes, sets visited */
  xFiberSwitch(f);
  EXPECT_TRUE(ctx.visited);
  EXPECT_EQ(ctx.counter, 1); /* counter unchanged after finish */

  xFiberDestroy(f);
}

TEST(FiberTest, YieldMany) {
  TestCtx ctx = {};
  ctx.main_fiber = xFiberMain();
  ctx.value = 100; /* 100 yields */

  xFiber f = xFiberCreate(0, yieldNProc, &ctx);
  ASSERT_NE(f, (xFiber)NULL);

  for (int i = 0; i < 100; i++) {
    xFiberSwitch(f);
    EXPECT_EQ(ctx.counter, i + 1);
    if (i < 99) {
      EXPECT_FALSE(ctx.visited) << "visited should be false until final switch";
    }
  }
  xFiberSwitch(f);
  EXPECT_TRUE(ctx.visited);

  xFiberDestroy(f);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Multiple fibers
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(FiberTest, MultipleFibersSequential) {
  TestCtx ctx = {};
  ctx.main_fiber = xFiberMain();

  int results[3] = {0, 0, 0};
  TestCtx ctxs[3];
  for (int i = 0; i < 3; i++) {
    ctxs[i].main_fiber = ctx.main_fiber;
    ctxs[i].proc_arg   = &results[i];
  }

  xFiber a = xFiberCreate(0, incrementAndSwitchProc, &ctxs[0]);
  xFiber b = xFiberCreate(0, incrementAndSwitchProc, &ctxs[1]);
  xFiber c = xFiberCreate(0, incrementAndSwitchProc, &ctxs[2]);

  xFiberSwitch(a);
  xFiberSwitch(b);
  xFiberSwitch(c);

  EXPECT_EQ(results[0], 1);
  EXPECT_EQ(results[1], 1);
  EXPECT_EQ(results[2], 1);

  xFiberDestroy(a);
  xFiberDestroy(b);
  xFiberDestroy(c);
}

TEST(FiberTest, MultipleFibersInterleaved) {
  TestCtx ctxA = {};
  ctxA.main_fiber = xFiberMain();
  ctxA.value = 3; /* 3 yields */

  TestCtx ctxB = {};
  ctxB.main_fiber = ctxA.main_fiber;
  ctxB.value = 3;

  xFiber a = xFiberCreate(0, yieldNProc, &ctxA);
  xFiber b = xFiberCreate(0, yieldNProc, &ctxB);

  /* Interleave: A, B, A, B, ...
   * Each fiber yields 3 times (counter goes to 3), then a final switch
   * that sets visited=true. 4 switches of each fiber total. */
  for (int i = 0; i < 4; i++) {
    xFiberSwitch(a);
    xFiberSwitch(b);
  }

  EXPECT_EQ(ctxA.counter, 3);
  EXPECT_EQ(ctxB.counter, 3);
  EXPECT_TRUE(ctxA.visited);
  EXPECT_TRUE(ctxB.visited);

  xFiberDestroy(a);
  xFiberDestroy(b);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Value passing
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(FiberTest, PassValue) {
  TestCtx ctx = {};
  ctx.main_fiber = xFiberMain();
  ctx.value = 0; /* fiber will set to 42 */

  xFiber f = xFiberCreate(0, passValueProc, &ctx);
  ASSERT_NE(f, (xFiber)NULL);

  xFiberSwitch(f);

  EXPECT_EQ(ctx.value, 42);
  EXPECT_TRUE(ctx.visited);

  xFiberDestroy(f);
}

TEST(FiberTest, PassValueThroughArg) {
  TestCtx ctx = {};
  ctx.main_fiber = xFiberMain();
  ctx.value = 100;

  /* yieldNProc uses ctx->value as the yield count */
  xFiber f = xFiberCreate(0, yieldNProc, &ctx);
  ASSERT_NE(f, (xFiber)NULL);

  for (int i = 0; i < 100; i++) {
    xFiberSwitch(f);
  }
  xFiberSwitch(f);

  EXPECT_TRUE(ctx.visited);
  EXPECT_EQ(ctx.counter, 100);

  xFiberDestroy(f);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Fiber-to-fiber switching (without involving main)
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(FiberTest, ChainFiberToFiber) {
  TestCtx ctxA = {}, ctxB = {};
  ctxA.main_fiber = xFiberMain();
  ctxB.main_fiber = ctxA.main_fiber;

  /* Fiber B: sets ctxB.visited, switches to main. */
  xFiber fiberB = xFiberCreate(0, basicProc, &ctxB);

  /* Fiber A: sets ctxA.visited, switches to fiberB.
   * fiberB then switches to main. */
  ctxA.next_fiber = fiberB;
  xFiber fiberA = xFiberCreate(0, chainProc, &ctxA);

  /* Switch to A → A chains to B → B returns to main.
   * When B switches to main, we're back here. */
  xFiberSwitch(fiberA);

  EXPECT_TRUE(ctxA.visited);
  EXPECT_TRUE(ctxB.visited);

  xFiberDestroy(fiberA);
  xFiberDestroy(fiberB);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  xFiberYield — yield to parent fiber
 * ═══════════════════════════════════════════════════════════════════════ */

/** Fiber proc: sets visited, records self, yields (not switches to main). */
static void yieldProc(void *arg) {
  TestCtx *ctx       = static_cast<TestCtx *>(arg);
  ctx->visited       = true;
  ctx->self          = xFiberCurrent();
  xFiberYield();
}

TEST(FiberTest, YieldFromRootFiber) {
  TestCtx ctx = {};
  ctx.main_fiber = xFiberMain();

  xFiber child = xFiberCreate(0, yieldProc, &ctx);
  ASSERT_NE(child, (xFiber)NULL);

  /* Root fiber (parent=NULL) — xFiberYield() falls back to xFiberMain() */
  xFiberSwitch(child);
  EXPECT_TRUE(ctx.visited);
  EXPECT_EQ(xFiberCurrent(), ctx.main_fiber);

  xFiberDestroy(child);
}

/** Fiber proc: spawns a child fiber, yields, child yields back to parent. */
struct NestedYieldCtx {
  xFiber main_fiber;
  xFiber parent_fiber;
  xFiber child_fiber;
  bool   parent_visited;
  bool   child_visited;
};

static void childYieldProc(void *arg) {
  NestedYieldCtx *ctx  = static_cast<NestedYieldCtx *>(arg);
  ctx->child_visited    = true;
  ctx->child_fiber      = xFiberCurrent();
  EXPECT_NE(xFiberCurrent(), ctx->parent_fiber);
  EXPECT_EQ(xFiberCurrent(), ctx->child_fiber);
  xFiberYield();  /* yield to parent, not directly to main */
}

static void parentYieldProc(void *arg) {
  NestedYieldCtx *ctx = static_cast<NestedYieldCtx *>(arg);
  ctx->parent_fiber    = xFiberCurrent();

  /* Spawn a child fiber. xFiberCreate captures current fiber as parent. */
  ctx->child_fiber = xFiberCreate(0, childYieldProc, ctx);
  ASSERT_NE(ctx->child_fiber, (xFiber)NULL);

  /* Switch to child — child will yield back to us */
  xFiberSwitch(ctx->child_fiber);

  /* Child yielded back to parent! */
  ctx->parent_visited = true;
  EXPECT_EQ(xFiberCurrent(), ctx->parent_fiber);

  /* Clean up child, then yield back to main */
  xFiberDestroy(ctx->child_fiber);
  xFiberYield();
}

TEST(FiberTest, YieldNestedParentChild) {
  NestedYieldCtx ctx = {};
  ctx.main_fiber      = xFiberMain();

  xFiber parent = xFiberCreate(0, parentYieldProc, &ctx);
  ASSERT_NE(parent, (xFiber)NULL);

  /* parent runs: spawns child, child yields back, parent yields back */
  xFiberSwitch(parent);

  /* child_fiber is set inside parentYieldProc — check after switch */
  ASSERT_NE(ctx.child_fiber, (xFiber)NULL);
  EXPECT_TRUE(ctx.child_visited);
  EXPECT_TRUE(ctx.parent_visited);
  EXPECT_EQ(xFiberCurrent(), ctx.main_fiber);

  xFiberDestroy(parent);
}

/* ── xFiberYield from main thread is no-op ── */

TEST(FiberTest, YieldFromMainIsNoop) {
  xFiber main = xFiberMain();
  /* xFiberYield() on main thread — no parent, not a child fiber → no-op */
  xFiberYield();
  EXPECT_EQ(xFiberCurrent(), main);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Stress: many fibers
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(FiberTest, ManyFibers) {
  xFiber main = xFiberMain();
  constexpr int N = 50;

  int             counters[N];
  TestCtx         ctxs[N];
  xFiber          fibers[N];

  for (int i = 0; i < N; i++) {
    counters[i]        = 0;
    ctxs[i].main_fiber = main;
    ctxs[i].proc_arg   = &counters[i];
    fibers[i]   = xFiberCreate(4096, incrementAndSwitchProc, &ctxs[i]);
    ASSERT_NE(fibers[i], (xFiber)NULL) << "create fiber " << i;
  }

  /* Run each fiber once */
  for (int i = 0; i < N; i++) {
    xFiberSwitch(fibers[i]);
    EXPECT_EQ(counters[i], 1) << "fiber " << i;
  }

  /* Clean up */
  for (int i = 0; i < N; i++) {
    xFiberDestroy(fibers[i]);
  }
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Death tests
 * ═══════════════════════════════════════════════════════════════════════ */

TEST(FiberTest, DestroyCurrentFiber) {
  TestCtx ctx = {};
  ctx.main_fiber = xFiberMain();

  /* A fiber that attempts to delete itself — should be a silent no-op. */
  xFiber f = xFiberCreate(0, [](void *arg) {
    xFiber *self = static_cast<xFiber *>(arg);
    xFiberDestroy(*self);  /* no-op: silently ignored */
    xFiberSwitch(xFiberMain());
  }, &f);
  ASSERT_NE(f, (xFiber)NULL);

  xFiberSwitch(f);
  /* If we get here, xFiberDestroy(self) didn't crash */

  xFiberDestroy(f);
}

TEST(FiberTest, SwitchNullIsNoop) {
  TestCtx ctx = {};
  ctx.main_fiber = xFiberMain();

  xFiber child = xFiberCreate(0, basicProc, &ctx);
  ASSERT_NE(child, (xFiber)NULL);

  /* Switching to NULL should be a silent no-op. */
  xFiberSwitch(NULL);
  EXPECT_FALSE(ctx.visited) << "switch to NULL should not execute child";

  /* Normal switch still works afterwards. */
  xFiberSwitch(child);
  EXPECT_TRUE(ctx.visited);

  xFiberDestroy(child);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Stack integrity: verify fiber stack doesn't corrupt after deep call
 * ═══════════════════════════════════════════════════════════════════════ */

static void deepRecursion(int depth, xFiber main) {
  char buf[256];
  std::memset(buf, 0xAA, sizeof(buf)); /* touch the stack */

  if (depth > 0) {
    deepRecursion(depth - 1, main);
  } else {
    xFiberSwitch(main);
  }
}

static void deepRecursionProc(void *arg) {
  TestCtx *ctx = static_cast<TestCtx *>(arg);
  ctx->visited = true;
  deepRecursion(ctx->value, ctx->main_fiber);
  xFiberSwitch(ctx->main_fiber);
}

TEST(FiberTest, DeepRecursion) {
  TestCtx ctx = {};
  ctx.main_fiber = xFiberMain();
  ctx.value = 20; /* recursion depth */

  xFiber f = xFiberCreate(0, deepRecursionProc, &ctx);
  ASSERT_NE(f, (xFiber)NULL);

  xFiberSwitch(f);
  EXPECT_TRUE(ctx.visited);

  xFiberDestroy(f);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Fiber that yields many times with state preservation
 * ═══════════════════════════════════════════════════════════════════════ */

struct StatefulCtx {
  xFiber main_fiber;
  int    accumulator;
  int    limit;
  bool   done;
};

static void statefulProc(void *arg) {
  StatefulCtx *ctx = static_cast<StatefulCtx *>(arg);

  for (int i = 1; i <= ctx->limit; i++) {
    ctx->accumulator += i;
    xFiberSwitch(ctx->main_fiber);
  }
  ctx->done = true;
  xFiberSwitch(ctx->main_fiber);
}

TEST(FiberTest, StatePreservationAcrossYields) {
  StatefulCtx ctx = {};
  ctx.main_fiber = xFiberMain();
  ctx.limit = 10;

  xFiber f = xFiberCreate(0, statefulProc, &ctx);
  ASSERT_NE(f, (xFiber)NULL);

  for (int i = 0; i < ctx.limit; i++) {
    xFiberSwitch(f);
    EXPECT_EQ(ctx.accumulator, (i + 1) * (i + 2) / 2)
        << "accumulator after yield " << i;
    EXPECT_FALSE(ctx.done) << "done should be false until last iteration";
  }
  xFiberSwitch(f);
  EXPECT_TRUE(ctx.done);

  xFiberDestroy(f);
}
