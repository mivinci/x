## ADDED Requirements

### Requirement: xUuid — 16-byte UUID value type

```c
XDEF_STRUCT(xUuid) {
    uint8_t bytes[16];
};
```

UUIDs are stack-allocatable 16-byte values, passed by value. No opaque handle, no lifetime management.

### Requirement: UUID v4 — random

`xUuidV4()` SHALL generate a version-4 UUID using `xRandomBytes`. Version (4 bits) and variant (2 bits) are set per RFC 4122 §4.4.

#### Scenario: Generate v4 UUID

- **WHEN** `xUuidV4()` is called
- **THEN** the returned UUID has version nibble = 4 and variant bits = 10
- **AND** two consecutive calls produce different UUIDs

### Requirement: UUID v7 — time-ordered random (RFC 9562)

`xUuidV7()` SHALL generate a version-7 UUID with 48-bit Unix millisecond timestamp, 12-bit random, and 62-bit random. UUIDs generated in sequence are sortable by creation time.

#### Scenario: Generate v7 UUID

- **WHEN** `xUuidV7()` is called
- **THEN** the returned UUID has version nibble = 7
- **AND** the version nibble is 7

#### Scenario: v7 sortability

- **WHEN** two v7 UUIDs are generated at different times (T1 < T2)
- **THEN** `xUuidCompare(uuid1, uuid2) < 0`

### Requirement: UUID v5 — namespace + name (SHA-1)

`xUuidV5(namespace, name)` SHALL generate a deterministic UUID by hashing the namespace UUID and name with SHA-1. The same inputs always produce the same UUID.

#### Scenario: v5 determinism

- **WHEN** `xUuidV5(dns_ns, "example.com")` is called twice
- **THEN** both calls return the same UUID

#### Scenario: v5 with predefined namespace

- **WHEN** `xUuidV5(*xUuidNamespaceDns(), "example.com")` is called
- **THEN** the result matches the known test vector from RFC 4122 Appendix A

### Requirement: String formatting

`xUuidToString(uuid, buf)` SHALL format a UUID as a lowercase hyphenated string: `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx` (36 chars + NUL = 37 bytes).

#### Scenario: Format UUID

- **WHEN** `xUuidToString(uuid, buf)` is called with a UUID of all zeros
- **THEN** `buf` contains `"00000000-0000-0000-0000-000000000000"`

### Requirement: String parsing

`xUuidFromString(str, &out)` SHALL parse a UUID string (with or without hyphens, case-insensitive) into an `xUuid`.

#### Scenario: Parse valid UUID

- **WHEN** `xUuidFromString("550e8400-e29b-41d4-a716-446655440000", &out)` is called
- **THEN** the return value is `xErrno_Ok`
- **AND** `out.bytes` matches the expected 16 bytes

#### Scenario: Parse without hyphens

- **WHEN** `xUuidFromString("550e8400e29b41d4a716446655440000", &out)` is called
- **THEN** the return value is `xErrno_Ok`

#### Scenario: Parse invalid string

- **WHEN** `xUuidFromString("not-a-uuid", &out)` is called
- **THEN** the return value is `xErrno_InvalidArg`

### Requirement: Comparison and nil check

- `xUuidCompare(a, b)` — memcmp-style comparison (-1, 0, 1)
- `xUuidIsNil(uuid)` — true if all 16 bytes are zero

#### Scenario: Compare equal UUIDs

- **WHEN** comparing two UUIDs with identical bytes
- **THEN** `xUuidCompare` returns 0

#### Scenario: Nil UUID

- **WHEN** `xUuidIsNil` is called with a zero-initialized UUID
- **THEN** the return value is true

### Requirement: Predefined namespace UUIDs

Per RFC 4122 Appendix C:

- `xUuidNamespaceDns()` → `6ba7b810-9dad-11d1-80b4-00c04fd430c8`
- `xUuidNamespaceUrl()` → `6ba7b811-9dad-11d1-80b4-00c04fd430c8`
