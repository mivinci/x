# fiber.h — Cross-Platform Lightweight Fibers

## Introduction

`fiber.h` provides a minimal C API for user-space cooperative multitasking — stackful coroutines (fibers) with their own call stack. Designed to integrate with `xEventLoop`: a fiber suspends itself, the event loop drives I/O, and the waker switches the fiber back in.

Modeled after the Windows Fiber API (`CreateFiber` / `SwitchToFiber`), but unified across Unix (`mmap` + `swapcontext` + `makecontext`/`setcontext`) and Windows (`CreateFiberEx` / `SwitchToFiber` / `DeleteFiber`).

## Design Philosophy

1. **Minimal Surface** — Seven functions. No scheduler, no message passing, no preemption. Fibers yield voluntarily via `xFiberSwitch()` or `xFiberYield()`. Higher-level scheduling is the caller's responsibility (e.g., event loop + waker integration).

2. **Independent Stacks** — Each fiber gets its own stack with a guard page (`PROT_NONE` on Unix, OS-managed on Windows). Stack overflow triggers `SIGSEGV` / access violation deterministically instead of corrupting adjacent memory.

3. **Thread-Local** — All operations are single-threaded per fiber set. `xFiberSwitch()` must only switch between fibers on the same thread. The TLS slot (`tl_fiber`) isolates independent fiber sets across threads with zero synchronization.

4. **API Parity** — The public API has identical signatures and semantics on Unix and Windows. Platform differences are confined to the implementation files (`fiber.c` / `fiber_win.c`).

5. **Parent-Child Chain** — Each fiber records its parent at creation time. `xFiberYield()` switches back to the parent (or the main fiber if none), enabling nested fiber hierarchies — a child fiber can `.wait()` a grandchild promise and get the result directly, without bouncing through the main fiber.

## Architecture

```
                    ┌─────────────┐
                    │ xFiberMain  │  ← Convert thread, get main fiber
                    └──────┬──────┘
                           │ (parent = NULL)
         ┌─────────────────┼─────────────────┐
         │                 │                 │
  ┌──────▼───────┐  ┌──────▼───────┐  ┌──────▼───────┐
  │  Fiber A     │  │  Fiber B     │  │  Fiber C     │
  │  (64 KiB)    │  │  (64 KiB)    │  │  (128 KiB)   │
  │  parent=main │  │  parent=main │  │  parent=A    │
  └──────────────┘  └──────────────┘  └──────────────┘

                                     Fiber C's parent = A:
                                     xFiberYield() in C → resumes A

xFiberSwitch(A):  main → A → main
xFiberSwitch(B):  main → B → main
xFiberYield() in C: C → A (skips main)
A spawns C: A → C → A (C's yield goes back to A)

All fibers share one thread. Only one fiber runs at a time.
```

### Stack Layout (Unix)

```
High addr
┌──────────────────────────┐
│  usable stack (RW)       │  ← sp starts here, grows downward
│  default 64 KiB          │
├──────────────────────────┤  ← stack_base
│  guard page (PROT_NONE)  │  ← touch → SIGSEGV
└──────────────────────────┘  ← stack (mmap return)
Low addr
```

## API Reference

### Types

| Type | Description |
| --- | --- |
| `xFiber` | Opaque handle. Represents either a main fiber (thread) or a child fiber. |
| `xFiberProc` | `typedef void (*xFiberProc)(void *arg)`. Fiber entry point. |

### Functions

| Function | Signature | Description |
| --- | --- | --- |
| `xFiberMain` | `xFiber xFiberMain(void)` | Convert the current thread. Idempotent. Returns the main fiber handle. |
| `xFiberCreate` | `xFiber xFiberCreate(size_t stack_size, xFiberProc proc, void *arg)` | Create a fiber on a new stack. Does NOT start execution. Returns `NULL` on failure. Also records the current fiber as `parent` for `xFiberYield()`. |
| `xFiberDestroy` | `void xFiberDestroy(xFiber fiber)` | Delete a finished fiber and free its stack. Safe with `NULL`. |
| `xFiberSwitch` | `void xFiberSwitch(xFiber target)` | Suspend current fiber, resume `target`. Implicitly calls `xFiberMain()` if needed. |
| `xFiberYield` | `void xFiberYield(void)` | Suspend current fiber and switch back to its parent (falling back to main if no parent). This is the primitive used by higher-level APIs (`PromiseContext::park()`, `xpp::fiber` trampoline) — `xFiberSwitch(xFiberMain())` is almost never what you want. |
| `xFiberCurrent` | `xFiber xFiberCurrent(void)` | Return the currently executing fiber, or `NULL` if unconverted. |

### Lifecycle

```
xFiberMain()    → main fiber handle
xFiberCreate()  → child fiber (stack allocated, not started; parent = current)
xFiberSwitch()  → start / yield / resume (target can be any fiber)
xFiberYield()   → suspend current, resume parent (no target needed)
xFiberDestroy() → free stack, free descriptor

A fiber that finishes its proc() MUST switch back to main
(or to another fiber) — never return to the uc_link (NULL),
which has undefined behavior.

Nested fibers:
  main → fiber A → fiber B
  B calls xFiberYield() → resumes A (B's parent)
  A calls xFiberYield() → resumes main (A's parent)
  xFiberSwitch() can still jump to any fiber, bypassing the parent chain.
```

