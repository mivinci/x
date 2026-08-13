# Serde

[← libxpp](../README.md)

## Introduction

`xpp::serde` is a trait-based serialization framework modeled on Rust's
[`serde`](https://serde.rs). A data type specializes `Serialize<T>` /
`Deserialize<T>` once, and any backend that satisfies the `Serializer` /
`Deserializer` concept can drive it. Backends are duck-typed (no vtable)
and template-monomorphized per call site.

**Why not just call `to_json()` / `from_json()` on each type?**

- Every format (JSON, binary, TOML, ...) would need its own pair of
  methods per type — N types × M formats = N×M hand-written conversions.
- Serde inverts this: each type implements **one** pair of traits, each
  format implements **one** pair of backends, and the two meet at the
  call site. N types + M formats instead of N×M.
- The same `XPP_SERDE(Person, (name)(age))` macro works for JSON, binary,
  and any future backend — zero edits to user types when adding a format.

Two backends ship in-tree:

- `xpp::serde::json` — wraps `libx/x/json/` (DOM-based, human-readable)
- `xpp::serde::bin` — compact length-prefixed binary format

## Quick Start

```cpp
#include <xpp/serde/json.h>
#include <xpp/serde/serde.h>
#include <xpp/string.h>

struct Person {
  xpp::String name;
  int32_t     age;
};

// One macro generates both Serialize<Person> and Deserialize<Person>.
XPP_SERDE(Person, (name), (age))

int main() {
  Person p{"Alice", 30};

  // Serialize to JSON (one-step: mirrors serde_json::to_string)
  auto j = xpp::serde::json::to_string(p);
  xpp::String json = std::move(j).unwrap();
  // json == R"({"name":"Alice","age":30})"

  // Deserialize back
  auto d_res = xpp::serde::json::Deserializer::from_string(json);
  auto d = std::move(d_res).unwrap();
  auto r = xpp::serde::deserialize<Person>(d);
  if (r.is_ok()) {
    Person &back = r.unwrap();
    // back.name == "Alice", back.age == 30
  } else {
    xpp::serde::Error &e = r.unwrap_err();
    // e.kind, e.message
  }
}
```

Switching to binary is one line at the call site — `Person` and the
`XPP_SERDE` macro stay untouched:

```cpp
#include <xpp/serde/bin.h>

xpp::serde::bin::Serializer ser;
xpp::serde::serialize(p, ser);
xpp::Vec<uint8_t> bytes = ser.into_buffer();

auto d = xpp::serde::bin::Deserializer::from_bytes(bytes).unwrap();
auto r = xpp::serde::deserialize<Person>(d);
```

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
- Call sites use `serde::serialize(v, ser)` / `serde::deserialize<T>(d)`,
  not the trait directly.

Built-in specializations: `bool`, `int32_t`, `int64_t`, `uint32_t`,
`uint64_t`, `float`, `double`, `xpp::String`, `Option<T>`, `Vec<T>`.

### Hand-writing a specialization

The `XPP_SERDE` macro covers the 80% case, but understanding the
hand-written form is useful for custom logic (validation, computed
fields, third-party types). The canonical example:

```cpp
struct Person {
  xpp::String name;
  int32_t     age = 0;
};

namespace xpp::serde {

template <>
struct Serialize<Person> {
  template <class S>
  static Result<Void, Error> run(const Person &p, S &s) {
    // 1. Open a struct scope with the type name and field count.
    XPP_SERDE_TRY_VAR(scope, s.serialize_struct("Person", 2));
    // 2. Emit each field by key. serde::serialize dispatches to the
    //    field type's own Serialize specialization.
    XPP_SERDE_TRY(scope.field("name", p.name));
    XPP_SERDE_TRY(scope.field("age", p.age));
    // 3. Close the scope.
    return scope.end();
  }
};

template <>
struct Deserialize<Person> {
  template <class D>
  static Result<Person, Error> run(D &d) {
    struct Visitor {
      Result<Person, Error> visit_map(typename D::MapAccess &m) {
        Person p{};
        bool   got_name = false, got_age = false;
        while (true) {
          XPP_SERDE_TRY_VAR(key, m.next_key());
          if (key.is_none()) break;  // end of map
          const xpp::String &k = key.unwrap();
          if (k == "name") {
            XPP_SERDE_TRY_VAR(v, m.template next_value<xpp::String>());
            p.name = std::move(v); got_name = true;
          } else if (k == "age") {
            XPP_SERDE_TRY_VAR(v, m.template next_value<int32_t>());
            p.age = v; got_age = true;
          } else {
            // Unknown field — skip its value. Forward-compatible.
            XPP_SERDE_TRY(m.next_value_ignored());
          }
        }
        if (!got_name) return err(error(ErrorKind::MissingField, "missing 'name'"));
        if (!got_age)  return err(error(ErrorKind::MissingField, "missing 'age'"));
        return ok(std::move(p));
      }
    };
    static const char *const kFields[] = {"name", "age"};
    return d.deserialize_struct("Person", kFields, 2, Visitor{});
  }
};

} // namespace serde
} // namespace xpp
```

Key points:

- `XPP_SERDE_TRY(expr)` propagates the error from `expr` if it's `Err`.
  `XPP_SERDE_TRY_VAR(name, expr)` also captures the `Ok` value.
- The `Visitor` struct's `visit_map` receives a `MapAccess&` — call
  `next_key()` (returns `Option<String>`, `None` at end) and
  `next_value<T>()` (returns `Result<T, Error>`).
- Unknown fields are **skipped by default** — forward-compatible. Call
  `next_value_ignored()` to advance the cursor without parsing.
- Missing required fields produce `Err(Error{ErrorKind::MissingField, ...})`.

### Nested types

`Serialize<T>` dispatches recursively, so nesting "just works" — no
special syntax. A field of type `Person` inside another struct calls
`Serialize<Person>::run` when emitted:

```cpp
struct Team {
  xpp::String  team_name;
  Person       lead;
  Vec<Person>  members;
};
XPP_SERDE(Team, (team_name), (lead), (members))
```

Serializes to:

```json
{
  "team_name": "infra",
  "lead": {"name": "Alice", "age": 30},
  "members": [
    {"name": "Bob", "age": 25},
    {"name": "Carol", "age": 28}
  ]
}
```

`Option<T>` serializes as `null` (JSON) or a discriminator tag (binary)
when `None`; `Vec<T>` serializes as an array (JSON) or length-prefixed
sequence (binary). Both round-trip automatically — no macro config needed.

## The `XPP_SERDE` Macro

The macro generates the same `Serialize<T>` / `Deserialize<T>`
specializations shown above, mechanically. Each field is paren-wrapped,
comma-separated. Max 64 fields.

```cpp
XPP_SERDE(Type, (field1), (field2), ..., (fieldN))
```

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

Serializing `Config{"localhost", 0, 0, "sk_123", "i_456"}` produces:

```json
{"host":"localhost","port":0,"retries":0,"apiKey":"sk_123"}
```

Note: `internal_id` is absent (SKIP), `api_key` is emitted as `apiKey`
(RENAME), and `port`/`retries` use their actual values on serialize
(DEFAULT only fills in on deserialize when the field is missing).

| Attribute | Serialize | Deserialize |
|---|---|---|
| `XPP_FIELD_DEFAULT(field, value)` | Emits the actual field value. | If the field is absent, uses `value` instead. |
| `XPP_FIELD_RENAME(field, "jsonName")` | Emits key `"jsonName"`. | Matches key `"jsonName"`. |
| `XPP_FIELD_SKIP(field)` | Does not emit the field. | Does not read; keeps the default-constructed value. |

## Tagged Variants (Enums)

Sum types — where a value is one of several alternatives, distinguished
by a tag — use `Enum<Ts...>` plus `XPP_ENUM_SERDE`. This covers LSP
messages, webhook payloads, GraphQL responses, and any protocol with a
type discriminator.

```cpp
struct ShapeCircle   { double r; };
struct ShapeSquare   { double s; };
struct ShapeTriangle { double base; double height; };

// Each alternative needs its own Serialize/Deserialize too.
XPP_SERDE(ShapeCircle,   (r))
XPP_SERDE(ShapeSquare,   (s))
XPP_SERDE(ShapeTriangle, (base), (height))

using Shape = xpp::Enum<ShapeCircle, ShapeSquare, ShapeTriangle>;

XPP_ENUM_SERDE(Shape,
  (ShapeCircle,   "circle"),
  (ShapeSquare,   "square"),
  (ShapeTriangle, "triangle"))
```

### Strategies

| Strategy | JSON shape | When to use | Macro |
|---|---|---|---|
| External (default) | `{"circle": {"r": 1.0}}` | Variant name is the wrapper key. Serde's default. | `XPP_ENUM_SERDE` |
| Adjacent | `{"tag": "circle", "content": {"r": 1.0}}` | Tag and payload are separate fields in the same object. Stripe webhooks, GraphQL. | `XPP_ENUM_SERDE_ADJACENT(Type, "tag", "content", ...)` |

External example:

```cpp
Shape c = ShapeCircle{1.0};
// JSON: {"circle":{"r":1.0}}
// Binary: [u32 tag_index=0][f64 1.0]
```

Adjacent example — useful for Stripe-style `{"type":"...", "data":{...}}`:

```cpp
using AdjShape = xpp::Enum<ShapeCircle, ShapeSquare>;
XPP_ENUM_SERDE_ADJACENT(AdjShape, "tag", "content",
  (ShapeCircle, "circle"),
  (ShapeSquare, "square"))

// JSON: {"tag":"circle","content":{"r":1.0}}
```

Internal tagging (`{"type":"circle","r":1.0}` — tag and payload fields
flat in the same object) is just adjacent with the payload fields inlined
directly. Use `XPP_ENUM_SERDE_ADJACENT` with the appropriate `tag_field`
name.

Unknown tags on deserialize produce
`Err(Error{ErrorKind::UnknownField, ...})`.

## Backends

- [JSON](json.md) — wraps `libx/x/json/`, DOM-based, human-readable
- [Binary](bin.md) — compact length-prefixed little-endian format

## Error Model

```cpp
enum class ErrorKind {
  Unexpected,      // backend-specific unexpected state
  Eof,             // ran out of input
  InvalidValue,    // type mismatch, bad UTF-8, NaN/Inf, etc.
  MissingField,    // required struct field absent on deserialize
  UnknownField,    // unknown variant tag (not unknown struct field — those are skipped)
  Custom,          // user-thrown via Error::custom
};

struct Error {
  ErrorKind kind;
  xpp::String message;
};
```

All fallible operations return `Result<T, Error>`. Construct errors with
`xpp::serde::error(kind, "message")` or `Error::custom("message")`.
Check `r.is_ok()` / `r.unwrap_err().kind` at the call site — no
exceptions cross the serde boundary.

Common error scenarios:

| Scenario | ErrorKind |
|---|---|
| Required field missing in JSON object | `MissingField` |
| Unknown variant tag (e.g. `"triangle"` when only `circle`/`square` declared) | `UnknownField` |
| Type mismatch (e.g. expecting `i32`, got string) | `InvalidValue` |
| Truncated binary input | `Eof` |
| NaN/Inf in `f64` | `InvalidValue` |

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
| `libxpp/xpp/serde/macros.h` | `XPP_SERDE` + `XPP_ENUM_SERDE` macros |
