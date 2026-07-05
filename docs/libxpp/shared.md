# Shared

## Introduction

`xpp::Shared<T>` is a conditional type alias for shared-ownership smart pointers:

```cpp
#if defined(XPP_MT)
#include <xpp/arc.h>
template <class T> using Shared = Arc<T>;  // atomic refcount (multi-threaded)
#else
#include <xpp/rc.h>
template <class T> using Shared = Rc<T>;   // non-atomic (single-threaded)
#endif
```

Default is `Rc<T>` (single-threaded, no atomic overhead). Pass `-DXPP_MT` to switch to `Arc<T>` for multi-threaded schedulers. API is identical — migrate by recompiling.

Used wherever ownership needs to be shared between multiple objects: `TcpStream`'s internal state (shared between `ReadHalf`/`WriteHalf` after `io::split`), `TcpListener`'s `Impl` (shared with libx callback via stable heap address).

## API Reference

| Type | Description |
| ---- | ----------- |
| `Shared<T>` | `Rc<T>` (default) or `Arc<T>` (with `XPP_MT`) |
| `Shared<T>::make(args...)` | Heap-allocate and construct `T` |
| `shared->` / `shared.get()` | Access the inner `T` |
| `shared.clone()` | Explicit +1 on refcount |

## Usage

```cpp
#include <xpp/shared.h>

// Default: single-threaded Rc
auto shared = xpp::Shared<MyType>::make(args...);

// Same API, recompile with -DXPP_MT for atomic Arc
shared->do_something();
```
