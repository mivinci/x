/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * fiber.c - Fiber implementation for Unix (Linux / macOS / BSD).
 *
 * Context switching (setjmp—based, since commit XXXXXXX):
 *
 *   _setjmp / _longjmp capture only the program counter, stack pointer,
 *   and callee-saved registers. They explicitly do NOT save or restore
 *   signal masks, making fiber migration across threads safe.  This
 *   replaces the ucontext API, which macOS deprecated as of 10.6
 *   ("No longer supported") and whose uc_sigmask field causes
 *   cross-thread correctness issues.
 *
 * First entry into a newly-created fiber uses a small per-arch inline
 * asm trampoline to switch the stack pointer and jump to the fiber's
 * trampoline routine.  All subsequent suspend / resume cycles use
 * _setjmp / _longjmp.
 *
 * Stack allocation:
 *   mmap(MAP_PRIVATE | MAP_ANONYMOUS) with PROT_NONE guard page.
 *   The guard page (lowest page of the mapping) catches stack overflow
 *   with SIGSEGV before it corrupts adjacent memory.
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
 * Portability notes:
 *   - _setjmp / _longjmp: POSIX.1-2001 (BSD extension). Always available
 *     on macOS / FreeBSD. On glibc requires _DEFAULT_SOURCE.
 *   - mmap / mprotect / munmap: POSIX.1-2001.
 *   - sysconf(_SC_PAGESIZE): POSIX.1-2001.
 *
 * Windows support:
 *   See fiber_win.c — the public API (fiber.h) is identical on both
 *   platforms. Windows uses CreateFiber / SwitchToFiber / DeleteFiber.
 */

/* _setjmp is a BSD extension. glibc >= 2.19 enables it by default
 * with _DEFAULT_SOURCE; macOS / FreeBSD expose it unconditionally.
 *
 * NOTE: We explicitly undef _FORTIFY_SOURCE because glibc's FORTIFY
 * adds a stack-bounds check to _longjmp that considers a longjmp from
 * one mmap'd stack to another "stack frame corruption".  In our
 * fiber-switch use case this is safe: each fiber has its own valid
 * mmap'd stack, and we explicitly manage the switch.  Disabling
 * FORTIFY for this file lets _setjmp / _longjmp work as the OS
 * intended — bare PC/SP/callee-saved register save/restore. */
#ifdef _FORTIFY_SOURCE
#undef _FORTIFY_SOURCE
#endif
#define _DEFAULT_SOURCE 1

/* ASAN cannot track our manual asm stack-switch + cross-stack _longjmp —
 * it would report false-positive SEGV on valid fiber transitions.
 * Disable ASAN instrumentation for the three hot-path functions. */
#if defined(__has_feature)
  #if __has_feature(address_sanitizer)
    #define X_FIBER_NO_ASAN __attribute__((no_sanitize("address")))
  #endif
#elif defined(__SANITIZE_ADDRESS__)
  #define X_FIBER_NO_ASAN __attribute__((no_sanitize("address")))
#endif
#ifndef X_FIBER_NO_ASAN
  #define X_FIBER_NO_ASAN
#endif

#include <x/base/fiber.h>

#include <setjmp.h>

/* On glibc, ASAN intercepts _longjmp at runtime (not compile time) and
 * crashes when jumping between independent mmap'd stacks.  Call the
 * internal __longjmp directly — ASAN only intercepts the public names
 * (longjmp, _longjmp, siglongjmp), not __longjmp. */
#ifdef __linux__
extern void __longjmp(jmp_buf __env, int __val) __attribute__((__noreturn__));
#define X_LONGJMP(env, val) __longjmp(env, val)
#else
#define X_LONGJMP(env, val) _longjmp(env, val)
#endif

#include <stdlib.h>
#include <stdint.h>

/* ── POSIX headers ─────────────────────────────────────────────────── */

#include <sys/mman.h>
#include <unistd.h>

/* ── Constants ──────────────────────────────────────────────────────── */

/** Default fiber stack size: 64 KiB — matches kj / capnproto. */
#define X_FIBER_DEFAULT_STACK 65536

/* ── Internal struct ────────────────────────────────────────────────── */

struct xFiber_ {
  void      *stack;       /* mmap base (guard page start), NULL for main */
  size_t     stack_size;  /* usable stack size in bytes */
  void      *stack_base;  /* guard page end = usable stack start         */
  jmp_buf    jmp;         /* saved execution state (PC / SP / regs)     */
  xFiberProc proc;        /* user entry point (NULL for main fiber)      */
  void      *proc_arg;    /* opaque argument passed to proc              */
  xFiber     parent;      /* parent fiber (NULL for root); yield target */
  int        first_switch;/* 1 = never entered yet                       */
};

