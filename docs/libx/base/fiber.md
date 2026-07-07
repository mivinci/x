# fiber.h — Cross-Platform Lightweight Fibers

## Introduction

`fiber.h` provides a minimal C API for user-space cooperative multitasking — stackful coroutines (fibers) with their own call stack. Designed to integrate with `xEventLoop`: a fiber suspends itself, the event loop drives I/O, and the waker switches the fiber back in.

Modeled after the Windows Fiber API (`CreateFiber` / `SwitchToFiber`), but unified across Unix (`mmap` + `_setjmp`/`_longjmp` + `makecontext`/`setcontext`) and Windows (`CreateFiberEx` / `SwitchToFiber` / `DeleteFiber`).

## Design Philosophy

1. **Minimal Surface** — Six functions. No scheduler, no message passing, no preemption. Fibers yield voluntarily via `xFiberSwitch()`. Higher-level scheduling is the caller's responsibility (e.g., event loop + waker integration).

2. **Independent Stacks** — Each fiber gets its own stack with a guard page (`PROT_NONE` on Unix, OS-managed on Windows). Stack overflow triggers `SIGSEGV` / access violation deterministically instead of corrupting adjacent memory.

3. **Thread-Local** — All operations are single-threaded per fiber set. `xFiberSwitch()` must only switch between fibers on the same thread. The TLS slot (`tl_fiber`) isolates independent fiber sets across threads with zero synchronization.

4. **API Parity** — The six API functions have identical signatures and semantics on Unix and Windows. Platform differences are confined to the implementation files (`fiber.c` / `fiber_win.c`).

5. **Zero-Overhead Switching** — On Unix, `_setjmp`/`_longjmp` save only callee-saved registers (~20-30 ns). On Windows, `SwitchToFiber` is a kernel transition (~50-80 ns). No syscall, no signal-mask manipulation.

## Architecture

```
                    ┌─────────────┐
                    │  xFiberMain  │  ← Convert thread, get main fiber
                    └──────┬──────┘
                           │
         ┌─────────────────┼─────────────────┐
         │                 │                 │
  ┌──────▼──────┐  ┌──────▼──────┐  ┌──────▼──────┐
  │  Fiber A     │  │  Fiber B     │  │  Fiber C     │
  │  (64 KiB)    │  │  (64 KiB)    │  │  (128 KiB)   │
  └──────────────┘  └──────────────┘  └──────────────┘

xFiberSwitch(A):  main → A → main
xFiberSwitch(B):  main → B → main
xFiberSwitch(C):  main → C → main

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
| `xFiberCreate` | `xFiber xFiberCreate(size_t stack_size, xFiberProc proc, void *arg)` | Create a fiber on a new stack. Does NOT start execution. Returns `NULL` on failure. |
| `xFiberDestroy` | `void xFiberDestroy(xFiber fiber)` | Delete a finished fiber and free its stack. Safe with `NULL`. |
| `xFiberSwitch` | `void xFiberSwitch(xFiber target)` | Suspend current fiber, resume `target`. Implicitly calls `xFiberMain()` if needed. |
| `xFiberCurrent` | `xFiber xFiberCurrent(void)` | Return the currently executing fiber, or `NULL` if unconverted. |

### Lifecycle

```
xFiberMain()    → main fiber handle
xFiberCreate()  → child fiber (stack allocated, not started)
xFiberSwitch()  → start / yield / resume
xFiberDestroy() → free stack, free descriptor

A fiber that finishes its proc() MUST switch back to main
(or to another fiber) — never return to the uc_link (NULL),
which has undefined behavior.
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
/* Fiber A switches to Fiber B, which switches back to main.
   This avoids main ↔ child ↔ main bounce for A → B transitions. */
xFiberSwitch(fiber_a);
/* fiber_a → fiber_b → main  (no main involvement in A→B hop) */
```

## Platform Notes

### Unix (Linux / macOS / BSD)

| Aspect | Detail |
| --- | --- |
| Stack allocation | `mmap(MAP_PRIVATE | MAP_ANONYMOUS)` |
| Guard page | `mprotect(PROT_NONE)` on the bottom page |
| First entry | `makecontext` + `setcontext` |
| Yield / resume | `_setjmp` + `_longjmp` (no signal mask, no syscall) |
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
  → xFiberSwitch(main) — switch back to event loop
  → xEventLoopRun(ONCE) — process I/O events
  → Promise resolves, waker fires
  → xFiberSwitch(fiber) — resume the waiting fiber
```

This is the core mechanism for `xpp::fiber::await()` (planned).

### Thread Safety

- `xFiberSwitch()`: **Single-thread only** — must switch between fibers on the same thread
- `xFiberCreate()` / `xFiberDestroy()`: **Single-thread** — operates on the calling thread's fiber set
- `xFiberCurrent()`: **Thread-safe** — returns the calling thread's fiber
- `xFiberMain()`: **Thread-safe** — idempotent per thread

## Diagnostics

| Assertion | Trigger |
| --- | --- |
| `target != NULL` | `xFiberSwitch(NULL)` |
| `f != tl_fiber` | `xFiberDestroy(current_fiber)` |
| `fiber proc returned` | A child fiber's proc returned without calling `xFiberSwitch()` |

Both trigger `abort()` — fibers are a deterministic system and undefined transitions must fail hard.

## See Also

- `event.h` — Event loop that drives fibers
- `promise.h` (libxpp) — `wait()` integrates with fibers for non-blocking I/O
