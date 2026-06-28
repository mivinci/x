# uuid.h — UUID Generation

## Introduction

`uuid.h` provides RFC 4122 / RFC 9562 UUID generation (v4 random, v7 time-ordered, v5 namespace+name SHA-1), string formatting, parsing, and comparison. UUIDs are 16-byte value types (`xUuid`) — stack-allocatable, no lifetime management. Random UUIDs use `xRandomBytes` for cryptographically secure randomness. v5 uses `xSha1` from [`xcrypto`](../crypto/README.md).

## Types

### xUuid — value type

```c
XDEF_STRUCT(xUuid) { uint8_t bytes[16]; };
```

### xUuidType — generation mode

```c
typedef enum {
  xUuidType_V4 = 1 << 0,
  xUuidType_V7 = 1 << 1,
  xUuidType_V5 = 1 << 2,
} xUuidType;
```

## API

### Generation

```c
XCAPI(xUuid) xUuidV4(void);
XCAPI(xUuid) xUuidV7(void);
XCAPI(xUuid) xUuidV5(xUuid ns, const char *name);
```

### Formatting

```c
XCAPI(void)   xUuidToString(xUuid uuid, char buf[37]);
XCAPI(xErrno) xUuidFromString(const char *str, xUuid *out);
```

### Comparison

```c
XCAPI(int)  xUuidCompare(xUuid a, xUuid b);
XCAPI(bool) xUuidIsNil(xUuid uuid);
```

### Namespace UUIDs

```c
XCAPI(const xUuid *) xUuidNamespaceDns(void);  // 6ba7b810-9dad-11d1-80b4-00c04fd430c8
XCAPI(const xUuid *) xUuidNamespaceUrl(void);  // 6ba7b811-9dad-11d1-80b4-00c04fd430c8
```

## Usage Examples

### Basic usage

```c
// v4 — random
xUuid id = xUuidV4();
char buf[37];
xUuidToString(id, buf);  // "550e8400-e29b-41d4-a716-446655440000"

// v7 — time-ordered, database-friendly
xUuid sortable = xUuidV7();

// v5 — deterministic, same inputs = same result
xUuid dns_id = xUuidV5(*xUuidNamespaceDns(), "example.com");

// comparison
if (xUuidIsNil(id)) printf("nil\n");
if (xUuidCompare(a, b) < 0) printf("a < b\n");
```

## See Also

- [`random.h`](random.md) — `xRandomBytes` cross-platform secure random
- [xcrypto](../crypto/README.md) — SHA-1 backend used by v5
