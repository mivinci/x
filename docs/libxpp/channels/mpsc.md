# mpsc

Multi-producer, single-consumer channel. Each value is consumed exactly once.

```text
Sender ──┬──▶ [v0][v1][v2] ──▶ Receiver
Sender ──┘
```

Bounded and unbounded variants available:

| Variant | API | Backend |
| --------- | ----- | --------- |
| Bounded | `channel<T>(cap)` | Lock-free ring buffer (pre-allocated slots) |
| Unbounded | `channel<T>()` | Lock-free linked list (heap-allocated per send) |

## Bounded channel

```cpp
#include <xpp/sync/mpsc.h>
auto [tx, rx] = xpp::sync::mpsc::channel<int>(16);

// Async: suspends when buffer is full
co_await tx.send(42);

// Sync: returns immediately, fails when full
auto r = tx.try_send(99);
if (r.is_err()) { /* Full or Closed */ }

// Clone the sender for multiple producers
auto tx2 = tx;
co_await tx2.send(10);

// Receive
auto v = co_await rx.recv();  // Option<T> — none if closed
auto v = rx.try_recv();       // Result<T, TryRecvError>
```

## Unbounded channel

```cpp
auto [tx, rx] = xpp::sync::mpsc::channel<int>();  // no capacity argument

// Send never blocks — always succeeds
tx.send(42);       // void — no failure case
tx.try_send(99);   // bool — always true

// Same receiver API
auto v = co_await rx.recv();
auto v = rx.try_recv();  // Option<T>
```

## API

### Bounded `Sender<T>`

| Method | Returns | Description |
| -------- | --------- | ------------- |
| `send(T)` | `Promise<void>` | Async send. Suspends when full. |
| `try_send(T)` | `Result<Void, TrySendError<T>>` | Sync send. Fails with `Full` or `Closed`. |
| `close()` | — | Explicitly close. Wakes all waiters. |

### Bounded `Receiver<T>`

| Method | Returns | Description |
| -------- | --------- | ------------- |
| `recv()` | `Promise<Option<T>>` | Async receive. `none` when closed & empty. |
| `try_recv()` | `Result<T, TryRecvError>` | Sync receive. |

### Unbounded `UnboundedSender<T>`

| Method | Returns | Description |
| -------- | --------- | ------------- |
| `send(T)` | `void` | Always succeeds. |
| `try_send(T)` | `bool` | Always returns `true`. |

### Unbounded `UnboundedReceiver<T>`

| Method | Returns | Description |
| -------- | --------- | ------------- |
| `recv()` | `Promise<Option<T>>` | Async receive. |
| `try_recv()` | `Option<T>` | Sync receive. |

## Thread safety

- Bounded: lock-free send path (`fetch_add` CAS). Multiple threads can `try_send` concurrently.
- Unbounded: lock-free linked list. Multiple threads can `send` concurrently.
- Both: receiver is single-consumer (no concurrent recv).
