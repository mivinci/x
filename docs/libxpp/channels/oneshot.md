# oneshot

Single-value, single-use channel. Think "async return value".

```text
Sender ──(value)──▶ Receiver
```

Once `send()` is called, the `Receiver`'s promise resolves. Calling `send()`
more than once is safe — only the first call takes effect (atomic CAS).

## Example

```cpp
#include <xpp/promise.h>
#include <xpp/sync/oneshot.h>

// Fire-and-forget: worker thread computes a result, main thread awaits it
auto [tx, rx] = xpp::sync::oneshot::channel<int>();

std::thread worker([tx = std::move(tx)]() mutable {
  tx.send(42);
});

int result = co_await std::move(rx).recv();  // resolves when worker sends
```

## API

| Type | Method | Description |
| -------- | --------- | ------------- |
| `Sender<T>` | `send(T value)` | Send the value. Idempotent. |
| `Receiver<T>` | `recv() &&` | Returns `Promise<T>`. Resolves when `send()` is called. |

## Thread safety

`Sender::send()` is inherently thread-safe — `PromiseResolver::resolve()` uses
`Arc<ArcWeak>` + atomic CAS internally. The `Sender` can be moved to another
thread and `send()` called from there without additional synchronization.
