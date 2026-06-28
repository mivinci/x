# random.h — Cross-Platform Secure Random

## Introduction

`random.h` provides `xRandomBytes()`, a cross-platform function for generating cryptographically secure random bytes. It uses platform-native APIs where available, falling back to `/dev/urandom` as a universal fallback. This is the randomness source used by the UUID module ([`uuid.h`](../crypto/uuid.md)).

## API

```c
XCAPI(xErrno) xRandomBytes(void *buf, size_t len);
```

### Parameters

| Parameter | Description |
| --- | --- |
| `buf` | Destination buffer. Must not be NULL. |
| `len` | Number of random bytes to generate. |

### Return Value

| Return | Description |
| --- | --- |
| `xErrno_Ok` | Success — `buf` has been filled with `len` random bytes. |
| `xErrno_InvalidArg` | `buf` is NULL. |

## Platform Backends

| Platform | Backend | Notes |
| --- | --- | --- |
| Linux (glibc ≥ 2.25) | `getrandom()` | Direct kernel CSPRNG, no fd required |
| macOS / BSD | `getentropy()` | 256 bytes max per call, looped for larger requests |
| Windows | `BCryptGenRandom()` | CNG API, cryptographically secure |
| Fallback (any POSIX) | `/dev/urandom` | Always available, but requires a fd |

## Usage Examples

### Basic usage

```c
#include <x/base/random.h>

// Generate 32 random bytes
uint8_t buf[32];
xErrno err = xRandomBytes(buf, sizeof(buf));
if (err != xErrno_Ok) {
    // Handle error (buf is NULL, or platform failure)
}
```

### Generate a random 64-bit integer

```c
uint64_t val;
xRandomBytes(&val, sizeof(val));
```

### Generate a random 128-bit UUID seed

```c
uint8_t seed[16];
xRandomBytes(seed, sizeof(seed));
// Use seed with your own UUID generator or as a session token
```

## See Also

- [`uuid.h`](../crypto/uuid.md) — UUID generation using `xRandomBytes` for v4 and v7
