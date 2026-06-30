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

## Formatting

The repo ships a `.clang-format` config at the root. It enforces Google-style include ordering plus layout rules (2-space indent, 100-char column limit, alignment, etc.).

**Include ordering** (Google style, strict):
1. Corresponding header (`"foo.h"` in `foo.c`)
2. C system headers (`<stdio.h>`)
3. C++ system headers (`<vector>`)
4. Other library headers (`<openssl/ssl.h>`, `<gtest/gtest.h>`)
5. Project headers (`"foo_private.h"`, `<x/base/log.h>`)

Each group separated by a blank line; entries sorted alphabetically within each group. Project headers go last so missing dependencies in `<x/...>` headers surface early.

```bash
# Format all C/C++ source files
find libx libdlproxy \( -name '*.c' -o -name '*.h' -o -name '*.cpp' \) -print0 \
  | xargs -0 clang-format -i

# Check formatting without modifying (used by CI)
find libx libdlproxy \( -name '*.c' -o -name '*.h' -o -name '*.cpp' \) -print0 \
  | xargs -0 clang-format --dry-run --Werror
```

CI runs the dry-run check on every push/PR (the `clang-format` lane in `.github/workflows/ci.yml`).

### Pre-commit hook (optional)

The repo includes a `.githooks/pre-commit` script that auto-formats staged C/C++ files on every `git commit`. To enable it (run once per clone):

```bash
git config core.hooksPath .githooks
```

The hook runs `clang-format -i` on each staged `.c`/`.h`/`.cpp` file under `libx/` or `libdlproxy/`, then re-stages the result. If no formatting is needed, the commit proceeds untouched. To bypass for a single commit: `git commit --no-verify`.

## CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `X_BUILD_STATIC` | `OFF` | Build static libraries |
| `X_BUILD_SHARED` | `OFF` | Enable symbol-visibility control (POSIX `-fvisibility=hidden` + Windows `dllexport`/`dllimport`). When OFF (default), all symbols are exported. When ON, only `XCAPI`-marked symbols are public. |
| `X_BUILD_TESTS` | `ON` | Build tests |
| `X_TLS_BACKEND` | `auto` | TLS backend: `auto`, `openssl`, `mbedtls`, `none` |
| `X_DEBUG_LEVEL` | `0` | Debug log verbosity (0-3) |
| `X_WS_DEFLATE` | `ON` | WebSocket permessage-deflate (zlib) |
| `X_BUILD_BENCHMARKS` | `OFF` | Build benchmarks |

Per-module test toggles (e.g. `-DXBASE_BUILD_TESTS=OFF`) inherit from `X_BUILD_TESTS` by default.
Per-module benchmark toggles (e.g. `-DXBASE_BUILD_BENCHMARKS=ON`) inherit from `X_BUILD_BENCHMARKS`.

```bash
# Build with benchmarks (requires Google Benchmark)
cmake -B build -G Ninja -DX_BUILD_BENCHMARKS=ON && cmake --build build -j
```

## Sanitizers

ASan is available via the test scripts (`--asan` flag). LSAN suppressions for known false positives (OpenSSL TLS state, libcurl) are in `scripts/lsan_suppressions.txt`.

TSan and UBSan are not currently configured in scripts or CI.

## Symbol Visibility

When `X_BUILD_SHARED=ON`, the build system applies `-fvisibility=hidden` (GCC/Clang) so only `XCAPI`-marked symbols are exported. `XCAPI_LOCAL` symbols are hidden from the dynamic symbol table.

```bash
# Build with visibility control
cmake -B build -G Ninja -DX_BUILD_SHARED=ON && cmake --build build -j

# Verify exported symbols (should only be XCAPI-marked)
nm build/libx/x/http/libxhttp.dylib | grep " T " | wc -l
```

**Key rules:**
- All public API functions/variables: declare with `XCAPI(T)`
- Internal helpers in `*_private.h` called only within the same module: declare with `XCAPI_LOCAL(T)`
- Internal helpers called from tests or other modules: declare with `XCAPI(T)` (privacy by convention, not visibility)
- Inline functions: declare with `XCAPI_INLINE(T)` (no export marker)
- `X_BUILDING_LIB` is defined automatically when building the library; consumers never define it

See `openspec/changes/establish-abi-export-conventions/` for the full design.

## Linting (clang-tidy)

The repo ships a `.clang-tidy` config at the root. Currently enforces `google-readability-casting` (no C-style casts in C++ — use `static_cast` / `reinterpret_cast` / `const_cast` instead). Adding new checks requires fixing all existing violations first; see the comment block in `.clang-tidy` for the policy.

