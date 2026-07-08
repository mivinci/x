/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * fiber.c - Fiber implementation for Unix (Linux / macOS / BSD).
 *
 * Context switching:
 *   swapcontext — atomically saves the current machine context and
 *     restores the target. POSIX.1-2001. Supports switching between
 *     independent stacks, which is required for fibers (the fiber
 *     stack is mmap'd separately from the main stack). Used for all
 *     fiber ↔ main transitions.
 *
 *   makecontext — called once per child fiber at creation time to
 *     configure the initial entry point (fiber_trampoline) on the
 *     fiber's own stack. Subsequent switches use swapcontext.
 *
 *   NOTE: _setjmp / _longjmp was originally used for performance
 *   (avoids saving the full machine context), but glibc fortification
 *   (FORTIFY_SOURCE) detects jumps to independent stacks as "uninitialized
 *   stack frame" and aborts. swapcontext is the POSIX-blessed way to
 *   switch between arbitrary stacks.
 *
 * Stack allocation:
 *   mmap(MAP_PRIVATE | MAP_ANONYMOUS) with PROT_NONE guard page.
 *   The guard page (lowest page of the mapping) catches stack overflow
 *   with SIGSEGV before it corrupts adjacent memory. On macOS, stack
 *   size is rounded up to the page boundary for proper alignment.
 *
 * Stack layout for each child fiber:
 *
 *     high addr
 *     ┌──────────────────────────┐
 *     │  usable stack (RW)       │  ← sp starts here, grows downward
 *     │  default 64 KiB          │
 *     ├──────────────────────────┤  ← stack_base
 *     │  guard page (PROT_NONE)  │  ← touch → SIGSEGV
 *     └──────────────────────────┘  ← stack (mmap return)
 *     low addr
 *
 * The main fiber (from xFiberMain()) has stack == NULL and uses the
 * thread's native stack. It cannot be munmap'd.
 *
 * Thread safety:
 *   All operations are single-threaded per fiber set. xFiberSwitch()
 *   must only switch between fibers on the same thread. The TLS slot
 *   (tl_fiber) is per-thread, so multiple threads can have
 *   independent fiber sets without synchronization.
 *
 * Portability notes:
 *   - swapcontext / makecontext: POSIX.1-2001, deprecated on macOS 10.6+
 *     but still functional. Wrapped with diagnostic suppression.
 *   - mmap / mprotect / munmap: POSIX.1-2001.
 *   - sysconf(_SC_PAGESIZE): POSIX.1-2001.
 *
 * Windows support:
 *   See fiber_win.c — the public API (fiber.h) is identical on both
 *   platforms. Windows uses CreateFiber / SwitchToFiber / DeleteFiber.
 */

/* ucontext.h on macOS requires _XOPEN_SOURCE to be set. Define it before
 * any system headers. */
#define _XOPEN_SOURCE

#include <x/base/fiber.h>

#include <stdlib.h>

/* ── POSIX headers ─────────────────────────────────────────────────── */

#include <sys/mman.h>
#include <unistd.h>

/* macOS deprecated ucontext functions — suppress the warning for the entire
 * file since every function (getcontext/makecontext/setcontext) triggers it. */
#ifdef __APPLE__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <ucontext.h>

/* ── Constants ──────────────────────────────────────────────────────── */

/** Default fiber stack size: 64 KiB — matches kj / capnproto. */
#define X_FIBER_DEFAULT_STACK 65536

/* ── Internal struct ────────────────────────────────────────────────── */

struct xFiber_ {
  void      *stack;       /* mmap base (guard page start), NULL for main */
  size_t     stack_size;  /* usable stack size in bytes */
  ucontext_t uctx;        /* saved machine context (SP / PC / regs)       */
  void      *stack_base;  /* guard page end = usable stack start         */
  xFiberProc proc;        /* user entry point (NULL for main fiber)       */
  void      *proc_arg;    /* opaque argument passed to proc               */
  xFiber     parent;      /* parent fiber (NULL for root); yield target  */
};

/* ── Thread-local current fiber ─────────────────────────────────────── */

static __thread struct xFiber_ *tl_fiber = NULL;

/* ── Fiber trampoline ──────────────────────────────────────────────────
 * macOS arm64 limitation: makecontext's variadic arguments cannot
 * reliably pass 64-bit pointers (arm64 va_arg for makecontext silently
 * truncates to 32 bits).  The workaround stores proc / arg in the
 * fiber descriptor and uses a fixed trampoline that reads them back.
 * tl_fiber is already set to the target fiber by xFiberSwitch() before
 * setcontext() is called, so the trampoline can find its descriptor
 * through the TLS slot. */

static void fiber_trampoline(void) {
  struct xFiber_ *f = tl_fiber;
  if (!f) {
    /* TLS corruption — unrecoverable.  The trampoline must run inside
     * a fiber whose descriptor was set by xFiberSwitch(). */
    abort();
  }
  f->proc(f->proc_arg);
  /* proc must call xFiberSwitch() instead of returning.  A bare return
   * would reach uc_link == NULL → undefined / crash. */
  abort();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Lifecycle
 * ═══════════════════════════════════════════════════════════════════════ */

xFiber xFiberMain(void) {
  static __thread struct xFiber_ *tl_main_fiber = NULL;

  if (tl_main_fiber) {
    return tl_main_fiber;
  }

  tl_main_fiber = (struct xFiber_ *)calloc(1, sizeof(struct xFiber_));
  if (!tl_main_fiber) {
    return NULL;
  }

  /* Main fiber uses the thread's native stack — no mmap needed.
   * stack == NULL signals xFiberDestroy() to skip munmap. */
  tl_main_fiber->stack = NULL;
  /* Initialize uctx so swapcontext can save/restore to it. */
  if (getcontext(&tl_main_fiber->uctx) != 0) {
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

  /* ── Allocate stack + guard page ──────────────────────────────── */
  long   page_size = sysconf(_SC_PAGESIZE);
  size_t total    = stack_size + (size_t)page_size;

  /* Round stack size up to page boundary for proper mmap alignment.
   * The actual usable stack may be slightly larger than requested. */
  if (total % (size_t)page_size != 0) {
    total = ((total / (size_t)page_size) + 1) * (size_t)page_size;
  }

  void *mem = mmap(NULL, total, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    return NULL;
  }

  /* Bottom page = guard (PROT_NONE). Stack overflow → SIGSEGV. */
  if (mprotect(mem, (size_t)page_size, PROT_NONE) != 0) {
    munmap(mem, total);
    return NULL;
  }

  /* ── Allocate fiber descriptor ────────────────────────────────── */
  struct xFiber_ *f =
      (struct xFiber_ *)calloc(1, sizeof(struct xFiber_));
  if (!f) {
    munmap(mem, total);
    return NULL;
  }

  f->stack      = mem;
  f->stack_size = stack_size;
  f->stack_base = (char *)mem + page_size;
  f->proc       = proc;
  f->proc_arg   = arg;
  f->parent     = (xFiber)tl_fiber;  /* NULL from main, parent fiber inside nested fiber */

  /* ── Initialize entry context (makecontext) ─────────────────────
   * makecontext/setcontext is used ONLY for the first switch-in.
   * All subsequent suspend/resume cycles use _setjmp/_longjmp. */

  if (getcontext(&f->uctx) != 0) {
    /* getcontext failure is exceedingly rare (only fails with invalid
     * ucp or on systems that don't implement it). Clean up. */
    munmap(mem, total);
    free(f);
    return NULL;
  }

  f->uctx.uc_stack.ss_sp   = f->stack_base;
  f->uctx.uc_stack.ss_size = stack_size;
  f->uctx.uc_link          = NULL; /* return to caller → NOTREACHED */
  makecontext(&f->uctx, (void (*)(void))fiber_trampoline, 0);

  return (xFiber)f;
}

void xFiberDestroy(xFiber handle) {
  if (!handle) return;

  struct xFiber_ *f = (struct xFiber_ *)handle;

  /* Never delete the currently running fiber. */
  if (f == tl_fiber) return;

  /* Main fiber has stack == NULL — only free the descriptor. */
  if (f->stack) {
    long   page_size = sysconf(_SC_PAGESIZE);
    size_t total    = f->stack_size + (size_t)page_size;
    if (total % (size_t)page_size != 0) {
      total = ((total / (size_t)page_size) + 1) * (size_t)page_size;
    }
    munmap(f->stack, total);
  }

  free(f);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Switching
 * ═══════════════════════════════════════════════════════════════════════ */

void xFiberSwitch(xFiber target) {
  struct xFiber_ *current  = tl_fiber;
  struct xFiber_ *target_p = (struct xFiber_ *)target;

  if (!target_p) return;

  /* Implicit thread conversion: if xFiberMain() hasn't been called
   * yet, do it now. This makes xFiberSwitch() safe to call from a
   * "bare" thread (e.g. a freshly-spawned pthread that hasn't been
   * prepared). */
  if (!current) {
    current = (struct xFiber_ *)xFiberMain();
  }

  /* swapcontext atomically saves the current machine context
   * (SP, PC, callee-saved registers) to current->uctx and restores
   * target_p->uctx. On first entry into a child fiber, target_p->uctx
   * was set up by makecontext; on subsequent entries, it holds the
   * state saved by the previous swapcontext out of that fiber.
   *
   * TLS fixup after swapcontext returns: tl_fiber may have been set
   * to any value by the fiber that switched to us. We correct it to
   * `current` (the fiber that just resumed). */
  tl_fiber = target_p;
  swapcontext(&current->uctx, &target_p->uctx);
  tl_fiber = current;
}

/**
 * @brief Yield to the parent fiber.
 *
 * If the current fiber has a parent (created inside another fiber),
 * switches to the parent. Otherwise, switches to the main fiber
 * (the thread's event loop). This enables nested fiber calls:
 * a fiber can spawn child fibers and have them yield back to it
 * instead of jumping all the way to the main stack.
 *
 * This is the preferred way to suspend a fiber — use inside
 * PromiseWaker::park() and trampoline cleanup instead of hardcoding
 * xFiberSwitch(xFiberMain()).
 */
void xFiberYield(void) {
  struct xFiber_ *cur = tl_fiber;
  if (!cur) return; /* not a fiber thread — nothing to yield */

  xFiber target = cur->parent ? cur->parent : xFiberMain();
  xFiberSwitch(target);
}

xFiber xFiberCurrent(void) {
  return (xFiber)tl_fiber;
}

#ifdef __APPLE__
#pragma clang diagnostic pop
#endif
