# variant.h — Tagged Union

## Introduction

`variant.h` provides `Variant<Types...>`, a type-safe tagged union holding exactly one of the specified types. It is the C++11 replacement for `std::variant` (C++17), serving as the storage foundation for `Result<T, E>`.

Always holds a value — no empty/default state. The active alternative is tracked by a runtime `size_t` index. Accessing the wrong alternative panics.

## API Reference

### Construction

| Expression | Description |
|---|---|
| `Variant<T...>(val)` | Construct from a value of one of the types. |
| `Variant<T...>(InPlaceIndex<N>, args...)` | In-place construct the N-th alternative. |

### Observers

| Method | Description |
|---|---|
| `is<T>()` | True if holding type T. |
| `is<N>()` | True if holding the N-th alternative. |
| `index()` | Zero-based runtime index of the active type. |

### Access (checked — panics on mismatch)

| Method | Returns |
|---|---|
| `get<T>()` | `T&` / `const T&` / `T&&` |
| `get<N>()` | Reference to the N-th type |

### Access (unchecked — debug assert only)

| Method | Returns |
|---|---|
| `get_unchecked<T>()` | `T&` |
| `get_unchecked<N>()` | Reference to the N-th type |

## Usage Examples

### Basic usage

```cpp
xpp::Variant<int, float, std::string> v(42);
assert(v.is<int>());
assert(v.index() == 0);

int x = v.get<int>();  // 42
// v.get<float>();     // panics: holding int, not float
```

### Disambiguating duplicate types

```cpp
xpp::Variant<int, int> a(xpp::InPlaceIndex<0>{}, 42);  // first int
xpp::Variant<int, int> b(xpp::InPlaceIndex<1>{}, 99);  // second int

assert(a.get<0>() == 42);
assert(b.get<1>() == 99);
```

### In-place construction

```cpp
xpp::Variant<int, std::string> v(
    xpp::InPlaceIndex<1>{}, "hello world");
assert(v.get<std::string>() == "hello world");
```

### Unchecked access on hot paths

```cpp
if (v.is<int>()) {
    // Caller has verified — skip the redundant check
    process(v.template get_unchecked<int>());
}
```

## Comparison

| | `xpp::Variant<T...>` | `std::variant<T...>` (C++17) | Rust `enum` |
|---|---|---|---|
| Standard | C++11 | C++17 | — |
| Empty state | None | `valueless_by_exception` possible | None |
| Access | `get<T>()` / `get<N>()` | `std::get<T>()` / `std::get<N>()` | Pattern matching |
| Error on wrong type | Panic | `std::bad_variant_access` | Compile-time |
| Visit | Not exposed (internal only) | `std::visit` | `match` |
| Duplicate types | `InPlaceIndex<N>` disambiguation | `std::in_place_index<N>` | Named variants |

## Implementation Notes

### Storage

```cpp
template <class... Types>
class Variant {
    using Storage = typename std::aligned_union<0, Types...>::type;
    Storage m_storage;
    size_t  m_index;
};
```

`std::aligned_union` provides a byte buffer sized and aligned for the largest type in `Types...`. The `m_index` field tracks which alternative is alive.

### Type-to-index mapping

```cpp
template <size_t I, class T, class First, class... Rest>
struct TypeIndex<I, T, First, Rest...> {
    static constexpr size_t k_value =
        std::is_same<T, First>::value ? I : TypeIndex<I + 1, T, Rest...>::k_value;
};
```

Compile-time recursive template: finds the position of `T` in `Types...`. When `T` appears multiple times, `get<T>()` returns the first match.

### Visit by index (internal)

Copy, move, and destroy use a compile-time visitor dispatch (`VisitByIndex`) that maps the runtime `m_index` to a typed operation:

```cpp
template <class Tuple, size_t N>
struct VisitByIndex {
    template <class Fn, class Storage>
    static void run(size_t i, Storage &storage, Fn &&fn) {
        if (i == N - 1) {
            using T = typename std::tuple_element<N - 1, Tuple>::type;
            fn(reinterpret_cast<T*>(&storage));
            return;
        }
        VisitByIndex<Tuple, N - 1>::run(i, storage, fn);
    }
};
```

This is a linear scan (O(N)) that beats `std::visit` for small N (2–4 types, which covers all current use cases: `Result<T, E>` and `Result<void, E>`). For larger variants, a jump table would be faster, but libxpp doesn't need one.

### Exception safety

Copy assignment uses copy-and-swap:

```cpp
Variant& operator=(const Variant &o) {
    if (this != &o) {
        Variant tmp(o);     // Copy first (may throw)
        destroy();          // Only then destroy old value
        m_index = tmp.m_index;
        move_from(std::move(tmp));  // Move tmp's value in
    }
    return *this;
}
```

If `copy_from` throws, `*this` is left unchanged. If it succeeds, the destroy-move sequence is noexcept for moveable types, providing the strong exception-safety guarantee.
