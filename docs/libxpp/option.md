# option.h — Nullable Values with Combinators

## Introduction

`option.h` provides `Option<T>`, a type-safe alternative to nullable pointers and `std::optional`. It closely mirrors **Rust's `Option<T>`** — both the owning `Option<T>` (heap-allocated or value-semantic) and the non-owning `Option<T&>` (zero-overhead nullable reference) — with the full set of monadic combinators.

Two key design rules separate it from `std::optional`:

- **No implicit conversion to bool.** Use `is_some()` / `is_none()` or `if (o)` for explicit checking.
- **Abort on unwrap of None.** `unwrap()` always checks, even in release builds. `unwrap_unchecked()` skips the check but debug-asserts. There is no "undefined value" path — either you handle None, or you crash cleanly.

## Design Philosophy

1. **Storage is placement-new, not union.** The value lives in `std::aligned_storage<sizeof(T), alignof(T)>` with explicit construction/destruction. This avoids the limitations of C++11's restricted union support and works uniformly for non-default-constructible, non-copyable, and non-movable types (with appropriate usage).

2. **Three unwrap strategies for three trust levels.**
   - `unwrap()` — always checks. Use when the invariant is not locally obvious.
   - `expect(msg)` — always checks with a custom panic message. Use when the failure reason is domain-specific.
   - `unwrap_unchecked()` — debug-asserts only, zero-cost in release. Use when the caller has proven `is_some()` structurally (e.g. after `if (o)`).

3. **Combinators are consuming where Rust is consuming.** `filter()`, `ok_or()`, `ok_or_else()`, and `unwrap_or_else()` are rvalue-qualified (`&&`), matching Rust's `self` semantics. This prevents accidental use-after-move and makes ownership transfer explicit in the type system.

4. **Option\<T&\> is a first-class specialization.** `sizeof(Option<T&>) == sizeof(T*)`. It's rebindable (unlike real C++ references), supports most combinators, and replaces raw `T*` with "might be null" semantics everywhere.

5. **Bridge to Result.** `ok_or(err)` and `ok_or_else(fn)` convert `Option<T>` → `Result<T, E>`, enabling smooth transitions between "value might be missing" and "value or error" code paths.

## Architecture

```mermaid
classDiagram
    class Option~T~ {
        -bool m_has_value
        -aligned_storage m_storage
        +is_some() bool
        +is_none() bool
        +unwrap() T&
        +unwrap_unchecked() T&
        +expect(msg) T&
        +unwrap_or(fallback) T
        +take() Option
        +map(fn) Option~U~
        +and_then(fn) Option~U~
        +or_else(fn) Option
        +unwrap_or_else(fn) T
        +filter(pred) Option
        +inspect(fn) Option&
        +ok_or(err) Result
        +ok_or_else(fn) Result
    }
    class Option~T&~ {
        -T* m_ptr
        +is_some() bool
        +unwrap() T&
        +take() Option
        +map(fn) Option~U~
        +and_then(fn) Option~U~
        +filter(pred) Option
        +inspect(fn) Option&
    }
    class None {
        <<tag>>
    }
    class Some {
        <<factory function>>
    }
    Option~T&~ --|> "specializes"
    Option~T~ ..> None : constructed from
    Option~T~ ..> Some : constructed via
```

**The two variants at a glance:**

| | `Option<T>` | `Option<T&>` |
|---|---|---|
| Storage | Inline `aligned_storage` | Raw pointer `T*` |
| Size | `sizeof(T) + padding` | `sizeof(T*)` |
| Owns value | Yes | No |
| Rebindable | Via `operator=` | Via `operator=` |
| Combinators | All | All except `ok_or`, `unwrap_or_else` |

## API Reference

### Construction

| Expression | Result |
|---|---|
| `Option<T> o;` | Empty (None) |
| `Option<T> o(none);` | Empty (None) |
| `Option<T> o(v);` | Holds copy of `v` |
| `Option<T> o(std::move(v));` | Holds moved `v` |
| `auto o = Some(v);` | Deduces `Option<decay_t<T>>` |
| `o = none;` | Clears held value |

### Observers

| Method | Returns | On None |
|---|---|---|
| `is_some()` | `bool` | — |
| `is_none()` | `bool` | — |
| `operator bool()` | `bool` (explicit) | — |

### Unwrap

| Method | Returns | On None |
|---|---|---|
| `unwrap()` | `T&` / `const T&` / `T&&` | `XPP_ASSERT` abort |
| `unwrap_unchecked()` | `T&` / `const T&` / `T&&` | Debug assert; UB in release |
| `expect(msg)` | `T&` / `const T&` / `T&&` | `XPP_ASSERT` with `msg` |
| `unwrap_or(v)` | `T` (by value) | Returns `v` |

### Combinators

| Method | Signature | Semantics |
|---|---|---|
| `take()` | `() -> Option<T>` | Extract value, leave None |
| `map(fn)` | `(T -> U) -> Option<U>` | Transform if Some |
| `and_then(fn)` | `(T -> Option<U>) -> Option<U>` | Monadic bind |
| `or_else(fn)` | `(() -> Option<T>) -> Option<T>` | Fallback if None |
| `unwrap_or_else(fn)` | `(() -> T) -> T` (rvalue only) | Lazy fallback |
| `filter(pred)` | `(T -> bool) -> Option<T>` (rvalue only) | Keep if predicate true |
| `inspect(fn)` | `(T -> void) -> Option<T>&` | Side-effect, chainable |
| `ok_or(e)` | `(E) -> Result<T, E>` (rvalue only) | Convert to Result |
| `ok_or_else(fn)` | `(() -> E) -> Result<T, E>` (rvalue only) | Lazy error |

