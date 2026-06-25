# libx

C foundation libraries for network applications.

## Modules

| Module | Description |
|--------|-------------|
| **xbase** | Event loop (kqueue/epoll/poll), intrusive data structures, memory, encoding, threading |
| **xlog** | Async logging with file rotation, three flush modes (timer/notify/mixed) |
| **xbuf** | Three buffer types: linear auto-growing, fixed-size ring, zero-copy block-chained IOBuf |
| **xnet** | TCP (async connect/listen), TLS (OpenSSL/mbedTLS), DNS resolution, URL parsing |
| **xcrypto** | SHA-1, SHA-256, MD5, CRC-32, HMAC with pluggable crypto backends |
| **xhttp** | HTTP/1.1 + HTTP/2 server and HTTP/1.1 client, WebSocket (RFC 6455), SSE |
| **xp2p** | WebRTC stack: ICE (RFC 5245), STUN (RFC 5389), TURN (RFC 5766), DTLS, SCTP, DataChannel |

## Quick Start

### Prerequisites

- CMake >= 3.16
- GTest (for tests)
- On macOS: `brew install googletest llhttp nghttp2 libusrsctp`

```bash
# Build
cmake -B build && cmake --build build

# Run tests
cd build && ctest
```

### Linking

```cmake
# All modules at once
target_link_libraries(myapp x)

# Or pick individual modules
target_link_libraries(myapp xbase xhttp)
```

## CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `X_BUILD_STATIC` | OFF | Build static libraries |
| `X_BUILD_TESTS` | ON | Build tests |
| `X_BUILD_BENCHMARKS` | OFF | Build benchmarks |
| `X_TLS_BACKEND` | auto | TLS backend: `auto`, `openssl`, `mbedtls`, `none` |
| `X_DEBUG_LEVEL` | 0 | Debug log verbosity (0-3) |

## License

MIT
