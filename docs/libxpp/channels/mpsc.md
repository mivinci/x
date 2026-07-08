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

## Bounded channel — `.await()`

```cpp
#include <xpp/sync/mpsc.h>

xpp::EventLoop loop;
xpp::WaitScope scope(loop);

auto [tx, rx] = xpp::sync::mpsc::channel<int>(16);

// Async: .await() suspends (fiber) or blocks + drives loop
tx.send(42).await();

// Sync: returns immediately, fails when full
auto r = tx.try_send(99);
if (r.is_err()) { /* Full or Closed */ }

// Clone the sender for multiple producers
auto tx2 = tx;
tx2.send(10).await();

// Receive
auto v = rx.recv().await();   // Option<T> — none if closed
auto v = rx.try_recv();       // Result<T, TryRecvError>
```

With fiber — non-blocking:

```cpp
xpp::fiber([]() {
  auto [tx, rx] = xpp::sync::mpsc::channel<int>(4);

  std::thread producer([tx = std::move(tx)]() mutable {
    for (int i = 0; i < 10; i++) tx.send(i).await();  // blocks when full
  }).detach();

  for (int i = 0; i < 10; i++) {
    auto v = rx.recv().await();  // fiber suspends when empty
    printf("got %d\n", v.unwrap());
  }
}).await();
```

## Bounded channel — `co_await` (C++20)

```cpp
auto [tx, rx] = xpp::sync::mpsc::channel<int>(16);
co_await tx.send(42);
auto tx2 = tx;
co_await tx2.send(10);
auto v = co_await rx.recv();
```

## Unbounded channel — `.await()`

```cpp
auto [tx, rx] = xpp::sync::mpsc::channel<int>();  // no capacity argument

tx.send(42);       // void — never blocks
tx.try_send(99);   // bool — always true

auto v = rx.recv().await();   // async receive
auto v = rx.try_recv();       // sync receive — Option<T>
```

## Unbounded channel — `co_await`

```cpp
auto [tx, rx] = xpp::sync::mpsc::channel<int>();
tx.send(42);
auto v = co_await rx.recv();
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
