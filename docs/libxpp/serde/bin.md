# Binary Backend

[← serde](README.md)

## Introduction

`xpp::serde::bin::Serializer` and `bin::Deserializer` provide a compact
length-prefixed binary backend. Field names are not encoded on the wire —
structs are field values back-to-back. All multi-byte integers are
little-endian.

This format is **not self-describing** — both ends must agree on the
schema (field types and order). It is roughly 2-4x smaller than JSON for
typical structs and faster to parse (no string comparisons for field
names).

## Usage

### Serialize

```cpp
#include <xpp/serde/bin.h>
#include <xpp/serde/serde.h>

Person p{"Alice", 30};

xpp::serde::bin::Serializer ser;
xpp::serde::serialize(p, ser);
xpp::Vec<uint8_t> bytes = ser.into_buffer();
// bytes contains the compact binary encoding
```

`into_buffer()` moves the internal buffer out (the `Serializer` is left
empty). `buffer()` returns a `Span<const uint8_t>` borrow instead — use
this when you need to inspect the bytes without taking ownership.

`reset()` clears the internal state for reuse.

### Deserialize

```cpp
auto d_res = xpp::serde::bin::Deserializer::from_bytes(bytes);
if (!d_res.is_ok()) {
  // Should not happen for a valid Vec<uint8_t>, but check anyway
  return;
}
auto d = std::move(d_res).unwrap();

auto r = xpp::serde::deserialize<Person>(d);
if (r.is_ok()) {
  Person &p = r.unwrap();
  // p.name == "Alice", p.age == 30
} else {
  xpp::serde::Error &e = r.unwrap_err();
  // e.g. Eof (truncated input), InvalidValue (bad UTF-8)
}
```

Entry points:

| Method | Input | Ownership |
|---|---|---|
| `Deserializer::from_bytes(const Vec<uint8_t>&)` | Vec | Copies the bytes into the deserializer |
| `Deserializer::from_bytes(const uint8_t*, size_t)` | Raw pointer + length | Copies the bytes |
| `Deserializer::borrow(Span<const uint8_t>)` | Span | Borrows — caller must keep the buffer alive |

The `Deserializer` holds its own copy by default (safe). `borrow` is the
zero-copy escape hatch for hot paths — the caller must ensure the source
buffer outlives the `Deserializer`.

## Wire Format

| Type | Encoding | Size |
|---|---|---|
| `bool` | `0x00` (false) / `0x01` (true) | 1 byte |
| `i32` / `u32` | little-endian | 4 bytes |
| `i64` / `u64` | little-endian | 8 bytes |
| `f32` | IEEE 754 LE | 4 bytes |
| `f64` | IEEE 754 LE | 8 bytes |
| `String` | `u32` length + UTF-8 bytes (no NUL terminator) | 4 + N |
| `Option::None` | `0x00` | 1 byte |
| `Option::Some(v)` | `0x01` + value | 1 + sizeof(v) |
| `Vec<T>` | `u32` count + count × element | 4 + Σ |
| `struct` | field values back-to-back, no names | Σ |
| `Enum` (external) | `u32` tag_index + payload | 4 + payload |
| `Enum` (adjacent) | `struct{tag: String, content: struct{...}}` | varies |

Self-delimiting for fixed-width primitives, `Option`, and `Vec`. Structs
rely on the visitor knowing the field count (passed via
`deserialize_struct`'s `n` parameter) — there are no length prefixes or
field separators between struct fields.

### Example encoding

```cpp
struct Point { int32_t x; int32_t y; };
XPP_SERDE(Point, (x), (y))
```

`Point{42, -7}` encodes to 8 bytes:

```
2a 00 00 00   f9 ff ff ff
└─ x = 42 ─┘  └─ y = -7 ─┘
```

For comparison, the JSON encoding is `{"x":42,"y":-7}` — 15 bytes,
nearly 2x larger.

## Schema Evolution

Because field names are not on the wire, schema changes require care:

| Change | Compatible? | Notes |
|---|---|---|
| Add field at the **end** | No (old data is shorter) | Reader expects the field, hits `Eof`. |
| Add field at the end + old reader | Yes | Old reader stops after existing fields; new field ignored. |
| Remove field | No | Reader expects it, data is misaligned. |
| Reorder fields | No | Binary is positional. |
| Change field type | No | Width/encoding mismatch. |
| Add `Option<T>` field at end | Partial | Old data has no byte for it — reader hits `Eof`. Use a version prefix instead. |

For forward-compatible binary protocols, version the format explicitly:
prefix the payload with a `u32` version and dispatch on it in the
`Deserialize<T>` specialization. Alternatively, use `XPP_FIELD_SKIP` on
the sender side to omit new fields — but both ends must agree on which
fields are skipped.

## Cross-Backend Interop

The same `Serialize<T>` / `Deserialize<T>` specialization works for both
JSON and binary. You can serialize to JSON, deserialize from JSON, then
re-serialize to binary (or vice versa) with no code changes:

```cpp
// JSON -> Person -> binary
auto jd = json::Deserializer::from_string(json_str).unwrap();
Person p = serde::deserialize<Person>(jd).unwrap();

bin::Serializer ser;
serde::serialize(p, ser);
Vec<uint8_t> bytes = ser.into_buffer();
```

This is useful for transcoding at protocol boundaries (e.g. receive JSON
from an HTTP API, store as binary in a cache).

## Error Handling

Binary-specific failures:

| Scenario | ErrorKind |
|---|---|
| Truncated input (ran out of bytes mid-field) | `Eof` |
| Invalid UTF-8 in `String` | `InvalidValue` |
| `f64` is NaN/Inf on serialize | `InvalidValue` |
| `Option` discriminator byte is neither `0x00` nor `0x01` | `InvalidValue` |
| `Enum` tag_index out of range | `InvalidValue` |

Unlike JSON, there is no "unknown field" concept — the reader consumes
exactly the fields it expects, in order. Extra trailing bytes are
silently ignored (the `Deserializer` stops at the last field, not at end
of buffer).