/* ── Thread-local current fiber ─────────────────────────────────────── */

static __thread struct xFiber_ *tl_fiber = NULL;

/* ── Fiber trampoline ──────────────────────────────────────────────────
 *
 * Every child fiber begins execution here. The asm first-switch path
 * in xFiberSwitch() sets SP to the fiber's stack and branches to this
 * function. tl_fiber is already set to the fiber descriptor so the
 * trampoline can reach proc / proc_arg.
 *
 * When the user's proc returns, the trampoline longjmps back to the
 * main fiber (or parent) which continues the event loop. */

X_FIBER_NO_ASAN
static void fiber_trampoline(void) {
  struct xFiber_ *f = tl_fiber;
  if (!f) {
    /* TLS corruption — unrecoverable. */
    abort();
  }
  f->proc(f->proc_arg);
  /* proc returned cleanly — fiber is done. Longjmp back to whoever
   * switched into us (parent if nested, otherwise main). */
  struct xFiber_ *back = f->parent
      ? (struct xFiber_ *)f->parent
      : (struct xFiber_ *)xFiberMain();
  X_LONGJMP(back->jmp, 1);
  abort(); /* NOTREACHED */
}

/* ── First-switch inline asm helpers ─────────────────────────────────── */

X_FIBER_NO_ASAN
static void fiber_first_switch_asm(struct xFiber_ *f) {
  /* Stack grows downward — sp starts at the HIGH end of the usable region.
   * Must be 16-byte aligned (ABI requirement for both ARM64 and x86-64). */
  void *sp = (char *)f->stack_base + f->stack_size;
  sp = (void *)((uintptr_t)sp & ~(uintptr_t)15ULL);

#if defined(__aarch64__)
  __asm__ volatile(
      "mov sp, %0\n\t"    /* point SP into the fiber's mmap'd stack */
      "mov fp, xzr\n\t"   /* clear frame pointer (fresh stack frame) */
      "mov lr, xzr\n\t"   /* clear link register (no return address) */
      "br   %1"           /* branch to fiber_trampoline              */
      : : "r"(sp), "r"((void *)fiber_trampoline)
      : "memory");
#elif defined(__x86_64__)
  __asm__ volatile(
      "movq %0, %%rsp\n\t" /* point RSP into the fiber's mmap'd stack */
      "xorq %%rbp, %%rbp\n\t" /* clear frame pointer                  */
      "jmpq *%1"              /* jump to fiber_trampoline             */
      : : "r"(sp), "r"((void *)fiber_trampoline)
      : "memory");
#else
  (void)sp;
  #error "Unsupported architecture for fiber first-switch via _setjmp"
#endif

  __builtin_unreachable();
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
   * stack == NULL signals xFiberDestroy() to skip munmap.
   * jmp_buf is lazily populated by the first _setjmp in xFiberSwitch(). */
  tl_main_fiber->stack       = NULL;
  tl_main_fiber->first_switch = 0;  /* main fiber goes through the normal path */

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

  f->stack        = mem;
  f->stack_size   = stack_size;
  f->stack_base   = (char *)mem + page_size;
  f->proc         = proc;
  f->proc_arg     = arg;
  f->parent       = (xFiber)tl_fiber;
  f->first_switch = 1;  /* will use asm path on first xFiberSwitch */

  /* No makecontext — the first xFiberSwitch will do an asm stack-switch
   * into fiber_trampoline, which then reads proc / proc_arg from TLS. */

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

X_FIBER_NO_ASAN
void xFiberSwitch(xFiber target) {
  struct xFiber_ *current = tl_fiber;
  struct xFiber_ *target_p = (struct xFiber_ *)target;

  if (!target_p) return;

  /* Implicit thread conversion — same semantics as before. */
  if (!current) {
    current = (struct xFiber_ *)xFiberMain();
  }

  if (target_p->first_switch) {
    /* ── First entry: asm stack-switch into fiber_trampoline ── */
    target_p->first_switch = 0;

    if (_setjmp(current->jmp) == 0) {
      /* Saved current (main/parent) state into its jmp_buf.
       * When the fiber longjmps back to us, _setjmp returns != 0
       * and execution resumes below. */
      tl_fiber = target_p;
      fiber_first_switch_asm(target_p);
      /* NOTREACHED — asm moves SP and branches away. */
      abort();
    }
    /* Fiber longjmp'd back — restore TLS. */
    tl_fiber = current;

  } else {
    /* ── Normal switch: save current, restore target ────────── */
    if (_setjmp(current->jmp) == 0) {
      tl_fiber = target_p;
      X_LONGJMP(target_p->jmp, 1);
      /* NOTREACHED. */
      abort();
    }
    /* Back from target — restore TLS. */
    tl_fiber = current;
  }
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
