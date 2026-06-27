## ADDED Requirements

### Requirement: xRandomBytes — cross-platform secure random

`xRandomBytes` SHALL fill a buffer with cryptographically secure random bytes. It uses platform-native APIs (getrandom on Linux, getentropy on macOS, BCryptGenRandom on Windows) with `/dev/urandom` fallback.

#### Scenario: Generate random bytes

- **WHEN** `xRandomBytes(buf, 16)` is called
- **THEN** all 16 bytes are filled with random data
- **AND** the return value is `xErrno_Ok`

#### Scenario: Null buffer

- **WHEN** `xRandomBytes(NULL, 16)` is called
- **THEN** the return value is `xErrno_InvalidArg`
