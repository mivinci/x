# xjson — JSON Parser, Builder & Serializer

## Introduction

**xjson** provides a complete JSON toolkit: DOM-style parsing and construction, SAX-style streaming parsing, and serialization. All built on libx's arena allocator and error-handling conventions.

Two parsing modes are supported:

| Mode | API | Use Case |
|------|-----|----------|
| **DOM** | `xJsonParse` / `xJsonParseCopy` | Full in-memory tree, query and mutate, serialize back |
| **SAX** | `xJsonSaxParse` | Large documents, callback-driven, no tree overhead |

## Design Philosophy

1. **Dual Memory Model** — Parse trees are arena-backed with O(1) `xJsonFree()`. Manually constructed trees use per-node `malloc` with recursive free. Ownership tracking via `XJSON_FLAG_OWNED` prevents double-free.

2. **Zero-Copy by Default** — `xJsonParse` strings point into the input buffer. Use `xJsonParseCopy` for safe copy into the arena when the input buffer must be freed.

3. **Ownership Transfer** — Set/Append/Insert operations take ownership of the value node. Replacing an existing value frees the old one automatically.

4. **Shared Tokenizer** — The DOM and SAX parsers share a common tokenizer (`json_parse.c`) that provides pure lexical helpers: skip whitespace, match literals, decode strings, and parse numbers.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                      xjson Module                        │
│                                                          │
│  ┌──────────────────┐   ┌──────────────────┐             │
│  │   json.h / json.c│   │ json_sax.h /     │             │
│  │   ───────────────│   │ json_sax.c       │             │
│  │   DOM Parse      │   │ ─────────────────│             │
│  │   DOM Construct  │   │ SAX Parse        │             │
│  │   Object/Array   │   │ Streaming Stubs  │             │
│  │   Serialize      │   │                  │             │
│  │   Free           │   │                  │             │
│  └────────┬─────────┘   └────────┬─────────┘             │
│           │                      │                       │
│           └──────────┬───────────┘                       │
│                      ▼                                   │
│           ┌─────────────────────┐                        │
│           │  json_parse.h / .c  │  (internal, XCAPI_LOCAL)│
│           │  ───────────────────│                        │
│           │  Tokenizer:         │                        │
│           │   skip_ws, match,   │                        │
│           │   string, number    │                        │
│           └─────────────────────┘                        │
└─────────────────────────────────────────────────────────┘
```

## DOM API

### Parse

| Function | Description |
|----------|-------------|
| `xJsonParse(json, len)` | Zero-copy parse. Strings point into input buffer. |
| `xJsonParseCopy(json, len)` | Safe-copy parse. All strings copied into arena. |

Both return `NULL` on parse failure. The caller must free the tree with `xJsonFree()`.

### Query

| Function | Return | UB if node is not... |
|----------|--------|---------------------|
| `xJsonType(node)` | `XJSON_NULL`, `XJSON_BOOL`, `XJSON_INT`, `XJSON_DOUBLE`, `XJSON_STRING`, `XJSON_ARRAY`, `XJSON_OBJECT` | — |
| `xJsonBool(node)` | `int` (0 or 1) | XJSON_BOOL |
| `xJsonInt(node)` | `int64_t` | XJSON_INT |
| `xJsonDouble(node)` | `double` | XJSON_DOUBLE |
| `xJsonString(node)` | `const char *` (NUL-terminated) | XJSON_STRING |
| `xJsonStringLength(node)` | `size_t` (byte count) | XJSON_STRING |

### Construct

Each constructor returns a malloc-backed node or `NULL` on allocation failure. Constructed nodes use the malloc memory model and are freed recursively by `xJsonFree()`.

| Function | Creates |
|----------|---------|
| `xJsonNewNull()` | `null` |
| `xJsonNewBool(v)` | `true` or `false` |
| `xJsonNewInt(v)` | Integer |
| `xJsonNewDouble(v)` | Double |
| `xJsonNewString(str)` | String (NUL-terminated input) |
| `xJsonNewStringN(str, len)` | String with explicit length (supports embedded NULs) |
| `xJsonNewArray()` | Empty array |
| `xJsonNewObject()` | Empty object |

### Object Operations

| Function | Description |
|----------|-------------|
| `xJsonObjectGet(obj, key)` | Look up a value by key. Returns `NULL` if not found. Returned node is still owned by `obj`. |
| `xJsonObjectSet(obj, key, val)` | Set a key-value pair. Takes ownership of `val`. Replaces existing key. Returns 0 on success. |
| `xJsonObjectDel(obj, key)` | Remove a key and free its value. No-op if key not found. |
| `xJsonObjectSize(obj)` | Return the number of key-value pairs. |

### Object Iterator

```c
xJsonIterator *it = xJsonNewIterator(obj);
while (xJsonIteratorNext(it)) {
    const char *key   = xJsonIteratorKey(it, NULL);
    xJson      *value = xJsonIteratorValue(it);
    // use key and value (value still owned by obj)
}
xJsonFree(it);  // iterator must be freed independently
```

Modifying the object during iteration on the key returned by the iterator invalidates the iterator.

### Array Operations

| Function | Description |
|----------|-------------|
| `xJsonArrayGet(arr, idx)` | Return the element at `idx`. Negative indices count from end. Returns `NULL` on OOB. |
| `xJsonArraySet(arr, idx, val)` | Replace element at `idx`. Takes ownership of `val`, frees old element. Returns 0 on success. |
| `xJsonArrayAppend(arr, val)` | Append to end. Takes ownership of `val`. Returns 0 on success. |
| `xJsonArrayInsert(arr, idx, val)` | Insert at `idx` (0..size). Takes ownership of `val`. Returns 0 on success. |
| `xJsonArrayRemove(arr, idx)` | Remove and free the element at `idx`. No-op on OOB. |
| `xJsonArraySize(arr)` | Return the number of elements. |

### Serialize

| Function | Output |
|----------|--------|
| `xJsonStringify(node)` | Compact JSON string: `{"a":1}` |
| `xJsonStringifyPretty(node)` | Pretty-printed with 2-space indent |
| `xJsonStringifyTo(node, pretty, buf, len)` | Write to caller-supplied buffer (like `snprintf`). `*len` updated to bytes including NUL. |

`xJsonStringify` and `xJsonStringifyPretty` return `malloc`'d strings; the caller must `free()` them.

### Free

```c
xJsonFree(ptr);
```

Dispatch based on memory model:
- **Arena-backed** (parse trees): destroys the arena in O(1)
- **Malloc-backed** (constructed trees): recursively walks and frees the subtree
- **Iterator**: frees the iterator struct
- **NULL**: no-op
- **Owned node** (`XJSON_FLAG_OWNED`): no-op (already transferred into a parent)

## SAX API

### One-Shot SAX

```c
int xJsonSaxParse(const char *json, size_t len,
                  const xJsonSaxHandler *handler, void *ctx);
