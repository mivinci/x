/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * fiber.h - Cross-platform lightweight fibers (stackful coroutines).
 *
 * A minimal C API for user-space cooperative multitasking. Each fiber
 * runs on its own stack and yields voluntarily via xFiberSwitch().
 * Designed to integrate with the xEventLoop — a fiber suspends itself,
 * the event loop drives I/O, and the waker switches the fiber back in.
 *
 * Modeled after Windows Fiber API semantics, but unified across Unix
 * (mmap + _setjmp/_longjmp) and Windows (CreateFiber / SwitchToFiber).
 * Fiber migration across threads is safe: _setjmp does not capture
 * thread-local signal masks.
 *
 * Fiber stacks are allocated with a guard page (PROT_NONE) for stack
 * overflow detection. Default stack size: 64 KiB.
 *
 * API summary:
 *   xFiberMain()              — convert thread, get main fiber handle
 *   xFiberCreate(sz, fn, arg) — create a fiber (does not start)
 *   xFiberSwitch(target)      — suspend current, resume target
 *   xFiberYield()             — yield to parent fiber (or main)
 *   xFiberCurrent()           — get currently executing fiber
 *   xFiberDestroy(fiber)      — free a finished fiber
 *
 * C++11-safe (safe to include from C++).
 *
 * Portability:
 *   Unix    (Linux / macOS / BSD) — fiber.c: mmap + _setjmp/_longjmp
 *   Windows                       — fiber_win.c: CreateFiber / SwitchToFiber
 */

#ifndef X_BASE_FIBER_H
#define X_BASE_FIBER_H

#include <stddef.h>

#include <x/base/base.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle to a fiber.
 *
 * A fiber is a lightweight, cooperatively-scheduled execution context
 * with its own stack. The "main fiber" represents the thread's primary
 * stack (obtained via xFiberMain()).
 */
XDEF_HANDLE(xFiber);

/** @brief Fiber entry point. */
typedef void (*xFiberProc)(void *arg);

/* ═════════════════════════════════════════════════════════════════
 *  Lifecycle
 * ═════════════════════════════════════════════════════════════════ */

/**
 * @brief Convert the current thread and return the "main fiber" handle.
 *
 * Must be called once per thread before any xFiberCreate / xFiberSwitch.
 * Idempotent: subsequent calls return the same handle without side effects.
 *
 * The main fiber represents the thread's original stack. When a child
 * fiber suspends (via xFiberSwitch back to main), execution resumes in
 * the event loop or whatever code was running before the Switch.
 *
 * @return The main fiber handle. Never NULL on Unix.
 */
XCAPI(xFiber) xFiberMain(void);

/**
 * @brief Create a new fiber on an independent stack.
 *
 * Allocates a stack of @p stack_size bytes (with guard page on supported
 * platforms) and binds @p proc as the fiber's entry point. The fiber
 * does NOT begin execution — use xFiberSwitch() to enter.
 *
 * @param stack_size  Stack size in bytes. 0 uses the default (64 KiB).
 * @param proc       Function to execute on the fiber's stack.
 * @param arg        Opaque argument passed to @p proc.
 * @return A new fiber handle, or NULL on allocation failure.
 */
XCAPI(xFiber) xFiberCreate(size_t stack_size, xFiberProc proc, void *arg);

/**
 * @brief Delete a fiber and free its stack memory.
 *
 * Must NOT be called on the currently executing fiber. Safe to call
 * with NULL (no-op). The main fiber (from xFiberMain()) can be passed
 * — its stack pointer is NULL so only the handle struct is freed.
 *
 * @param fiber  Fiber to destroy. NULL is silently ignored.
 */
XCAPI(void) xFiberDestroy(xFiber fiber);

/* ═════════════════════════════════════════════════════════════════
 *  Switching
 * ═════════════════════════════════════════════════════════════════ */

/**
 * @brief Suspend the current fiber and resume @p target.
 *
 * The current fiber's register state (including stack pointer) is saved.
 * When another fiber later switches back, this call returns as if
 * nothing happened (much like setjmp returning 0 the first time and
 * non-zero on the way back).
 *
 * If the current thread has not been converted (xFiberMain() not called
 * yet), xFiberMain() is called implicitly.
 *
 * This is the ONLY switching primitive — there is no distinction
 * between "main → child" and "child → main".
 *
 * @param target  The fiber to resume. Must have been created via
 *                xFiberCreate() or xFiberMain().
 */
XCAPI(void) xFiberSwitch(xFiber target);

/**
 * @brief Yield the current fiber to its parent.
 *
 * If the current fiber was created inside another fiber (parent != NULL),
 * switches back to the parent so it can continue execution. Otherwise,
 * switches to the main fiber (the thread's event loop).
 *
 * This is the preferred way for a fiber to voluntarily suspend itself.
 * Used internally by PromiseWaker::park() for fiber-aware waiting and
 * by the xpp::fiber trampoline for cleanup.
 */
XCAPI(void) xFiberYield(void);

/**
 * @brief Return the currently executing fiber.
 *
 * @return The fiber handle, or NULL if xFiberMain() has not been
 *         called on this thread (thread is not fiber-capable).
 */
XCAPI(xFiber) xFiberCurrent(void);

#ifdef __cplusplus
}
#endif

#endif /* X_BASE_FIBER_H */