### Option\<T&\> Specifics

- **No `unwrap_or_else`** — returning a reference to a stack local would dangle.
- **No `ok_or` / `ok_or_else`** — Result does not have a reference specialization.
- **`filter` is const-qualified** (not rvalue-only) — references are trivially copyable.
- **`take()` returns `Option<T&>`** pointing to the same object; original is cleared.

## Usage Examples

### Basic value presence

```cpp
xpp::Option<int> maybe = get_value();

if (maybe) {
    process(maybe.unwrap());
} else {
    use_default();
}

// Or with combinators:
auto result = maybe.map([](int x) { return x * 2; })
                   .unwrap_or(0);
```

### Short-circuit with and_then

```cpp
xpp::Option<User>  user  = find_user(id);
xpp::Option<Order> order = user.and_then([](User &u) {
    return u.last_order();
});

// Only runs if user was Some AND last_order() was Some.
```

### Fallback chain with or_else

```cpp
auto config = read_file("local.conf")
              .or_else([] { return read_file("global.conf"); })
              .or_else([] { return read_file("/etc/defaults.conf"); });
```

### Lazy default with unwrap_or_else

```cpp
// compute_default() is only called if `maybe` is None.
int value = std::move(maybe).unwrap_or_else([] { return compute_default(); });
```

### Side-effect inspection in a chain

```cpp
auto result = parse(input)
              .inspect([](auto &v) { log("parsed: {}", v); })  // log if Some
              .map([](auto v) { return transform(v); });
```

### Filter with predicate

```cpp
auto positive = std::move(maybe).filter([](int x) { return x > 0; });
// positive is None if maybe was None OR value <= 0.
```

### Nullable references (no heap, no copy)

```cpp
// Find returns Option<T&> — zero allocation, pointer-sized.
auto found = registry.find(key);
if (found) {
    found.unwrap().update();  // mutates the registry entry directly
}

// Rebindable:
int a = 1, b = 2;
Option<int&> ref(a);
ref = Option<int&>(b);  // now points to b
ref.unwrap() = 99;      // b becomes 99, a unchanged
```

### Bridge to Result

```cpp
auto r = std::move(maybe_user).ok_or<std::string>("user not found");
// Result<User, string>: Ok(user) or Err("user not found")

// Lazy error:
auto r2 = std::move(maybe).ok_or_else([&] {
    return format_error("missing key: {}", key);
});
```

## Comparison

| | `xpp::Option<T>` | `std::optional<T>` | Rust `Option<T>` |
|---|---|---|---|
| Empty check | `is_some()` / `is_none()` | `has_value()` | `is_some()` / `is_none()` |
| Unwrap | `unwrap()` — always checks | `value()` — throws `bad_optional_access` | `unwrap()` — panics |
| Unchecked | `unwrap_unchecked()` | `operator*` — UB | `unwrap_unchecked()` — UB |
| Map | `map(fn)` | `transform(fn)` (C++23) | `map(fn)` |
| AndThen | `and_then(fn)` | `and_then(fn)` (C++23) | `and_then(fn)` |
| OrElse | `or_else(fn)` | `or_else(fn)` (C++23) | `or_else(fn)` |
| Filter | `filter(pred)` | ✗ | `filter(pred)` |
| Inspect | `inspect(fn)` | ✗ | `inspect(fn)` |
| OkOr | `ok_or(e)` | ✗ | `ok_or(e)` |
| Nullable ref | `Option<T&>` | ✗ | Built into borrow checker |
| Size (ref) | `sizeof(T*)` | N/A | N/A |

## Implementation Notes

### Storage

```cpp
bool m_has_value = false;
typename std::aligned_storage<sizeof(T), alignof(T)>::type m_storage;
```

The value is constructed in-place via `new (&m_storage) T(...)` and destroyed via `reinterpret_cast<T*>(&m_storage)->~T()`. `clear()` is the sole destruction site, used by destructor, assignment, and `operator=(none)`.

`aligned_storage` was chosen over anonymous `union` to support non-default-constructible types (`T()` = delete) in C++11 without compiler-specific extensions.

### Option\<T&\>

Trivially a `T*` pointer. `is_none()` is `m_ptr == nullptr`. No heap, no lifetime management — the caller must ensure the referent outlives the Option. Rebindable via `operator=`, unlike native C++ references.

### Combinators on rvalue vs lvalue

Consuming combinators (`filter`, `ok_or`, `ok_or_else`, `unwrap_or_else`) are `&&`-qualified. This enforces Rust-like ownership semantics: you move the Option, the combinator consumes it, and the original is left in a valid-but-unspecified state (typically empty). Non-consuming combinators (`map`, `and_then`, `or_else`, `inspect`) have `const&` and `&&` overloads.

### Bridge to Result

`ok_or` and `ok_or_else` are declared but **not defined** in `option.h`. Their definitions live in `result.h` via an explicit include order dependency: the user must `#include <xpp/result.h>` after `#include <xpp/option.h>`. This avoids a circular header dependency while still allowing `Option` to name `Result` via a forward declaration.
