## 1. xRandomBytes — cross-platform secure random

- [ ] 1.1 Create `libx/x/base/random.h` — `xRandomBytes` declaration
- [ ] 1.2 Create `libx/x/base/random.c` — Linux getrandom, macOS getentropy, Windows BCryptGenRandom, /dev/urandom fallback
- [ ] 1.3 Add `random.c` to `libx/x/base/CMakeLists.txt`
- [ ] 1.4 Test: generate 16 bytes, verify non-zero and unique across calls
- [ ] 1.5 Test: null buffer returns xErrno_InvalidArg

## 2. UUID module

- [ ] 2.1 Create `libx/x/base/uuid.h` — xUuid struct, xUuidV4/V7/V5, xUuidToString/FromString, xUuidCompare/IsNil, namespace accessors
- [ ] 2.2 Create `libx/x/base/uuid.c` — v4 (random + version/variant bits), v7 (48-bit timestamp + random), v5 (SHA-1 hash of namespace + name)
- [ ] 2.3 Add `uuid.c` to `libx/x/base/CMakeLists.txt`
- [ ] 2.4 Test: v4 — version nibble = 4, variant bits correct, two calls produce different UUIDs
- [ ] 2.5 Test: v7 — version nibble = 7, two calls at different times are sortable
- [ ] 2.6 Test: v5 — determinism (same inputs → same UUID), RFC 4122 test vector (DNS namespace + "example.com")
- [ ] 2.7 Test: xUuidToString — all-zeros → "00000000-0000-0000-0000-000000000000", round-trip with v4
- [ ] 2.8 Test: xUuidFromString — hyphenated, non-hyphenated, uppercase, invalid string
- [ ] 2.9 Test: xUuidCompare — equal, less-than, greater-than
- [ ] 2.10 Test: xUuidIsNil — zero UUID returns true, v4 returns false
- [ ] 2.11 Test: namespace accessors — xUuidNamespaceDns/Url return correct predefined UUIDs
