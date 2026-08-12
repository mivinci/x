# serde — Serialization Framework

## Introduction

`xpp::serde` is a trait-based serialization framework modeled on Rust's
[`serde`](https://serde.rs). A data type specializes `Serialize<T>` /
`Deserialize<T>` once, and any backend that satisfies the `Serializer` /
`Deserializer` concept can drive it. Backends are duck-typed (no vtable)
and template-monomorphized per call site.

Two backends ship in-tree:

- `xpp::serde::json` — wraps `libx/x/json/` (DOM-based, human-readable)
- `xpp::serde::bin` — compact length-prefixed binary format

The same `Serialize<T>` / `Deserialize<T>` specialization works unchanged
across both backends. Switching format is a one-line change at the call
site; user types never move.

## Trait Model

```cpp
namespace xpp::serde {

template <class T> struct Serialize;     // user specializes
template <class T> struct Deserialize;   // user specializes

template <class T, class S>
Result<Void, Error> serialize(const T&, S&);        // dispatcher

template <class T, class D>
Result<T, Error>    deserialize(D&);                 // dispatcher
}
```

- `Serialize<T>::run(const T&, S&)` writes `T` through `S&`.
- `Deserialize<T>::run(D&)` reads a `T` from `D&`.
- Both return `Result<T, Error>` — **no exceptions, no RTTI**.

Built-in specializations: `bool`, `int32_t`, `int64_t`, `uint32_t`,
`uint64_t`, `float`, `double`, `xpp::String`, `Option<T>`, `Vec<T>`.

## The `XPP_SERDE` Macro

The 80% case — a plain struct with named fields — is covered by one
macro:

```cpp
struct Person {
  xpp::String name;
  int32_t     age;
};
XPP_SERDE(Person, (name), (age))
```

The macro generates both `Serialize<Person>` and `Deserialize<Person>`
specializations. Each field is paren-wrapped, comma-separated. Max 64
fields.

### Field Attributes

Three attributes compose inline in the field list:

```cpp
struct Config {
  xpp::String host;
  int32_t     port;
  int32_t     retries;
  xpp::String api_key;
  xpp::String internal_id;
};
XPP_SERDE(Config,
  (host),
  (port,        XPP_FIELD_DEFAULT(port, 8080)),
  (retries,     XPP_FIELD_DEFAULT(retries, 3)),
  (api_key,     XPP_FIELD_RENAME(api_key, "apiKey")),
  (internal_id, XPP_FIELD_SKIP(internal_id)))
```

| Attribute | Effect |
|---|---|
| `XPP_FIELD_DEFAULT(field, value)` | If the field is absent on deserialize, use `value`. |
| `XPP_FIELD_RENAME(field, "jsonName")` | Serialize/deserialize under `"jsonName"` instead of `"field"`. |
| `XPP_FIELD_SKIP(field)` | Don't serialize; on deserialize keep the default-constructed value. |

## Tagged Variants

Sum types use `Enum<Ts...>` (alias for `Variant<Ts...>`) plus
`XPP_VARIANT_SERDE`:

```cpp
struct ShapeCircle   { double r; };
struct ShapeSquare   { double s; };
struct ShapeTriangle { double base; double height; };

XPP_SERDE(ShapeCircle,   (r))
XPP_SERDE(ShapeSquare,   (s))
XPP_SERDE(ShapeTriangle, (base), (height))

using Shape = xpp::Enum<ShapeCircle, ShapeSquare, ShapeTriangle>;

XPP_VARIANT_SERDE(Shape,
  (ShapeCircle,   "circle"),
  (ShapeSquare,   "square"),
  (ShapeTriangle, "triangle"))
```

### Strategies

| Strategy | JSON shape | Macro |
|---|---|---|
| External (default) | `{"circle": {"r": 1.0}}` | `XPP_VARIANT_SERDE` |
| Adjacent | `{"tag": "circle", "content": {"r": 1.0}}` | `XPP_VARIANT_SERDE_ADJACENT(Type, "tag", "content", ...)` |

Adjacent is how Stripe webhooks (`{"type": "...", "data": {...}}`) and
GraphQL responses are shaped — pick this when the protocol separates the
discriminator from the payload.

```cpp
using AdjShape = xpp::Enum<ShapeCircle, ShapeSquare>;
XPP_VARIANT_SERDE_ADJACENT(AdjShape, "tag", "content",
  (ShapeCircle, "circle"),
  (ShapeSquare, "square"))
```

Internal tagging (`{"type": "circle", "r": 1.0}`) is just adjacent with
a single content field — use `XPP_VARIANT_SERDE_ADJACENT` with the
appropriate `tag_field` name and put the payload fields inline.

Unknown tags produce `Err(Error{ErrorKind::UnknownField, ...})`.

## Backends

### JSON

```cpp
xpp::serde::json::Serializer ser;
xpp::serde::serialize(person, ser);
xpp::String json = ser.buffer();

auto d = xpp::serde::json::Deserializer::from_string(json).unwrap();
auto r = xpp::serde::deserialize<Person>(d);
```

### Binary

Compact, length-prefixed, little-endian. No field names on the wire.

```cpp
xpp::serde::bin::Serializer ser;
xpp::serde::serialize(person, ser);
xpp::Vec<uint8_t> bytes = ser.into_buffer();

auto d = xpp::serde::bin::Deserializer::from_bytes(bytes).unwrap();
auto r = xpp::serde::deserialize<Person>(d);
```

Wire format:

| Type | Encoding |
|---|---|
| `bool` | 1 byte (`0x00` / `0x01`) |
| `i32` / `u32` | 4 bytes LE |
| `i64` / `u64` | 8 bytes LE |
| `f32` / `f64` | 4 / 8 bytes IEEE 754 LE (NaN/Inf rejected) |
| `String` | `u32` length + UTF-8 bytes (no NUL) |
| `Option::None` | `0x00` |
| `Option::Some(v)` | `0x01` + value |
| `Vec<T>` | `u32` count + count * element |
| `struct` | field values back-to-back, no names |
| `Enum` (external) | `u32` tag_index + payload |
| `Enum` (adjacent) | `struct{tag: String, content: struct{...}}` |

## Error Model

```cpp
enum class ErrorKind {
  Unexpected,
  Eof,
  InvalidValue,
  MissingField,
  UnknownField,
  Custom,
};

struct Error {
  ErrorKind kind;
  xpp::String message;
};
```

All fallible operations return `Result<T, Error>`. Construct errors with
`xpp::serde::error(kind, "message")` or `Error::custom("message")`.

## C++11 Compatibility

The framework compiles on `-std=c++11` with `-fno-exceptions -fno-rtti`.
No use of `if constexpr`, fold expressions, structured bindings,
`std::variant`, `std::optional`, `std::string_view`, or CTAD.

## Files

| File | Purpose |
|---|---|
| `libxpp/xpp/serde/serde.h` | Traits, dispatchers, primitive specializations, concept docs |
| `libxpp/xpp/serde/error.h` | `ErrorKind` + `Error` |
| `libxpp/xpp/serde/json.h` | JSON backend |
| `libxpp/xpp/serde/bin.h` | Binary backend |
| `libxpp/xpp/serde/macros.h` | `XPP_SERDE` + `XPP_VARIANT_SERDE` macros |
