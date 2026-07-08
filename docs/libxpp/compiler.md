# compiler.h — Portable Compiler Attributes

## Introduction

`compiler.h` wraps non-portable compiler extensions (`__builtin_*`, `__attribute__`, `__declspec`) behind `XPP_`-prefixed macros that degrade gracefully on unsupported toolchains. Self-contained — no project dependencies.

## Macros

### Branch Prediction

| Macro | Description | Fallback |
|---|---|---|
| `XPP_LIKELY(x)` | Branch expected true: `__builtin_expect(!!(x), 1)` | `(x)` |
| `XPP_UNLIKELY(x)` | Branch expected false: `__builtin_expect(!!(x), 0)` | `(x)` |

### Function Attributes

| Macro | Description | Fallback |
|---|---|---|
| `XPP_NORETURN` | `[[noreturn]]` | Compiler-specific or empty |
| `XPP_FORCE_INLINE` | `inline __attribute__((always_inline))` | `inline` |
| `XPP_NOINLINE` | `__attribute__((noinline))` | Empty |

### Control Flow

| Macro | Description | Fallback |
|---|---|---|
| `XPP_UNREACHABLE()` | `__builtin_unreachable()` | `std::abort()` |
| `XPP_FALLTHROUGH` | `[[fallthrough]]` (C++17) or `__attribute__((fallthrough))` | `((void)0)` |

### Deprecation

| Macro | Description |
|---|---|
| `XPP_DEPRECATED(msg)` | `[[deprecated(msg)]]` (C++14) or `__attribute__((deprecated(msg)))` |

### Feature Detection

| Macro | Description |
|---|---|
| `XPP_DEBUG` | `1` in debug (`NDEBUG` off), `0` in release. Override with `-DXPP_DEBUG=`. |
| `XPP_HAS_COROUTINES` | `1` if C++20 coroutines are available. Checks `__cplusplus >= 202002L` AND `__cpp_coroutines >= 201902L`. Override with `-DXPP_HAS_COROUTINES=0/1`. |
| `XPP_FIBER` | `1` when fiber support is enabled (`libxpp` CMake option). Enables `xpp::fiber()` and fiber-aware `.await()`. |
| `XPP_MT` | `1` when multi-threading support is enabled (`libxpp` CMake option). Switches `Shared<T>` from `Rc` to `Arc`.

### Usage

```cpp
// Cold path: assertion failure
if (XPP_UNLIKELY(!ptr)) {
    log_error_and_abort();
}

// Intentional switch fallthrough
case STATE_A:
    init_a();
    XPP_FALLTHROUGH;
case STATE_B:
    process();

// Feature-gated coroutine support
#if XPP_HAS_COROUTINES
    // C++20 coroutine code
#endif
```

## Implementation Notes

`XPP_DEBUG` defaults to `!defined(NDEBUG)` but is overridable at the command line. This allows enabling debug assertions in release builds (`-DXPP_DEBUG=1`) or disabling them in debug builds (`-DXPP_DEBUG=0`) for benchmarking.

`XPP_HAS_COROUTINES` checks both `__cpp_coroutines` and `__cpp_impl_coroutine` — Apple Clang historically defined only the latter.
