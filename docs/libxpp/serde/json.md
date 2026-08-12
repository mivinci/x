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

```cpp
xpp::serde::json::Serializer ser;
xpp::serde::serialize(person, ser);
xpp::String json = ser.buffer();

auto d = xpp::serde::json::Deserializer::from_string(json).unwrap();
auto r = xpp::serde::deserialize<Person>(d);
```

## Encoding

JSON is self-describing — field names are encoded as object keys, and
`Option<T>` uses `null` for `None`. No special discriminator bytes are
needed for `Some`; `serialize_some` is a plain forward to
`serde::serialize`.

Tagged variants:

| Strategy | JSON shape |
|---|---|
| External | `{"circle": {"r": 1.0}}` |
| Adjacent | `{"tag": "circle", "content": {"r": 1.0}}` |
