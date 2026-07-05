# Notify

Reusable multi-waiter notification primitive. Unlike oneshot, Notify can be
used repeatedly — after `notify_one()` wakes a waiter, a subsequent call to
`notified()` will park the coroutine until the next notification.

Think "manual condvar": a sender can signal waiting coroutines without
sending data.

## Example

```cpp
#include <xpp/sync/notify.h>

xpp::sync::Notify n;

// Coroutine 1: waits for signal
co_await n.notified();  // suspends until notify_one() or notify_waiters()

// Coroutine 2: also waits
co_await n.notified();  // suspends

// Sender: wake one
n.notify_one();    // wakes coroutine 1 (or 2)

// Sender: wake all
n.notify_waiters(); // wakes all remaining waiters
```

## When notify arrives before notified()

If a worker thread calls `notify_one()` before any coroutine has called
`notified()`, the notification is accumulated as a pending count. The next
`notified()` call resolves immediately without suspending.

```cpp
Notify n;

// Worker thread (no event loop)
std::thread([&] {
    n.notify_one();
}).detach();

// Event loop coroutine
co_await n.notified();  // resolves immediately — notification was pending
```

## API

| Method | Returns | Description |
| -------- | --------- | ------------- |
| `notified()` | `Promise<void>` | Wait for the next notification. |
| `notify_one()` | `void` | Wake one waiting coroutine. |
| `notify_waiters()` | `void` | Wake ALL waiting coroutines. |

## Internals

Uses `loom::Mutex<std::vector<PromiseResolver<void>>>` for the waiter list.
An atomic pending counter handles the "notify before notified" race.

```text
notified() → m_pending > 0? → resolve immediately (lock-free fast path)
          → lock → push resolver → unlock → wait

notify_one() → lock → pop resolver → unlock → resolve
            → waiters empty? → m_pending++ (accumulate)
```

## Thread safety

`PromiseResolver::resolve()` uses `Arc<ArcWeak>` + atomic CAS internally.
The pending counter is atomic. Cross-thread wakeup flows through
`PromiseWaker::wake()` → `xEventLoopPost()` for cross-thread notification.
