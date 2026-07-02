# opaque.h — Opaque Handle Wrapper

## Introduction

`opaque.h` provides a single type alias:

```cpp
template <class D>
using OwnedOpaquePointer = Own<void, D>;
```

libx's opaque handles are all `typedef void* xFoo` (via `XDEF_HANDLE`). Wrapping them with `Own<void, CustomDeleter>` is correct but exposes `void` at every use site. `OwnedOpaquePointer` hides `void` behind a name that communicates intent: "I own an opaque pointer."

## Usage

```cpp
struct EventLoopDestroy {
    void operator()(void* h) const noexcept {
        xEventLoopDestroy(static_cast<xEventLoop>(h));
    }
};

class EventLoop {
    OwnedOpaquePointer<EventLoopDestroy> m_loop;
    // Equivalent to: Own<void, EventLoopDestroy> m_loop;
};
```

The deleter must have `void operator()(void*) const noexcept`. It typically casts to the correct handle type and calls the corresponding `xXxxDestroy` function. EBO applies — if the deleter is stateless, `sizeof(OwnedOpaquePointer<D>) == sizeof(void*)`.
