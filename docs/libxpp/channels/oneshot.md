# oneshot

Single-value, single-use channel. Think "async return value".

```text
Sender ──(value)──▶ Receiver
```

Once `send()` is called, the `Receiver`'s promise resolves. Calling `send()`
more than once is safe — only the first call takes effect (atomic CAS).

## Example — `.await()`

```cpp
#include <xpp/promise.h>
#include <xpp/sync/oneshot.h>

xpp::EventLoop loop;
xpp::WaitScope scope(loop);

auto [tx, rx] = xpp::sync::oneshot::channel<int>();

std::thread worker([tx = std::move(tx)]() mutable {
  tx.send(42);
});

int result = std::move(rx).recv().await();  // resolves when worker sends
// result == 42
```

With `xpp::fiber()` — non-blocking:

```cpp
xpp::fiber([]() {
  auto [tx, rx] = xpp::sync::oneshot::channel<int>();
  std::thread([tx = std::move(tx)]() { tx.send(42); }).detach();
  int result = std::move(rx).recv().await();  // fiber suspends
  return result;
}).then([](int v) { printf("got %d\n", v); });
```

## Example — `co_await` (C++20)

```cpp
xpp::Promise<int> await_result() {
  auto [tx, rx] = xpp::sync::oneshot::channel<int>();
  std::thread([tx = std::move(tx)]() { tx.send(42); }).detach();
  co_return co_await std::move(rx).recv();
}
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
