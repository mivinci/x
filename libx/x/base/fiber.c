/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * fiber.c - Fiber implementation for Unix (Linux / macOS / BSD).
 *
 * Context switching:
 *   _setjmp / _longjmp — fast, saves only callee-saved registers and
 *     SP/FP/PC. Does NOT save the signal mask (unlike setjmp/longjmp),
 *     avoiding a syscall per switch. Used for all fiber ↔ main
 *     transitions after the initial entry.
 *
 *   makecontext / setcontext — only called once per fiber, at first
 *     entry. makecontext configures a ucontext_t with the fiber's
 *     stack and entry point; setcontext loads the full machine context
 *     (including SP) and jumps. After the first switch-in, subsequent
 *     yield/resume cycles use _setjmp/_longjmp exclusively.
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
 *   - _setjmp / _longjmp: POSIX.1-2001. Available on all Unix.
 *   - makecontext / setcontext: POSIX.1-2001, deprecated on macOS 10.6+
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

#include <stdbool.h>
#include <stdlib.h>

/* ── POSIX headers ─────────────────────────────────────────────────── */

#include <setjmp.h>
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
  jmp_buf    jmp;         /* _setjmp save point (registers + SP/FP/PC)   */
  ucontext_t uctx;        /* makecontext state (initial entry only)       */
  bool       started;     /* true after first switch-in (use _longjmp)   */
  void      *stack_base;  /* guard page end = usable stack start         */
  xFiberProc proc;        /* user entry point (NULL for main fiber)       */
  void      *proc_arg;    /* opaque argument passed to proc               */
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
  tl_main_fiber->started = true; /* already "running", no makecontext entry */
  tl_main_fiber->stack    = NULL;

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
  struct xFiber_ *current = tl_fiber;
  struct xFiber_ *target_p = (struct xFiber_ *)target;

  if (!target_p) return;

  /* Implicit thread conversion: if xFiberMain() hasn't been called
   * yet, do it now. This makes xFiberSwitch() safe to call from a
   * "bare" thread (e.g. a freshly-spawned pthread that hasn't been
   * prepared). */
  if (!current) {
    current = (struct xFiber_ *)xFiberMain();
  }

  /* ── Save current context ───────────────────────────────────────
   * _setjmp returns 0 on the save path (first call), and non-zero
   * on the restore path (when another fiber longjmp's back).
   *
   * TLS fixup (tl_fiber = current) below is needed because
   * _longjmp lands back here, but tl_fiber may have been set to
   * any value by the fiber that switched to us. We correct it. */

  if (_setjmp(current->jmp) == 0) {
    /* Save path: about to enter `target` */

    tl_fiber = target;

    if (target_p->started) {
      /* Target has run before — use _longjmp to restore its saved
       * register state (including its stack pointer). */
      _longjmp(target_p->jmp, 1);
    }

    /* First entry into target — use setcontext to load the full
     * machine context from makecontext initialization. This sets
     * SP, PC, and callee-saved registers for the new stack.
     * setcontext() does not return. */
    target_p->started = true;
    setcontext(&target_p->uctx);

    /* NOTREACHED — setcontext does not return */
  }

  /* Restore path: somebody switched back to `current`.
   * Fix tl_fiber which may have been clobbered by the switcher. */
  tl_fiber = current;
}

xFiber xFiberCurrent(void) {
  return (xFiber)tl_fiber;
}

#ifdef __APPLE__
#pragma clang diagnostic pop
#endif
