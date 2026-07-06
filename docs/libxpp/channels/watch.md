# watch

Single-value, version-tracked channel. Only the latest value is retained.
Each receiver tracks which version it has "seen".

```text
Sender ──(value)──▶ [latest] ──▶ Receiver₁ (seen=v2)
                    version++    Receiver₂ (seen=v1)
```

Use for configuration hot-reload, state observation, or any pattern where
consumers want the *latest* value, not every intermediate value.

## Example

```cpp
#include <xpp/sync/watch.h>
auto [tx, rx] = xpp::sync::watch::channel<std::string>("localhost:8080");

// On config change:
tx.send("0.0.0.0:9090");

// Wait for changes
while (true) {
    auto r = co_await rx.changed();
    if (r.is_err()) break;  // sender dropped

    auto ref = rx.borrow_and_update();  // read + mark seen
    std::cout << *ref << "\n";
}
```

## "Seen" semantics

```text
Initial: version=0, rx.seen=0          (initial value already "seen")

tx.send("hello")   → version=1
rx.changed()       → version≠seen → mark seen=1, return Ok
rx.changed()       → version==seen → wait

tx.send("world")   → version=2
rx.changed()       → resume, seen=2, return Ok
```

`borrow()` reads without marking seen — `changed()` will still trigger.
`borrow_and_update()` reads AND marks seen — `changed()` will wait for next.

## API

### `Sender<T>`

| Method | Returns | Description |
| -------- | --------- | ------------- |
| `send(T)` | `Result<T, SendError<T>>` | Replace value. Returns old value. |
| `borrow()` | `Ref<T>` | Read-lock current value. |
| `subscribe()` | `Receiver<T>` | New receiver (current value = seen). |
| `receiver_count()` | `size_t` | Number of active receivers. |
| `is_closed()` | `bool` | Whether channel is closed. |
| `closed()` | `Promise<void>` | Resolves when all receivers dropped. |

### `Receiver<T>`

| Method | Returns | Description |
| -------- | --------- | ------------- |
| `changed()` | `Promise<Result<void, RecvError>>` | Wait for unseen value, marks seen. |
| `has_changed()` | `Result<bool, RecvError>` | Sync check, does NOT mark seen. |
| `borrow_and_update()` | `Ref<T>` | Read + mark seen. |
| `borrow()` | `Ref<T>` | Read, does NOT mark seen. |

### `Ref<T>`

A read guard that holds an exclusive lock on the shared value. Prevents
concurrent writes while the value is being read. In single-threaded builds,
the lock is a no-op.

| Method | Returns | Description |
| -------- | --------- | ------------- |
| `operator*()` | `const T&` | Access the value. |
| `operator->()` | `const T*` | Access members. |

## Thread safety

- `loom::Mutex<T>` protects the shared value (exclusive write + exclusive read).
- `Ref<T>` automatically releases the lock on destruction.
- Cross-thread wakeup via Notify's `notify_waiters()`.
