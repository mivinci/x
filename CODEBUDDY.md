# CODEBUDDY.md

This file provides guidance to CodeBuddy Code when working with code in this repository.

## Build & Test

```bash
# Configure and build (macOS)
cmake -B build -G Ninja && cmake --build build -j

# Run all tests
cd build && ctest --output-on-failure

# Run a single test executable
./build/libx/x/base/xbase_test
./build/libx/x/http/xhttp_test

# Run a specific test by filter
cd build && ctest -R "server_h1" --output-on-failure

# macOS test script (with ASan, TLS backend selection)
zsh scripts/test-mac.sh -t openssl -j $(sysctl -n hw.ncpu) --asan

# Linux test script
bash scripts/test-linux.sh -t openssl -j $(nproc) --asan
```

## CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `X_BUILD_STATIC` | `OFF` | Build static libraries |
| `X_BUILD_TESTS` | `ON` | Build tests |
| `X_TLS_BACKEND` | `auto` | TLS backend: `auto`, `openssl`, `mbedtls`, `none` |
| `X_DEBUG_LEVEL` | `0` | Debug log verbosity (0-3) |
| `X_WS_DEFLATE` | `ON` | WebSocket permessage-deflate (zlib) |
| `X_BUILD_BENCHMARKS` | `OFF` | Build benchmarks |

Per-module test toggles (e.g. `-DXBASE_BUILD_TESTS=OFF`) inherit from `X_BUILD_TESTS` by default.

## Dependencies (macOS)

```bash
brew install googletest llhttp nghttp2 mbedtls libusrsctp
```

## Architecture

### Module Layering (bottom-up)

```
xbase   — Core primitives: event loop, memory, strings, containers, OS abstraction
 xlog   — Async logger (depends on xbase)
 xbuf   — Buffer primitives: linear, ring, block-chain IO buffers (depends on xbase)
 xnet   — TCP/TLS/DNS/URL (depends on xbase, xbuf)
 xcrypto — MD5, SHA1, SHA256, HMAC (depends on xbase)
 xp2p    — WebRTC: ICE, STUN, TURN, DTLS, SCTP, peer connection (depends on xbase, xnet)
 xhttp  — HTTP/1.1, HTTP/2, WebSocket, SSE client & server (depends on xbase, xbuf, xnet)
```

An aggregated `x` INTERFACE target links all modules: `target_link_libraries(myapp x)`. Fine-grained linking (`xbase`, `xhttp`, etc.) is also supported.

### Core Pattern: Backend Selection

The project compiles only the backend files for detected/configured platforms. Example from event loop:

- `event.c` + `event.h` — public API (always compiled)
- `event_private.h` — internal dispatch table and state shared by all backends
- `event_kqueue.c` / `event_epoll.c` / `event_poll.c` / `event_wsapoll.c` — one selected per platform via `#ifdef X_HAS_KQUEUE` etc. in CMake

Same pattern applies to TLS (`tls_openssl.c` / `tls_mbedtls.c`), WS crypto (`ws_crypto_openssl.c` / `ws_crypto_mbedtls.c` / `ws_crypto_builtin.c`), transport (`transport_openssl.c` / `transport_mbedtls.c` / `transport_plain.c`), SHA (`sha1_openssl.c` / `sha1_mbedtls.c` / `sha1_builtin.c`), and DTLS (`dtls_openssl.c` / `dtls_mbedtls.c`).

### Code Conventions

- **Language**: C99 core library, C++ test files
- **Naming**: All public symbols prefixed with `x` (types `xFoo`, functions `xFooBar`, enums `xErrno_Ok`, macros `XDEF_STRUCT`)
- **Opaque handles**: Public types use `XDEF_HANDLE(T)` (void pointer) or `XDEF_HANDLE_EXPLICIT(T)` (forward-declared struct); internal state is hidden behind the pointer
- **Error handling**: Functions return `xErrno` (0 = success), defined in `libx/x/base/error.h`
- **C-linkage macro**: `XCAPI(T)` expands to `extern "C" T` (C++) or `extern T` (C) for all public symbols
- **Formatting**: LLVM-based, 2-space indent, 100-char column limit (see `.clang-format`)
- **clangd**: Compilation database at `build/compile_commands.json` (`.clangd` points there)

### Event Loop (`libx/x/base/event.h`)

The event loop is the heart of the library. All async I/O, timers, signals, and offloaded work are built on it. Key concepts:

- **Backends**: kqueue (macOS/BSD), epoll (Linux), poll (POSIX fallback), WSAPoll (Windows) — all edge-triggered
- **Run modes**: `X_RUN_DEFAULT` (block until stop), `X_RUN_ONCE` (one iteration), `X_RUN_NOWAIT` (non-blocking poll)
- **Thread safety**: `xEventLoopPost()` for cross-thread callbacks; `xWorkSubmit()` for offloading blocking work to a thread pool
- **Per-iteration order**: drain done queue → poll I/O → drain done queue again → fire timers → sweep deleted sources
- See `libx/x/base/EVENT.md` for detailed API reference and usage patterns

### Test Structure

- Tests use Google Test (gtest), written in C++ (`*_test.cpp` files)
- Each module has a single test executable (e.g. `xbase_test`, `xhttp_test`) that compiles all `*_test.cpp` files in its directory tree
- Tests are added to CTest by `add_test()` in each module's `CMakeLists.txt`
- CI runs with ASan (`--asan` flag in test scripts) on both Linux and macOS, with both openssl and mbedtls TLS backends

### Documentation

Docs are in `docs/`, built with mdbook. The `docs/SUMMARY.md` defines the TOC structure. Module docs mirror the source tree under `docs/libx/`.
