# event.h — RAII Event Loop Wrapper

## Introduction

`event.h` provides C++ RAII wrappers for the libx event loop. Two classes with distinct responsibilities:

- **`EventLoop`** — Owns the `xEventLoop` handle. Creates on construction, destroys when out of scope. Move-only.
- **`WaitScope`** — Binds the loop to the current thread (`xEventLoopEnter`/`Leave`). Non-copyable, non-movable — tied to its scope.

This separation mirrors the C API where `xEventLoopCreate`/`Destroy` and `xEventLoopEnter`/`Leave` are independent operations. `EventLoop` manages the resource; `WaitScope` manages the thread binding.

## Design Philosophy

1. **Handle ownership vs. thread binding are separate concerns.** Creating a loop and binding it to a thread are two orthogonal operations. Collapsing them into a single RAII type would complicate sharing and lifetime management.

2. **WaitScope is scope-tied, not a movable object.** `xEventLoopEnter`/`Leave` form a stack-like pair. Moving the guard would leave the original scope without a corresponding `Leave`, violating the contract.

3. **The handle itself is thread-safe.** `stop()`, `wake()`, and `xEventLoopPost` can be called from any thread. `run()`, `xTimerStart`, and `xEventAdd` must be called from the entered thread.

4. **`EventLoop::current()` panics outside WaitScope.** This catches the common bug of calling `Promise::await()` without an active event loop binding.

5. **Fiber integration.** With `XPP_FIBER`, `PromiseWaker::park()` can suspend a fiber via `xFiberYield()` instead of blocking the thread with `X_RUN_ONCE`. The same WaitScope and EventLoop drive all fibers — no separate scheduler needed. See [`.await()` docs](promise/await.md).

## API Reference

### EventLoop

| Member | Description |
|---|---|
| `EventLoop()` | Create an event loop. `operator bool()` checks success. |
| `~EventLoop()` | Destroy the loop. Does NOT call `xEventLoopLeave`. |
| `EventLoop(EventLoop&&)` | Move ctor. Source becomes falsy. |
| `EventLoop& operator=(EventLoop&&)` | Move assignment. Old loop destroyed. |
| `void run(RunMode mode)` | Run the loop (blocks until stopped or idle). |
| `void stop()` | Stop a running loop (thread-safe). |
| `void wake()` | Wake from `epoll_wait`/`kevent` (thread-safe). |
| `xEventLoop handle()` | Access the underlying C handle for interop. |
| `operator bool()` | True if the loop was created successfully. |
| `static xEventLoop current()` | The loop bound to this thread. Panics outside WaitScope. |

### RunMode

| Value | Behavior |
|---|---|
| `RunMode::Default` | Block until `stop()` or no more active handles. |
| `RunMode::Once` | Single iteration, block until at least one event. |
| `RunMode::NoWait` | Single iteration, non-blocking poll. |

### WaitScope

| Member | Description |
|---|---|
| `WaitScope(const EventLoop&)` | Enter the loop. Binds it to the current thread. |
| `~WaitScope()` | Leave the loop. Unbinds the thread. |
| Non-copyable, non-movable | The enter/leave pair must stay in one scope. |

## Usage Examples

### Basic event loop

```cpp
#include <xpp/event.h>
#include <xpp/timer.h>

int main() {
    xpp::EventLoop loop;

    {
        xpp::WaitScope scope(loop);

        // One-shot: fire once after 100ms, then stop the loop
        xpp::Timer(100, 0, [&]() { loop.stop(); });

        loop.run();  // Blocks ~100ms, then timer fires → stop
    }
    // WaitScope leaves the loop here
}
```

### Interop with Promise\<T\>

```cpp
#include <xpp/event.h>
#include <xpp/promise.h>
#include <xpp/timer.h>

xpp::EventLoop loop;
{
    xpp::WaitScope scope(loop);

    auto r = xpp::PromiseResolver<int>::create();

    xpp::Timer(50, 0, [&]() { r.resolve(42); });

    int result = r.promise().await();  // EventLoop::current() succeeds
}
```

### Manual wake from another thread

```cpp
#include <thread>

xpp::EventLoop loop;
{
    xpp::WaitScope scope(loop);

    std::thread worker([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        loop.wake();   // Thread-safe: unblocks epoll_wait
        // or: loop.stop();
    });

    loop.run();
    worker.join();
}
```

## Comparison

| | `xpp::EventLoop` + `WaitScope` | `uv_loop_t` + `uv_run()` | `asio::io_context` |
|---|---|---|---|
| **Ownership** | RAII (move-only) | Manual alloc/free | RAII (copyable or moveable) |
| **Thread binding** | Explicit via WaitScope | Implicit on first call | Explicit via `run()` |
| **Wake from another thread** | `loop.wake()` | `uv_async_send` | `post()` |
| **Size** | `sizeof(void*)` (opaque handle) | ~1KB struct | Large (many members) |
| **Embeddability** | Zero deps beyond libx | libuv needed | Boost/standalone asio needed |

## Implementation Notes

### Storage

```cpp
class EventLoop {
    OwnedOpaquePointer<Destroy> m_loop;
};
```

`EventLoop` stores an `OwnedOpaquePointer<Destroy>` — essentially `Own<void, Destroy>` — where `Destroy` is a stateless deleter that calls `xEventLoopDestroy`. The handle itself is opaque; `operator bool()` delegates to `Own::operator bool()`.

### EventLoop::current()

```cpp
static xEventLoop current() {
    xEventLoop loop = xEventLoopCurrent();
    XPP_ASSERT(loop != nullptr, "EventLoop::current() called outside WaitScope");
    return loop;
}
```

On macOS/Linux, `xEventLoopCurrent()` reads a `__thread` / `thread_local` variable set by `xEventLoopEnter`. No global registry, no lookup — pure thread-local storage. The assert catches the case where no `WaitScope` is active on the calling thread.
