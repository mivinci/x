# Notify

Reusable multi-waiter notification primitive. Unlike oneshot, Notify can be
used repeatedly — after `notify_one()` wakes a waiter, a subsequent call to
`notified()` will park the coroutine until the next notification.

Think "manual condvar": a sender can signal waiting coroutines without
sending data.

## Example — `.await()`

```cpp
#include <xpp/sync/notify.h>

xpp::EventLoop loop;
xpp::WaitScope scope(loop);

xpp::sync::Notify n;

// Waiter 1: waits for signal
n.notified().await();  // suspends (fiber) or blocks + drives loop

// Waiter 2: also waits
n.notified().await();

// Sender: wake one
n.notify_one();    // wakes waiter 1 (or 2)

// Sender: wake all
n.notify_waiters(); // wakes all remaining waiters
```

With fiber — non-blocking:

```cpp
xpp::fiber([]() {
  xpp::sync::Notify n;

  std::thread([&] { n.notify_one(); }).detach();

  n.notified().await();  // fiber suspends, event loop continues
  printf("woken up\n");
}).await();
```

## Example — `co_await` (C++20)

```cpp
xpp::sync::Notify n;
co_await n.notified();  // suspends until notify_one() or notify_waiters()
n.notify_one();         // wake one
n.notify_waiters();     // wake all
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

n.notified().await();  // resolves immediately — notification was pending
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
