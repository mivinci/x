## Context

The project was extracted from a larger `moo`/`tvkproxy` monorepo which had a mature CI pipeline. libx now lives in its own repository and needs its own CI. The reference pipeline at `../moo/.github/workflows/ci.yml` + `../moo/scripts/test-*.sh` serves as a template but can be significantly simplified since libx has fewer modules and no C++ wrapper.

## Goals / Non-Goals

**Goals:**
- GitHub Actions CI with matrix: macOS + Linux × OpenSSL + mbedTLS (4 jobs)
- Full build + ctest on every push/PR touching C/C++ sources or build files
- ASan (AddressSanitizer + LeakSanitizer) always enabled
- Clean separation: `ci.yml` orchestrates, `scripts/test-*.sh` do the work
- FetchContent fallback for llhttp on Ubuntu (no apt package)

**Non-Goals:**
- Module change detection (always build everything — libx is fast enough)
- Docker/container-based local testing (CI runs natively on GitHub runners)
- Documentation deployment (mdBook)
- Code coverage collection
- Branch naming lint

## Decisions

### 1. Simplify test scripts — no change detection

Moo's `test-linux.sh` has ~200 lines of git diff + dependency graph logic. For libx (7 modules, ~300 source files), a full build on CI takes under 2 minutes. The complexity of change detection is not worth it.

Each script just: `cmake -B build -DX_TLS_BACKEND=$tls && cmake --build build -j$jobs && ctest --output-on-failure`.

### 2. ASan always on, same suppression list

Same approach as moo: `ASAN_OPTIONS=halt_on_error=0` (needed for forkpty tests), leak suppression for OpenSSL/libcurl false positives. Copy the suppression file from moo directly — the false positive patterns are identical since libx uses the same OpenSSL/libcurl versions.

### 3. FetchContent for llhttp on Ubuntu

On macOS, `brew install llhttp` provides `llhttp-config.cmake`. On Ubuntu, there's no llhttp package. Add FetchContent fallback inside `cmake/FindLlhttp.cmake`:

```cmake
find_library(Llhttp_LIBRARIES NAMES llhttp ...)
find_path(Llhttp_INCLUDE_DIRS NAMES llhttp.h ...)

if(NOT Llhttp_FOUND_OR_FETCHED)
  include(FetchContent)
  FetchContent_Declare(llhttp
    GIT_REPOSITORY https://github.com/nodejs/llhttp
    GIT_TAG release/v9.2.0
  )
  # ... build and create Llhttp::Llhttp target
endif()
```

The `x/http/CMakeLists.txt` code (`find_package(Llhttp REQUIRED)`) does not change.

### 4. Build cache with actions/cache@v4

Cache the `build/` directory keyed on OS, TLS backend, and CMakeLists.txt hash. This avoids recompiling unchanged sources across CI runs.

### 5. ci.yml triggers

```yaml
on:
  push:
    branches: [main]
    paths: ['**.c', '**.h', '**.cpp', 'CMakeLists.txt', '**.cmake', '.github/workflows/ci.yml']
  pull_request:
    branches: [main]
    paths: ['**.c', '**.h', '**.cpp', 'CMakeLists.txt', '**.cmake', '.github/workflows/ci.yml']
```

## Risks / Trade-offs

- **llhttp FetchContent adds network dependency** — first CI run on clean cache will download llhttp. Sub-minute download, acceptable.
- **No mbedTLS testing on Ubuntu** — `libmbedtls-dev` is available in apt. On macOS, `brew install mbedtls`. Both work.
- **Cache misses on CMakeLists.txt changes** — intentional, ensures rebuild after build system changes.
