# nonnull.h — Non-Null Pointer Wrapper

## Introduction

`nonnull.h` provides `NonNull<T>`, a guaranteed-non-null pointer wrapper with a niche-optimized `Option<NonNull<T>>` specialization. It is the libxpp counterpart of Rust's `NonNull<T>` and `Option<&T>` niche optimization.

`sizeof(NonNull<T>) == sizeof(T*)` and `sizeof(Option<NonNull<T>>) == sizeof(T*)` — matching Rust's layout exactly. Compared to `Option<T*>` (16 bytes due to the bool tag), this saves 8 bytes per slot when non-nullness can be proven at the type level.

## Why Not Just `T*`?

A raw `T*` has no type-level non-null guarantee. Passing `T*` everywhere forces every caller to either document or check the "must not be null" contract. `NonNull<T>` moves the contract into the type system:

| Approach | Compile-time non-null? | Nullable via ? | Size |
|---|---|---|---|
| `T*` | No | Check manually | 8 bytes |
| `Option<T*>` | No | `is_some()` | 16 bytes |
| `NonNull<T>` | **Yes** | N/A | 8 bytes |
| `Option<NonNull<T>>` | **Yes** (when Some) | `is_some()` | **8 bytes** |

## API Reference

### NonNull\<T\>

| Member | Description |
|---|---|
| `NonNull(T& ref)` | Bind to an existing referent (always safe, SFINAE-excluded for `void`). |
| `static NonNull new_unchecked(T*)` | Wrap a raw pointer; debug-asserts non-null. |
| `static Option<NonNull> from(T*)` | Checked: `nullptr → None`, non-null → `Some`. |
| `T* get()` | Raw pointer access (never null). |
| `T& operator*()` | Dereference (SFINAE-removed for `T = void`). |
| `T* operator->()` | Member access (SFINAE-removed for `T = void`). |
| `operator==` / `operator!=` | Pointer equality. |

Copyable, moveable, trivially destructible.

### Option\<NonNull\<T\>\>

| Method | Returns | Notes |
|---|---|---|
| `unwrap()` | `NonNull<T>` (by value) | Panics if None |
| `unwrap_unchecked()` | `NonNull<T>` (by value) | Debug assert only |
| `unwrap_or(fallback)` | `NonNull<T>` | Fallback if None |
| `map(fn)` | `Option<U>` | fn receives `NonNull<T>` |
| `and_then(fn)` | `R` | fn returns `Option<U>` |
| `filter(pred)` | `Option<NonNull<T>>` | Consuming (rvalue) |
| `inspect(fn)` | Chainable | Side effect |

## Usage Examples

### Binding to a reference

```cpp
int x = 42;
xpp::NonNull<int> p(x);   // Always safe — references are non-null
*p = 10;                    // modifies x
```

### Checked construction from raw pointer

```cpp
int* raw = get_some_pointer_maybe_null();
auto opt = xpp::NonNull<int>::from(raw);
opt.inspect([](auto p) { *p += 1; });
```

### Type-level contract in APIs

```cpp
// Before: "widget must not be null" (documentation contract)
void draw(Widget* widget);

// After: contract enforced at compile time
void draw(xpp::NonNull<Widget> widget);
```

### Niche-optimized Option

```cpp
struct Node {
    int value;
    xpp::Option<xpp::NonNull<Node>> next;  // 8 bytes, not 16
    // nullptr = None, any other = Some
};
static_assert(sizeof(Node) == 16);  // int (4+padding) + pointer (8)
```

### Combinator chain

```cpp
auto result = xpp::NonNull<Connection>::from(raw)
    .map([](auto conn) { conn->send("ping"); return conn->recv(); })
    .unwrap_or("timeout");
```

## Implementation Notes

### Storage

```cpp
template <class T>
class NonNull {
    T* m_ptr;   // Invariant: m_ptr != nullptr
};
```

Just a raw pointer with an invariant. No runtime overhead beyond what `T*` already costs. `Option<NonNull<T>>` stores `T*` directly — `nullptr` encodes `None`.

### Reference constructor SFINAE guards

```cpp
template <class U = T,
          class = typename std::enable_if<
              !std::is_void<U>::value &&
              std::is_same<U, T>::value>::type>
explicit NonNull(U& ref) noexcept : m_ptr(&ref) {}
```

Two constraints:
1. `!std::is_void<U>` — `void&` is ill-formed, so the constructor is removed for `NonNull<void>`.
2. `std::is_same<U, T>` — prevents GCC from preferring this template over the implicit copy constructor when a `NonNull<T>` lvalue is passed by value.

### Niche optimization: Option\<NonNull\<T\>\>

```cpp
template <class T>
class Option<NonNull<T>> {
    T* m_ptr;   // nullptr = None, non-null = Some
};
```

Unlike the general `Option<T>` which uses `aligned_storage + bool` (16 bytes for pointers), this specialization stores only the pointer. `nullptr` is a niche value — `NonNull` guarantees `m_ptr != nullptr`, so `nullptr` is free to repurpose.

```cpp
static_assert(sizeof(Option<NonNull<int>>) == sizeof(int*));
```

The same pattern is used by `Option<Box<T>>` (box.h) and Rust's `Option<Box<T>>` / `Option<NonNull<T>>` in the standard library.
