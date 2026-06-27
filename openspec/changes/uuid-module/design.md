## Context

libx needs UUID generation for WebSocket keys, session IDs, correlation IDs, and DNS transaction IDs. Currently no UUID support exists. The module goes in `libx/x/base/` (a base primitive, not a separate module) alongside existing utilities like `hex.c`, `base64.c`, `base58.c`.

## Goals / Non-Goals

**Goals:**
- UUID v4 (random), v7 (time-ordered random), v5 (namespace + name SHA-1)
- String format: `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx` (lowercase, hyphenated)
- Parse from string back to xUuid
- Cross-platform secure random source (`xRandomBytes`)
- Minimal API surface, no external dependencies beyond xbase + xcrypto

**Non-Goals:**
- UUID v1 (MAC address — privacy concern)
- UUID v2 (DCE Security — rarely used)
- UUID v3 (MD5 — v5 with SHA-1 is better)
- UUID v6 (reordered v1 — same MAC address issue)
- Batch generation / UUID pools
- ORM/database integration

## Decisions

### D1: Placement — `libx/x/base/`, not a separate module

UUID is a base primitive (like hex, base64, base58). It belongs in xbase, not as a separate top-level module. Files: `uuid.h`, `uuid.c`, `uuid_test.cpp`.

### D2: `xRandomBytes` — cross-platform secure random

New function in xbase for cryptographically secure random bytes:

| Platform | Implementation |
|----------|---------------|
| Linux | `getrandom()` (since 3.17) with `/dev/urandom` fallback |
| macOS | `getentropy()` (since 10.12) with `/dev/urandom` fallback |
| Windows | `BCryptGenRandom()` |

Placed in `libx/x/base/random.c`, declared in `libx/x/base/random.h`.

### D3: UUID v7 — RFC 9562

```
 48 bits: unix_ts_ms (big-endian)
 12 bits: rand_a (random)
  2 bits: version (0111)
 62 bits: rand_b (random, counter optional)
```

Version 7 is time-ordered — UUIDs generated in sequence are sortable by creation time. This is valuable for database indexes, log correlation, and event sourcing.

### D4: UUID v5 — uses existing xcrypto SHA-1

v5 = SHA-1(namespace_uuid || name). The SHA-1 implementation already exists in `libx/x/crypto/sha1.h`. Link xbase uuid.c against xcrypto.

### D5: API style — value type, not opaque handle

UUIDs are 16-byte values. Use a struct (not a handle) for zero-allocation usage:

```c
XDEF_STRUCT(xUuid) {
    uint8_t bytes[16];
};
```

Stack-allocatable, pass by value, no lifetime management.

### D6: String format — lowercase, hyphenated

Standard format: `550e8400-e29b-41d4-a716-446655440000` (36 chars + NUL = 37 bytes). Parsing accepts both upper and lowercase.

## Risks / Trade-offs

- **[SHA-1 dependency for v5]** → xcrypto is already a dependency of the aggregated `x` target. For fine-grained linking, `xbase` alone doesn't include SHA-1. Users who only link `xbase` and want v5 must also link `xcrypto`. v4 and v7 don't need xcrypto.
- **[getrandom availability]** → Linux < 3.17 and macOS < 10.12 fall back to `/dev/urandom`. Very old systems only.
- **[v7 clock resolution]** → v7 uses millisecond timestamps. Multiple UUIDs generated in the same millisecond differ only in the random portion. This is fine — RFC 9562 allows this.
