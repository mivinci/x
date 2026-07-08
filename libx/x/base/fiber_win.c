/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * fiber_win.c - Fiber implementation for Windows.
 *
 * Uses the native Windows Fiber API (CreateFiber / SwitchToFiber /
 * DeleteFiber / ConvertThreadToFiber).  Semantics are identical to
 * the Unix implementation (fiber.c) — the public API (fiber.h)
 * is the same on both platforms.
 *
 * Context switching:
 *   SwitchToFiber() saves the current fiber's callee-saved registers
 *   (including SP) and restores the target's.  This is a kernel
 *   transition on x64 (it goes through the NT kernel fiber dispatcher),
 *   but still substantially faster than a thread context switch.
 *
 * Stack allocation:
 *   Windows manages fiber stacks internally via VirtualAlloc.  We
 *   pass the requested size to CreateFiberEx and let the OS handle
 *   guard pages (automatically added by VirtualAlloc for fiber stacks).
 *
 * Thread safety:
 *   Same rules as the Unix implementation — fibers are per-thread.
 *   xFiberSwitch() must only switch between fibers on the same thread.
 *   The TLS slot (tl_fiber) is per-thread via __declspec(thread).
 *
 * API mapping:
 *   xFiberMain()       → ConvertThreadToFiber(NULL)
 *   xFiberCreate()     → CreateFiberEx()
 *   xFiberSwitch()     → SwitchToFiber()
 *   xFiberDestroy()    → DeleteFiber()
 *   xFiberCurrent()    → tl_fiber (mirrors GetCurrentFiber())
 */

#include <x/base/fiber.h>

#include <stdbool.h>
#include <stdlib.h>

/* ── Windows headers ────────────────────────────────────────────── */

/* WIN32_LEAN_AND_MEAN: strip rarely-used Windows API to speed up build. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* ── Constants ──────────────────────────────────────────────────── */

/** Default fiber stack size: 64 KiB. */
#define X_FIBER_DEFAULT_STACK 65536

/* ── Internal struct ────────────────────────────────────────────── */

struct xFiber_ {
  LPVOID     handle;       /* CreateFiber / ConvertThreadToFiber handle */
  bool       is_main;      /* true for the main-fiber (thread) context   */
  size_t     stack_size;   /* requested stack size (0 for main / default) */
  xFiberProc proc;         /* user entry point (NULL for main fiber)      */
  void      *proc_arg;     /* opaque argument passed to proc              */
  xFiber     parent;       /* parent fiber (NULL for root); yield target  */
};

/* ── Thread-local current fiber ─────────────────────────────────── */

static __declspec(thread) struct xFiber_ *tl_fiber = NULL;

/* ── Fiber trampoline ─────────────────────────────────────────────
 * Windows CreateFiberEx calls this directly with the LPVOID arg.
 * We store proc/proc_arg in the fiber descriptor (retrieved via
 * tl_fiber) before calling the user's entry point.
 *
 * NOTE: the fiber descriptor *must* be in TLS before the first
 * SwitchToFiber returns, so xFiberCreate() sets tl_fiber temporarily
 * and restores it right after the switch. */

static VOID CALLBACK fiber_trampoline(LPVOID lpParameter) {
  struct xFiber_ *f = tl_fiber;
  if (!f) {
    /* TLS corruption — unrecoverable.  The trampoline must run inside
     * a fiber whose descriptor was set by xFiberSwitch(). */
    abort();
  }
  f->proc(lpParameter);
  /* proc must call xFiberSwitch() instead of returning. */
  abort();
}

/* ═══════════════════════════════════════════════════════════════════
 *  Lifecycle
 * ═══════════════════════════════════════════════════════════════════ */

