# JSON Backend

[← serde](README.md)

## Introduction

`xpp::serde::json::Serializer` and `json::Deserializer` wrap `libx/x/json/`,
providing a DOM-based JSON backend for the serde framework. The Serializer
builds an `xJson` tree with `xJsonNew*` (malloc-backed) and dumps it via
`xJsonStringify`. The Deserializer parses with `xJsonParseCopy` (arena-backed,
safe) and walks the DOM.

Link target: any TU including `json.h` must link `xjson`.

## Usage

### Serialize

```cpp
#include <xpp/serde/json.h>
#include <xpp/serde/serde.h>

Person p{"Alice", 30};

xpp::serde::json::Serializer ser;
xpp::serde::serialize(p, ser);
xpp::String json = ser.buffer();
// json == R"({"name":"Alice","age":30})"
```

`Serializer::buffer()` returns a `xpp::String` view of the JSON output.
Call it after `serialize()` returns `Ok`. `Serializer::reset()` clears
the internal state for reuse.

### Deserialize

```cpp
auto d_res = xpp::serde::json::Deserializer::from_string(R"({"name":"Bob","age":25})");
if (!d_res.is_ok()) {
  // Parse error (malformed JSON)
  xpp::serde::Error &e = d_res.unwrap_err();
  // e.kind == ErrorKind::InvalidValue, e.message has details
  return;
}
auto d = std::move(d_res).unwrap();

auto r = xpp::serde::deserialize<Person>(d);
if (r.is_ok()) {
  Person &p = r.unwrap();
  // p.name == "Bob", p.age == 25
} else {
  xpp::serde::Error &e = r.unwrap_err();
  // e.g. MissingField, InvalidValue
}
```

Entry points:

| Method | Input |
|---|---|
| `Deserializer::from_string(const xpp::String&)` | Owned `String` |
| `Deserializer::from_string(const char*)` | C string literal |

Both parse a copy into an arena — the input does not need to outlive the
`Deserializer`.

## Encoding

JSON is self-describing — field names are encoded as object keys, and
`Option<T>` uses `null` for `None`. No special discriminator bytes are
needed for `Some`; `serialize_some` is a plain forward to
`serde::serialize`.

### Primitives

| C++ type | JSON |
|---|---|
| `bool` | `true` / `false` |
| `int32_t` / `int64_t` | number |
| `uint32_t` / `uint64_t` | number |
| `float` / `double` | number (NaN/Inf rejected on serialize) |
| `xpp::String` | `"..."` |

### Composite types

| C++ type | JSON |
|---|---|
| `Option<T>` (None) | `null` |
| `Option<T>` (Some) | value as-is (no wrapper) |
| `Vec<T>` | `[...]` |
| `struct` (via `XPP_SERDE`) | `{"field": ...}` |
| `Enum` (external) | `{"tagString": {payload}}` |
| `Enum` (adjacent) | `{"tag": "tagString", "content": {payload}}` |

Example — a struct with `Option` and `Vec`:

```cpp
struct Group {
  xpp::String       name;
  Option<int32_t>   priority;   // null if not set
  Vec<xpp::String>  tags;
};
XPP_SERDE(Group, (name), (priority), (tags))
```

`Group{"infra", none, Vec<String>{"a","b"}}` serializes to:

```json
{"name":"infra","priority":null,"tags":["a","b"]}
```

### Tagged variants

| Strategy | JSON shape |
|---|---|
| External | `{"circle": {"r": 1.0}}` |
| Adjacent | `{"tag": "circle", "content": {"r": 1.0}}` |

See the [serde README](README.md#tagged-variants-enums) for the macro
declarations.

## Unknown Fields

The JSON deserializer **skips unknown fields by default** — if the JSON
object contains keys that the `Deserialize<T>` visitor doesn't recognize,
they are ignored via `next_value_ignored()`. This makes the format
forward-compatible: adding a field to the JSON (e.g. from a newer server)
does not break older clients.

If you need strict mode (reject unknown fields), hand-write the
`Deserialize<T>` specialization and return `Err` from the `else` branch
instead of calling `next_value_ignored()`.

## Error Handling

All errors surface as `Result<T, Error>`. Common JSON-specific failures:

| Scenario | ErrorKind | Example |
|---|---|---|
| Malformed JSON at parse time | `InvalidValue` | `from_string("{bad}")` |
| Type mismatch | `InvalidValue` | expecting `i32`, got `"hello"` |
| Required field missing | `MissingField` | `{"name":"X"}` into a struct requiring `age` |
| Unknown variant tag | `UnknownField` | `{"triangle":{...}}` when only `circle`/`square` declared |
| NaN/Inf in `f64` | `InvalidValue` | `serialize(1.0/0.0, ser)` |

Errors are non-fatal — the `Deserializer` can be reused after an error
on a different input (call `from_string` again).