```

Parses a complete JSON document synchronously, firing callbacks as tokens are encountered:

```c
XDEF_STRUCT(xJsonSaxHandler) {
  int (*on_null)(void *ctx);
  int (*on_bool)(void *ctx, int v);
  int (*on_int)(void *ctx, int64_t v);
  int (*on_double)(void *ctx, double v);
  int (*on_string)(void *ctx, const char *s, size_t len);
  int (*on_key)(void *ctx, const char *s, size_t len);
  int (*on_array_begin)(void *ctx);
  int (*on_array_end)(void *ctx);
  int (*on_object_begin)(void *ctx);
  int (*on_object_end)(void *ctx);
};
```

Each callback returns 0 to continue or non-zero to abort (the non-zero value is returned by `xJsonSaxParse`). String values are arena-backed and valid only for the duration of the callback — copy them if needed.

Returns: `0` on success, `-1` on parse error, or the callback's non-zero abort value.

### Streaming SAX

```c
xJsonSax       *xJsonSaxCreate(&handler, ctx, max_depth);
xJsonSaxResult  xJsonSaxFeed(sax, data, len);
xJsonSaxResult  xJsonSaxFinalize(sax);
void            xJsonSaxReset(sax);
void            xJsonSaxDestroy(sax);
```

Feed bytes incrementally as they arrive. Callbacks fire when complete tokens are available:

| Result | Meaning |
|--------|---------|
| `xJsonSaxResult_NeedMore` (1) | Parser expects more data |
| `xJsonSaxResult_Done` (0) | Document complete |
| `xJsonSaxResult_Error` (-1) | Parse error |

Call `xJsonSaxFinalize()` after the last `xJsonSaxFeed()` to detect truncated documents.

> **Note**: Streaming is currently stub-only (returns `xJsonSaxResult_Error`). The full state-machine implementation is planned for a future release. Use `xJsonSaxParse` for synchronous SAX parsing.

## Quick Start

### DOM Usage

```c
#include <stdio.h>
#include <x/json/json.h>

