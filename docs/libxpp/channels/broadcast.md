# broadcast

Multi-producer, multi-consumer channel. Every consumer sees every value.

```text
Sender ──┬──▶ [v0][v1][v2] ──▶ Receiver₁ (pos=2)
Sender ──┘                        Receiver₂ (pos=1)
```

Values are retained until all receivers have read them, or evicted when the
bounded buffer wraps around. Receivers that fall behind get `RecvError::Lagged`.

## Example — `.await()`

```cpp
#include <xpp/sync/broadcast.h>

xpp::EventLoop loop;
xpp::WaitScope scope(loop);

auto [tx, rx1] = xpp::sync::broadcast::channel<std::string>(16);
auto rx2 = tx.subscribe();

tx.send("hello");
tx.send("world");

// Both receivers see both values
auto v1 = rx1.recv().await();  // Ok("hello")
auto v2 = rx2.recv().await();  // Ok("hello")
auto v3 = rx1.recv().await();  // Ok("world")
auto v4 = rx2.recv().await();  // Ok("world")

// Late subscriber only sees future values
auto rx3 = tx.subscribe();
tx.send("!");
auto v5 = rx3.recv().await();  // Ok("!") — didn't see hello/world
```

## Example — `co_await` (C++20)

```cpp
auto [tx, rx1] = xpp::sync::broadcast::channel<std::string>(16);
auto rx2 = tx.subscribe();
tx.send("hello");
tx.send("world");
auto v1 = co_await rx1.recv();  // Ok("hello")
auto v2 = co_await rx2.recv();  // Ok("hello")
```

## Handling lag

```cpp
auto [tx, rx] = xpp::sync::broadcast::channel<int>(2);

tx.send(1);
tx.send(2);
tx.send(3);  // buffer full → 1 is evicted

auto r = rx.recv().await();
if (r.is_err()) {
  // RecvError::Lagged — values were lost
  // Position auto-resets to current head, can continue
}
auto v = rx.recv().await();  // Ok(2)
auto v = rx.recv().await();  // Ok(3)
```

## API

### `Sender<T>`

| Method | Returns | Description |
| -------- | --------- | ------------- |
| `send(T)` | `Promise<Result<size_t, SendError<T>>>` | Send value. Returns receiver count. |
| `try_send(T)` | `Result<size_t, SendError<T>>` | Synchronous send. |
| `subscribe()` | `Receiver<T>` | Create new receiver (future values only). |
| `receiver_count()` | `size_t` | Number of active receivers. |
| `len()` | `size_t` | Number of buffered values. |

### `Receiver<T>`

| Method | Returns | Description |
| -------- | --------- | ------------- |
| `recv()` | `Promise<Result<T, RecvError>>` | Next value, or `Lagged`/`Closed`. |
| `try_recv()` | `Result<T, TryRecvError>` | Synchronous receive. |

### Error types

| Type | Variant | Description |
| ------ | --------- | ------------- |
| `RecvError` | `Lagged` | Values were evicted before reading. |
| | `Closed` | All senders dropped, buffer empty. |
| `TryRecvError` | `Empty` | No value available. |
| | `Closed` | Channel empty and closed. |
| `SendError<T>` | `NoReceiver(v)` | No receivers subscribed; value returned. |

## Thread safety

- Lock-free send path (mutex on `m_head`/`m_tail` update only).
- Receiver is single-consumer.
- Multiple senders (cloned Sender) supported.
