## Why

libx has no UUID generation — WebSocket keys, session IDs, correlation IDs, and DNS transaction IDs all need unique identifiers. Currently users must bring their own UUID library or use ad-hoc random bytes. A built-in UUID module makes libx self-contained for common networking use cases.

## What Changes

- Add `xRandomBytes(buf, len)` to `xbase` — cross-platform cryptographically secure random source (getrandom on Linux, /dev/urandom on macOS, BCryptGenRandom on Windows)
- Add `uuid.h` / `uuid.c` to `libx/x/base/` — UUID generation and formatting
  - UUID v4 (random) — most common
  - UUID v7 (time-ordered random, RFC 9562) — database-friendly, sortable
  - UUID v5 (namespace + name SHA-1) — deterministic, uses existing xcrypto SHA-1
  - String formatting/parsing (`xUuidToString` / `xUuidFromString`)
  - Comparison and nil check
  - Predefined namespace UUIDs (DNS, URL) per RFC 4122
- Add `uuid_test.cpp` — round-trip, format, v4 uniqueness, v7 sortability, v5 determinism, parse tests

## Capabilities

### New Capabilities
- `uuid-generation`: UUID v4/v7/v5 generation, string formatting/parsing, comparison

### Modified Capabilities
- `xbase-random`: Add `xRandomBytes` cross-platform secure random source

## Impact

- **New code**: `libx/x/base/uuid.h` (~60 lines), `libx/x/base/uuid.c` (~350 lines), `libx/x/base/uuid_test.cpp` (~200 lines)
- **Modified code**: `libx/x/base/base.h` or new `random.h` for `xRandomBytes` declaration, `libx/x/base/random.c` (~80 lines) for implementation
- **Dependencies**: xcrypto (SHA-1 for v5, already exists), xbase
- **Build**: Add `uuid.c` and `random.c` to `libx/x/base/CMakeLists.txt`
