# Custom Adapters

[← Promise](README.md)

## When to Use

You have an async operation (HTTP fetch, DNS lookup, thread pool work) and want to bridge it into the Promise system with automatic cancellation when the Promise is destroyed.

## Adapter Contract

An Adapter is a class with:

- **Constructor**: receives `PromiseResolver<T>&&` + user args. Starts the async operation.
- **Destructor**: cancels the async operation (if still in-flight).
- **Async callback**: calls `resolver.resolve(value)` when done. Safe to call from any thread.

```cpp
class MyAdapter {
  PromiseResolver<int> m_resolver;
public:
  MyAdapter(PromiseResolver<int>&& r, const char* url)
    : m_resolver(std::move(r)) {
    // Start async fetch...
    // When done: m_resolver.resolve(response_code);
  }
  ~MyAdapter() {
    // Cancel the fetch if still in-flight
  }
};
```

## `adapt<T, Adapter>(args...)`

```cpp
#include <xpp/promise_adapter.h>

auto p = xpp::adapt<int, MyAdapter>(url);
// MyAdapter is constructed with (PromiseResolver<int>&&, url)
// When the fetch completes, MyAdapter calls resolver.resolve(code)
int code = p.wait();
```

## How It Works

```text
AdapterPromiseNode<T, Adapter>
  ├─ Arc<ResolveState<T>> m_state    ← strong ref (keeps state alive)
  ├─ Adapter m_adapter               ← owns the async operation
  │    └─ PromiseResolver<T>         ← weak ref (ArcWeak to state)
  │
  ├─ poll(waker):                    ← generic, same for all adapters
  │    if resolved → return value
  │    else register waker → re-check → return None
  │
  └─ ~dtor: ~Adapter() → ~Arc()     ← cancel op, then drop strong ref
```

### ResolveState — Shared via Arc/ArcWeak

```cpp
struct ResolveState<T> {
  Option<T>          value;
  AtomicPromiseWaker waker;
  std::atomic<bool>  resolved{false};
};
```

- **AdapterPromiseNode** holds `Arc<ResolveState<T>>` (strong) — keeps state alive.
- **PromiseResolver** holds `ArcWeak<ResolveState<T>>` (weak) — `resolve()` calls `upgrade()`.
- When node is destroyed → strong count → 0 → `upgrade()` returns `None` → `resolve()` silently drops.

### Lifecycle Safety

```text
Promise destroyed (e.g., race loser):
  1. ~AdapterPromiseNode()
  2. ~Adapter() → cancel async operation (e.g., xTimerStop)
  3. ~Arc<State>() → strong count = 0
  4. Async callback fires later → resolver.resolve(v)
  5. ArcWeak::upgrade() → None (strong = 0) → silently drop
  6. No UAF.
```

## TimerAdapter — Built-in Adapter

`TimerAdapter` replaces the old `TimerPromiseNode`. It's a thin adapter (~25 lines) that owns an `xTimer` handle:

```cpp
class TimerAdapter {
  xTimer m_handle;
  std::atomic<bool> m_fired{false};
  PromiseResolver<void> m_resolver;
public:
  TimerAdapter(PromiseResolver<void>&& r, uint64_t ms)
    : m_resolver(std::move(r)) {
    m_handle = xTimerStart(
      [](void* a) {
        auto* self = static_cast<TimerAdapter*>(a);
        self->m_fired.store(true, release);
        self->m_resolver.resolve();
      },
      this,
      [](void* a) {  // on_cancel (loop destroy)
        auto* self = static_cast<TimerAdapter*>(a);
        self->m_handle = nullptr;
        self->m_fired.store(true, release);
      },
      ms, 0);
  }
  ~TimerAdapter() {
    if (!m_fired.load(acquire) && m_handle)
      xTimerStop(m_handle);
  }
};
```

Used internally by `after(ms)`:

```cpp
Promise<void> after(uint64_t ms) {
  return adapt<void, TimerAdapter>(ms);
}
```

## `async<T>()` — Manual Resolve

`async` uses `ManualResolveNode<T>` (same `poll_state` logic, no Adapter):

```cpp
auto [p, r] = xpp::async<int>();
// p is backed by ManualResolveNode<int>
// r is PromiseResolver<int> (ArcWeak to shared state)
r.resolve(42);
p.wait();  // 42
```

## Writing a Custom Cross-Thread Adapter

The built-in `WorkAdapter` covers the common case. For custom adapters that
need more control (e.g., specific task group, progress reporting):

```cpp
class MyFetchAdapter {
  struct Ctx { PromiseResolver<Response> resolver; std::string url; };
  Ctx* m_ctx;
  xWork m_work;
public:
  MyFetchAdapter(PromiseResolver<Response>&& r, const std::string& url)
      : m_ctx(new Ctx{std::move(r), url}) {
    m_work = xWorkSubmit(
        nullptr,
        [](void* a) -> void* {
          auto* ctx = static_cast<Ctx*>(a);
          Response resp = do_fetch(ctx->url);
          ctx->resolver.resolve(std::move(resp));  // cross-thread, safe
          return nullptr;
        },
        [](void* a, void*) { delete static_cast<Ctx*>(a); },
        [](void* a, void*) { delete static_cast<Ctx*>(a); },
        m_ctx);
  }
  ~MyFetchAdapter() { if (m_work) xWorkCancel(m_work); }
};

auto p = xpp::adapt<Response, MyFetchAdapter>(url);
```