## Usage Examples

### Basic Round Trip

```c
#include <assert.h>
#include <x/base/fiber.h>

static bool visited = false;

static void my_proc(void *arg) {
    xFiber *main = (xFiber *)arg;
    visited = true;
    xFiberSwitch(*main);  /* yield back to caller */
}

int main(void) {
    xFiber main = xFiberMain();
    xFiber child = xFiberCreate(0, my_proc, &main);
    assert(child != NULL);

    xFiberSwitch(child);
    assert(visited);

    xFiberDestroy(child);
    return 0;
}
```

### Multiple Yields (Generator Pattern)

```c
static void counter_proc(void *arg) {
    int *ctx = (int *)arg;
    for (int i = 0; i < (*ctx); i++) {
        xFiberSwitch(g_main);  /* yield after each increment */
    }
    xFiberSwitch(g_main);  /* final yield */
}

/* main: drive the fiber N times */
for (int i = 0; i < N; i++) {
    xFiberSwitch(fiber);
    /* fiber just yielded, ctx->counter has been incremented */
}
```

### Fiber Chaining

```c
/* Fiber A spawns Fiber B, B yields back to A via xFiberYield(). */
static void proc_b(void *arg) {
    int *val = (int *)arg;
    *val = 42;
    xFiberYield();  /* resume parent (A) */
}

static void proc_a(void *arg) {
    int result = 0;
    xFiber b = xFiberCreate(0, proc_b, &result); /* b.parent = A */
    xFiberSwitch(b);        /* A → B */
    /* B yielded back → result is 42 now */
    assert(result == 42);
    xFiberDestroy(b);
    xFiberYield();          /* resume main */
}

/* Main: */
xFiberSwitch(a);  /* main → A */
/* A back: fiber completed */
xFiberDestroy(a);
```

### Yield Without Knowing Parent

```c
/* Any fiber can call xFiberYield() — no need to track who the parent is. */
static void worker(void *arg) {
    int *counter = (int *)arg;
    for (int i = 0; i < 10; i++) {
        (*counter)++;
        xFiberYield();  /* back to whoever spawned me */
    }
}
```

## Platform Notes

### Unix (Linux / macOS / BSD)

| Aspect | Detail |
| --- | --- |
| Stack allocation | `mmap(MAP_PRIVATE | MAP_ANONYMOUS)` |
| Guard page | `mprotect(PROT_NONE)` on the bottom page |
| First entry | `makecontext` + `setcontext` |
| Yield / resume | `swapcontext` (atomically saves current ucontext and restores target; POSIX-blessed for cross-stack switching) |
| macOS arm64 note | `makecontext` variadic args cannot pass 64-bit pointers — trampoline reads `proc`/`proc_arg` from the fiber descriptor via TLS |

### Windows

| Aspect | Detail |
| --- | --- |
| Stack allocation | `CreateFiberEx` with `FIBER_FLAG_FLOAT_SWITCH` (preserves FPU/SSE/AVX) |
| Guard page | OS-managed via `VirtualAlloc` |
| First entry | `SwitchToFiber` enters the trampoline automatically |
| Yield / resume | `SwitchToFiber` (kernel-assisted fiber dispatcher on x64) |

## Architecture Integration

### With xEventLoop

The fiber API is designed to integrate with `xEventLoop` for async I/O:

```
Fiber suspends in Promise::wait()
  → xFiberYield() — switch back to parent (or main)
  → xEventLoopRun(ONCE) — process I/O events
  → Promise resolves, waker fires
  → xFiberSwitch(fiber) — resume the waiting fiber
```

This is the core mechanism for `xpp::fiber()`.

### Thread Safety

- `xFiberSwitch()`: **Single-thread only** — must switch between fibers on the same thread
- `xFiberCreate()` / `xFiberDestroy()`: **Single-thread** — operates on the calling thread's fiber set
- `xFiberCurrent()`: **Thread-safe** — returns the calling thread's fiber
- `xFiberMain()`: **Thread-safe** — idempotent per thread

## Diagnostics

| Condition | Behavior |
| --- | --- |
| `xFiberSwitch(NULL)` | Silent no-op (returns immediately) |
| `xFiberDestroy(current_fiber)` | Silent no-op (returns immediately) |
| Fiber proc returns without switching | `abort()` — fibers are a deterministic system and undefined transitions must fail hard |
| `xFiberCreate` allocation failure | Returns `NULL` |
| `xFiberYield` from main thread | Silent no-op |
| `xFiberYield` from root fiber (parent = NULL) | Switches to main fiber |

## See Also

- `event.h` — Event loop that drives fibers
- `promise.h` (libxpp) — `wait()` integrates with fibers for non-blocking I/O
- `fiber.h` (libxpp) — `xpp::fiber()` high-level API built on top of this module