xFiber xFiberMain(void) {
  static __declspec(thread) struct xFiber_ *tl_main_fiber = NULL;

  if (tl_main_fiber) {
    return tl_main_fiber;
  }

  tl_main_fiber = (struct xFiber_ *)calloc(1, sizeof(struct xFiber_));
  if (!tl_main_fiber) {
    return NULL;
  }

  /* ConvertThreadToFiber() fails if the thread is already a fiber.
   * This happens when xFiberCreate() is called before xFiberMain():
   * CreateFiberEx implicitly converts the thread.  In that case,
   * retrieve the existing handle via GetCurrentFiber(). */
  if (IsThreadAFiber()) {
    tl_main_fiber->handle = GetCurrentFiber();
  } else {
    tl_main_fiber->handle = ConvertThreadToFiber(NULL);
  }
  tl_main_fiber->is_main = true;

  if (!tl_main_fiber->handle) {
    /* Either ConvertThreadToFiber or GetCurrentFiber failed — extremely
     * unlikely but recoverable.  Clean up and return NULL. */
    free(tl_main_fiber);
    tl_main_fiber = NULL;
    return NULL;
  }

  tl_fiber = tl_main_fiber;
  return tl_main_fiber;
}

xFiber xFiberCreate(size_t stack_size, xFiberProc proc, void *arg) {
  if (stack_size == 0) {
    stack_size = X_FIBER_DEFAULT_STACK;
  }

  /* Allocate the fiber descriptor first so tl_fiber can point to it
   * during the trampoline's initial invocation. */
  struct xFiber_ *f =
      (struct xFiber_ *)calloc(1, sizeof(struct xFiber_));
  if (!f) {
    return NULL;
  }

  f->stack_size = stack_size;
  f->proc       = proc;
  f->proc_arg   = arg;
  f->is_main    = false;
  f->parent     = (xFiber)tl_fiber;  /* NULL from main, parent fiber inside nested fiber */

  /* CreateFiberEx with FIBER_FLAG_FLOAT_SWITCH: preserves the x87
   * FPU / SSE / AVX state on context switches, which is the safe
   * default for modern code that uses floating-point math. */
  f->handle = CreateFiberEx(
      (SIZE_T)stack_size,             /* commit size */
      (SIZE_T)stack_size,             /* reserve size (= commit, no growth) */
      FIBER_FLAG_FLOAT_SWITCH,        /* preserve FPU/SSE/AVX state */
      fiber_trampoline,
      NULL /* lpParameter — use tl_fiber->proc_arg instead */
  );

  if (!f->handle) {
    free(f);
    return NULL;
  }

  return (xFiber)f;
}

void xFiberDestroy(xFiber handle) {
  if (!handle) return;

  struct xFiber_ *f = (struct xFiber_ *)handle;

  /* Never delete the currently running fiber. */
  if (f == tl_fiber) return;

  /* Only delete Windows fiber handles (not the main fiber — that's
   * the owning thread's stack and is cleaned up by ExitThread). */
  if (!f->is_main) {
    DeleteFiber(f->handle);
  }

  free(f);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Switching
 * ═══════════════════════════════════════════════════════════════════ */

void xFiberSwitch(xFiber target) {
  struct xFiber_ *current  = tl_fiber;
  struct xFiber_ *target_p = (struct xFiber_ *)target;

  if (!target_p) return;

  /* Implicit thread conversion: if xFiberMain() hasn't been called
   * yet, do it now. */
  if (!current) {
    current = (struct xFiber_ *)xFiberMain();
  }

  /* target_p->handle should always be set (it was created by
   * CreateFiberEx).  If not, it's a programming error — silently
   * bail out instead of crashing. */
  if (!target_p->handle) return;

  tl_fiber = target_p;
  SwitchToFiber(target_p->handle);

  /* Control returns here when another fiber switches back to 'current'.
   * Fix tl_fiber which may have been clobbered by the switcher. */
  tl_fiber = current;
}

void xFiberYield(void) {
  struct xFiber_ *cur = tl_fiber;
  if (!cur) return; /* not a fiber thread — nothing to yield */

  xFiber target = cur->parent ? cur->parent : xFiberMain();
  xFiberSwitch(target);
}

xFiber xFiberCurrent(void) {
  return (xFiber)tl_fiber;
}