int main(void) {
    // Parse a JSON string
    const char *json = "{\"name\":\"leo\",\"scores\":[95,87,92]}";
    xJson *root = xJsonParse(json, strlen(json));
    if (!root) return 1;

    // Query
    xJson *name   = xJsonObjectGet(root, "name");
    xJson *scores = xJsonObjectGet(root, "scores");
    printf("Name: %s\n", xJsonString(name));
    printf("Score 0: %lld\n", (long long)xJsonInt(xJsonArrayGet(scores, 0)));

    // Serialize
    char *out = xJsonStringifyPretty(root);
    printf("%s\n", out);
    free(out);

    xJsonFree(root);
    return 0;
}
```

### Building from Scratch

```c
xJson *obj = xJsonNewObject();
xJsonObjectSet(obj, "id",    xJsonNewInt(1));
xJsonObjectSet(obj, "name",  xJsonNewString("leo"));
xJsonObjectSet(obj, "admin", xJsonNewBool(1));

xJson *tags = xJsonNewArray();
xJsonArrayAppend(tags, xJsonNewString("c"));
xJsonArrayAppend(tags, xJsonNewString("json"));
xJsonObjectSet(obj, "tags", tags);

char *json = xJsonStringify(obj);
// {"id":1,"name":"leo","admin":true,"tags":["c","json"]}
printf("%s\n", json);
free(json);
xJsonFree(obj);
```

### SAX Usage

```c
#include <x/json/json_sax.h>

static int on_int(void *ctx, int64_t v) {
    int64_t *sum = (int64_t *)ctx;
    *sum += v;
    return 0;  // continue
}

int main(void) {
    xJsonSaxHandler handler = {0};
    handler.on_int = on_int;

    int64_t sum = 0;
    int r = xJsonSaxParse("[1,2,3,4,5]", 11, &handler, &sum);
    if (r == 0) printf("Sum: %lld\n", (long long)sum);  // Sum: 15
    return r;
}
```

## Memory Model

```
Parse Tree (arena-backed)           Constructed Tree (malloc-backed)
────────────────────────            ────────────────────────────────
xJsonParse(str, len)                xJsonNewObject()
    │                                   │
    ▼                                   ▼
┌──────────┐                       ┌──────────┐
│ xArena   │                       │ xJson_*  │ (malloc)
│ ┌──────┐ │                       │   ├─key  │ (malloc)
│ │nodes │ │                       │   ├─str  │ (malloc)
│ │strings│                        │   └─next │ ...
│ └──────┘ │                       │ xJson_*  │ (malloc)
└──────────┘                       │   └─...  │
    │                                   │
xJsonFree(root)                   xJsonFree(root)
  → xArenaDestroy (O(1))            → recursive walk + free
```

**Important**: Mixing parse trees with constructed trees is unsupported and may lead to use-after-free or double-free.

## Type Constants

| Constant | Value | JSON Type |
| ---------- | ------- | ----------- |
| `XJSON_NULL` | 0x00 | `null` |
| `XJSON_BOOL` | 0x01 | `true` / `false` |
| `XJSON_INT` | 0x02 | integer |
| `XJSON_DOUBLE` | 0x03 | floating-point |
| `XJSON_STRING` | 0x04 | string |
| `XJSON_ARRAY` | 0x05 | array |
| `XJSON_OBJECT` | 0x06 | object |

## Relationship with Other Modules

- **xbase** — Uses `xArena` for memory management in parse trees. Follows `XCAPI` / `XCAPI_LOCAL` visibility conventions. Error handling via return values (0 = success, -1 = error).
- **xhttp** — Can be used to parse JSON request/response bodies. No direct dependency — applications combine both modules as needed.