```bash
# Run locally (requires build/compile_commands.json)
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
./scripts/run-clang-tidy.sh -j $(nproc)

# Scan a single file
./scripts/run-clang-tidy.sh libx/x/http/ws_test.cpp
```

CI runs the same script on every push/PR (the `clang-tidy` lane in `.github/workflows/ci.yml`). On macOS with Homebrew LLVM, the script passes `--extra-arg=-stdlib=libc++` and `--sysroot` so clang-tidy can find libc++ headers.

## Dependencies (macOS)

```bash
brew install googletest llhttp nghttp2 mbedtls libusrsctp
```

## Platform Notes

**Windows**: Uses WSAPoll event backend and static library. The DNS module links `iphlpapi ws2_32`. No Windows CI is configured, but the CMake build system supports MSVC with `_WIN32_WINNT=0x0601`.

## Architecture

### Module Layering (bottom-up)

```
xbase   — Core primitives: event loop, memory (VTable/slab/MPSC), strings, containers, OS abstraction
 xlog   — Async logger (depends on xbase)
 xbuf   — Buffer primitives: linear, ring, block-chain IO buffers (depends on xbase)
 xnet   — TCP/TLS/DNS/URL (depends on xbase, xbuf)
 xcrypto — MD5, SHA1, SHA256, HMAC (depends on xbase)
 xdns    — DNS client & server, packet parsing (depends on xbase)
 xp2p    — WebRTC: ICE, STUN, TURN, DTLS, SCTP, peer connection (depends on xbase, xnet, xcrypto)
 xhttp  — HTTP/1.1 (llhttp), HTTP/2 (nghttp2), WebSocket, SSE client & server (depends on xbase, xbuf, xnet)
 xfs    — Filesystem operations (depends on xbase)
```

An aggregated `x` INTERFACE target links all modules: `target_link_libraries(myapp x)`. Fine-grained linking (`xbase`, `xhttp`, etc.) is also supported.

**External library**: `libdlproxy` — VOD download proxy (depends on xhttp, xfs, xbase).

### Core Pattern: Backend Selection

The project compiles only the backend files for detected/configured platforms. Example from event loop:

- `event.c` + `event.h` — public API (always compiled)
- `event_private.h` — internal dispatch table and state shared by all backends
- `event_kqueue.c` / `event_epoll.c` / `event_poll.c` / `event_wsapoll.c` — one selected per platform via `#ifdef X_HAS_KQUEUE` etc. in CMake

Same pattern applies to TLS (`tls_openssl.c` / `tls_mbedtls.c`), WS crypto (`ws_crypto_openssl.c` / `ws_crypto_mbedtls.c` / `ws_crypto_builtin.c`), transport (`transport_openssl.c` / `transport_mbedtls.c` / `transport_plain.c`), SHA (`sha1_openssl.c` / `sha1_mbedtls.c` / `sha1_builtin.c`), and DTLS (`dtls_openssl.c` / `dtls_mbedtls.c`). WebSocket permessage-deflate (`ws_deflate.c`) is conditionally compiled when zlib is available (`X_WS_DEFLATE=ON`).

### xlog (`libx/x/log/logger.h`)

- **Three flush modes**: `xLogMode_Timer` (periodic, default 100ms), `xLogMode_Notify` (pipe-based immediate), `xLogMode_Mixed` (timer + pipe for high-severity)
- **Five levels**: Debug, Info, Warn, Error, Fatal — Fatal triggers abort after sync write
- **Thread-local context**: `xLoggerEnter/Leave/Current()` with convenience macros `XLOG_DEBUG()`, `XLOG_INFO()`, `XLOG_WARN()`, `XLOG_ERROR()`, `XLOG_FATAL()`
- **File rotation**: Automatic by `max_size` and `max_files`
- **Async queue**: Lock-free MPSC queue for enqueue; event loop drains and writes to disk

### Memory Management (`libx/x/base/`)

- **VTable-based allocation** (`memory.h`): `xAlloc/xFree` with ctor/dtor lifecycle via `XDEF_VTABLE(T)`, `XDEF_CTOR(T)`, `XDEF_DTOR(T)` macros. Supports `xRetain/xRelease` reference counting and `xCopy/xMove`.
- **Slab allocator** (`slab.h`): Fixed-size object pool. `xSlab` (single-thread), `xSlabMt` (multi-thread, lock-free Treiber stack freelist). `xSlabReset()` for bulk reclaim without freeing chunks.
- **MPSC queue** (`mpsc.h`): Multi-producer, single-consumer lock-free queue — used by the event loop's done queue and xlog.
- **xString** (`string.h`): Dynamic string based on SDS pattern.

