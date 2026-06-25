# CODEBUDDY.md

This file provides guidance to CodeBuddy Code when working with code in this repository.

## Build System

CMake (C) + optional Go (benchmarks only).

```
# Configure and build (shared libraries, tests enabled by default)
cmake -B build && cmake --build build

# Build with benchmarks
cmake -B build -DX_BUILD_BENCHMARKS=ON && cmake --build build

# Static libraries
cmake -B build -DX_BUILD_STATIC=ON

# Disable tests
cmake -B build -DX_BUILD_TESTS=OFF
```

**Key CMake options:**

| Option | Default | Description |
|--------|---------|-------------|
| `X_BUILD_STATIC` | OFF | Build static (vs shared) libraries |
| `X_BUILD_TESTS` | ON | Build GTest-based test targets |
| `X_BUILD_BENCHMARKS` | OFF | Build benchmark executables |
| `X_TLS_BACKEND` | "auto" | TLS: `auto`, `openssl`, `mbedtls`, `none` |
| `X_DEBUG_LEVEL` | 0 | Debug logging verbosity (0-3) |

**External dependencies:** GTest (required for tests), libcurl, llhttp, nghttp2. Optional: OpenSSL/mbedTLS (TLS), zlib (WebSocket permessage-deflate), libunwind (backtrace), usrsctp (P2P SCTP).

## Running Tests

Tests use Google Test with CTest integration. Test targets are `{module}_test` (e.g., `xbase_test`, `xhttp_test`).

```
# Run all tests
cd build && ctest

# Run a specific module's tests
./build/x/http/xhttp_test

# Filter by test name
cd build && ctest -R xhttp
./build/x/base/xbase_test --gtest_filter=EventLoop*
```

Test files use C++ (`.cpp`) with `extern "C" { ... }` blocks to include C headers. No `main()` function — GTest provides the entry point.

## Running Benchmarks

E2E benchmarks: `./bench/run_bench.sh [tcp|http|all]` (requires `-DX_BUILD_BENCHMARKS=ON` build and `wrk` for HTTP).

Micro-benchmarks: build each module's benchmark target individually, e.g. `./build/x/base/buf_bench`.

Go-based WebSocket benchmark servers: `cd bench && go run ./http/ws_bench_server_gorilla.go`.

## Code Style

- C sources (`.c`/`.h`), C++ test files (`.cpp`)
- clang-format style: LLVM-based with 2-space indent, 100-col limit, `PointerAlignment: Right`, `BreakBeforeBraces: Attach`, sorted includes with `IncludeBlocks: Preserve`
- All public symbols use `x` prefix: `xHttpServerCreate`, `xBufferCreate`, `xTcpConnect`
- Public API functions use `XCAPI(T)` macro for C linkage
- Opaque handles use `XDEF_HANDLE(xFoo)` macro (typedef to `void*`)
- Return types use `xErrno` from `<x/base/error.h>`, with `xErrno_Ok` for success

## Architecture

This is a C foundation library (MIT-licensed, project internal name "moo"/"tvkproxy"). Seven modules arranged in a dependency stack, each a separate CMake target:

### Module Dependency Graph

```
xp2p (WebRTC: ICE/STUN/TURN/DTLS/SCTP/DataChannel)
 ├─ xcrypto (SHA-1/256, MD5, CRC-32, HMAC)
 │    └─ xbase
 ├─ xnet (TCP, TLS, DNS, transport abstraction)
 │    ├─ xbase
 │    └─ xbuf
 └─ xbase

xhttp (HTTP/1.1 + HTTP/2 server, HTTP/1.1 client, WebSocket, SSE)
 ├─ xbase
 ├─ xbuf
 └─ xnet

xlog (async logger)
 └─ xbase

xbuf (xBuffer, xRingBuffer, xIOBuffer)
 └─ xbase

xbase (event loop, memory, data structures, encoding, threading, time, IO abstractions)
```

### Module Summaries

- **x/base**: Foundation. Event loop (kqueue/epoll/poll), slab allocator, intrusive linked lists, hash/tree/flat maps, binary heap, MPSC queue, hex/base64/base58 encoding, `xErrno` error type, `xReader`/`xWriter` IO abstractions, command-line flag parsing, subprocess execution (POSIX PTY), monotonic clock, thread helpers.

- **x/log**: Async logger with file rotation. Three modes: timer-based periodic flush, notify (pipe-based immediate), mixed (timer + urgent notify). Thread-local context via `xLoggerEnter/Leave`. Convenience macros: `XLOG_INFO`, `XLOG_ERROR`, etc.

- **x/buf**: Three buffer types — `xBuffer` (linear auto-growing, 2x realloc), `xRingBuffer` (fixed-size power-of-2, suitable for embedded/pipeline), `xIOBuffer` (8KB block-chained, reference-counted, zero-copy split/append, lock-free block pool, scatter-gather via `writev`). See `x/buf/README.md` for a selection guide (Chinese).

- **x/net**: TCP (`xTcpConn`, `xTcpConnect`, `xTcpListener` — all async), TLS abstraction (`xTlsConf`/`xTlsCtx` with OpenSSL/mbedTLS backends), transport vtable, async DNS resolution, URL parsing.

- **x/crypto**: Hash functions with pluggable backends (auto-detected from `X_TLS_BACKEND`). Pure-C built-in fallback for SHA-1, SHA-256 when no TLS library is available. MD5 and CRC-32 are always built-in. Also HMAC-SHA1/SHA256/MD5.

- **x/http**: The largest module. HTTP server uses a protocol handler vtable (`xHttpProto`) to support both HTTP/1.1 (llhttp) and HTTP/2 (nghttp2) on the same port with auto-detection. Features routing with path parameters, response streaming, WebSocket (RFC 6455 with permessage-deflate), SSE client, and deferred responses. HTTP client uses libcurl multi-socket API (async, HTTP/2, HTTPS). See `x/http/TODO.md` for planned HTTP/3 (QUIC via ngtcp2+nghttp3) roadmap and current HTTP/2 implementation status.

- **x/p2p**: Full WebRTC stack in C. `xPeerConnection` mirrors the browser RTCPeerConnection API. Includes ICE agent (RFC 5245), STUN (RFC 5389), TURN client (RFC 5766), DTLS (OpenSSL/mbedTLS), SCTP via usrsctp, DataChannel (RFC 8831), SDP parsing/generation, and NAT type probing.

### Namespace and Include Convention

All public headers are included as `<x/{module}/{header}.h>`:

```c
#include <x/base/event.h>        // Event loop
#include <x/base/list.h>         // Intrusive linked lists
#include <x/base/buffer.h>       // Note: xbase's internal buffer (not xbuf)
#include <x/buf/buf.h>           // xBuffer
#include <x/buf/io.h>            // xIOBuffer
#include <x/net/tcp.h>           // TCP
#include <x/net/tls.h>           // TLS config
#include <x/crypto/sha256.h>     // SHA-256
#include <x/http/server.h>       // HTTP server
#include <x/http/ws.h>           // WebSocket
#include <x/log/logger.h>        // Async logging
#include <x/p2p/peer_connection.h>
```

### No Application Entry Points

This is a library, not an application. There are no `main()` functions in the library code. All executables are test runners (`{module}_test`) or benchmark binaries.

### Benchmark Structure

- `bench/` — E2E benchmarks with comparison Go servers
- Per-module micro-benchmarks (e.g., `buf_bench`, `map_bench`, `event_bench`)
- Go module `moo-bench` in `bench/go.mod` provides WebSocket baseline servers (gorilla, gobwas, nhooyr) for comparative benchmarking
