# Binary Backend

[← serde](README.md)

## Introduction

`xpp::serde::bin::Serializer` and `bin::Deserializer` provide a compact
length-prefixed binary backend. Field names are not encoded on the wire —
structs are field values back-to-back. All multi-byte integers are
little-endian.

## Usage

```cpp
xpp::serde::bin::Serializer ser;
xpp::serde::serialize(person, ser);
xpp::Vec<uint8_t> bytes = ser.into_buffer();

auto d = xpp::serde::bin::Deserializer::from_bytes(bytes).unwrap();
auto r = xpp::serde::deserialize<Person>(d);
```

## Wire Format

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

Self-delimiting for fixed-width primitives and seq. Structs rely on the
visitor knowing the field count (passed via `deserialize_struct` `n`),
matching the JSON backend's contract.