### Code Conventions

- **Language**: C99 core library, C++ test files
- **Naming**: All public symbols prefixed with `x` (types `xFoo`, functions `xFooBar`, enums `xErrno_Ok`, macros `XDEF_STRUCT`)
- **Opaque handles**: Public types use `XDEF_HANDLE(T)` (void pointer) or `XDEF_HANDLE_EXPLICIT(T)` (forward-declared struct); internal state is hidden behind the pointer
- **Error handling**: Functions return `xErrno` (0 = success), defined in `libx/x/base/error.h`
- **C-linkage macros** (defined in `libx/x/base/base.h`):
  - `XCAPI(T)` — public API. C linkage + `extern` storage + export marker. Use for all public function and variable declarations.
  - `XCAPI_LOCAL(T)` — private API. C linkage + `extern` storage + hidden visibility. Use for internal helpers in `*_private.h` that are called across TUs but should not appear in the dynamic symbol table. **Do NOT use** for functions called from tests or other modules (those must be `XCAPI`).
  - `XCAPI_INLINE(T)` — inline function. C linkage + `inline`, no export marker. Use for header-defined inline functions.
  - When `X_BUILD_SHARED=OFF` (default), all three expand identically to pre-visibility behavior (no export markers).
- **Formatting**: LLVM-based, 2-space indent, 100-char column limit (see `.clang-format`)
- **clangd**: Compilation database at `build/compile_commands.json` (`.clangd` points there)

### Event Loop (`libx/x/base/event.h`)

The event loop is the heart of the library. All async I/O, timers, signals, and offloaded work are built on it. Key concepts:

- **Backends**: kqueue (macOS/BSD), epoll (Linux), poll (POSIX fallback), WSAPoll (Windows) — all edge-triggered
- **Run modes**: `X_RUN_DEFAULT` (block until stop), `X_RUN_ONCE` (one iteration), `X_RUN_NOWAIT` (non-blocking poll)
- **Thread safety**: `xEventLoopPost()` for cross-thread callbacks; `xWorkSubmit()` for offloading blocking work to a thread pool
- **Per-iteration order**: drain done queue → poll I/O → drain done queue again → fire timers → sweep deleted sources
- See `libx/x/base/EVENT.md` for detailed API reference and usage patterns

### Benchmarks

Benchmarks use [Google Benchmark](https://github.com/google/benchmark). Build with `-DX_BUILD_BENCHMARKS=ON`.

```bash
# Build and run benchmarks
cmake -B build -G Ninja -DX_BUILD_BENCHMARKS=ON && cmake --build build -j

# Micro-benchmarks
./build/libx/x/base/event_bench
./build/libx/x/base/slab_bench
./build/libx/x/base/mpsc_bench

# End-to-end TCP/HTTP benchmarks (requires wrk for HTTP)
cd libx/bench && bash run_bench.sh
```

Benchmark targets: event loop, memory/slab/map/heap/MPSC/task (xbase), buffers (xbuf), TCP echo, DNS, HTTP/WS server (libx/bench).

### Test Structure

- Tests use Google Test (gtest), written in C++ (`*_test.cpp` files)
- Each module has a single test executable (e.g. `xbase_test`, `xhttp_test`) that compiles all `*_test.cpp` files in its directory tree
- Tests are added to CTest by `add_test()` in each module's `CMakeLists.txt`
- CI runs with ASan (`--asan` flag in test scripts) on both Linux and macOS, with both openssl and mbedtls TLS backends

### CI

GitHub Actions runs a 4-config matrix (Linux/macOS x openssl/mbedtls) with ASan on every push/PR to `main` (`.github/workflows/ci.yml`). Docs are deployed to GitHub Pages on pushes to docs (`.github/workflows/docs.yml`).

```bash
# Run CI locally in Docker
bash .container/run-ci.sh
```

### Documentation

Docs are in `docs/`, built with [mdBook](https://rust-lang.github.io/mdBook/) and mermaid diagrams. Covers ~27 pages across all modules: event loop, memory, slab, strings, DNS, TCP/TLS, HTTP client/server, WebSocket, SSE, ICE, peer connection, and more. See `docs/SUMMARY.md` for the full TOC.

```bash
# Build and serve docs locally
cd docs && mdbook serve
```
