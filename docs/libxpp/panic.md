# panic.h — Fatal Error Reporting

## Introduction

`panic.h` provides macros for reporting unrecoverable contract violations. Three levels of assertion:

| Macro | When checked | Use case |
|---|---|---|
| `XPP_PANIC(fmt, ...)` | Always | Unconditional termination |
| `XPP_ASSERT(cond, fmt, ...)` | Always (release too) | Public API contract checks |
| `XPP_DEBUG_ASSERT(cond, fmt, ...)` | Debug only (`XPP_DEBUG=1`) | Internal invariants on hot paths |

Panics are for **bugs**, not runtime conditions. For recoverable errors, use `Result<T, E>`.

## API Reference

### XPP_PANIC

```cpp
XPP_PANIC("invariant X violated");
XPP_PANIC("idx %zu out of range (size=%zu)", idx, size);
```

Outputs `panic at <file>:<line>: <message>` and terminates the process. The format suffix `—` (U+2014 em dash) separates the standard prefix from the user message.

### XPP_ASSERT

```cpp
XPP_ASSERT(ptr != nullptr, "null pointer passed to %s", func_name);
XPP_ASSERT(idx < size, "idx=%zu size=%zu", idx, size);
```

Output: `panic at foo.cpp:42: assertion failed: idx < size — idx=7 size=4`

The condition is always evaluated, even in release builds. The stringified condition is automatically included in the panic message.

### XPP_DEBUG_ASSERT

```cpp
XPP_DEBUG_ASSERT(m_has_value, "internal: Option storage uninitialized");
XPP_DEBUG_ASSERT(idx < size, "idx=%zu size=%zu", idx, size);
```

Compiled to `((void)0)` when `XPP_DEBUG=0`. Used for `*Unchecked()` APIs (e.g. `unwrap_unchecked`) and internal invariants.

### XPP_DEBUG

Controlled by `NDEBUG`:

- `NDEBUG` not defined → `XPP_DEBUG = 1` (Debug build)
- `NDEBUG` defined → `XPP_DEBUG = 0` (Release build)

Override with `-DXPP_DEBUG=1` or `-DXPP_DEBUG=0` to decouple from build type.

## Usage Pattern

```cpp
// Public API: always check — caller might misuse
T& Option<T>::unwrap() & {
    XPP_ASSERT(m_has_value, "unwrap() on None Option");
    return *reinterpret_cast<T*>(&m_storage);
}

// Internal hot path: debug check only — caller has verified
T& Option<T>::unwrap_unchecked() & noexcept {
    XPP_DEBUG_ASSERT(m_has_value, "internal: Option must be Some");
    return *reinterpret_cast<T*>(&m_storage);
}
```

## Implementation Notes

`do_panic` is defined in `panic.cpp` (not header-only) to avoid pulling in the libx logging dependency transitively. A type-safe `printf`-style `__attribute__((format(printf, 1, 2)))` on GCC/Clang lets the compiler validate format strings at every macro use site.

`XPP_ASSERT` uses `XPP_UNLIKELY` to hint the branch predictor that the assertion rarely fires, keeping the hot path in the instruction cache.
