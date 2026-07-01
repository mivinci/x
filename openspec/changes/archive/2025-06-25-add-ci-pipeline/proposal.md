## Why

The project currently has no CI pipeline. Every build and test must be run manually, and there is no automated verification that code works across macOS, Linux, OpenSSL, and mbedTLS. Without CI, regressions can slip through unnoticed.

## What Changes

- Add `.github/workflows/ci.yml` — GitHub Actions workflow with 4-job matrix (ubuntu/macos × openssl/mbedtls), ASan enabled, full build + test every push/PR
- Add `scripts/test-linux.sh` — Linux build + test script (~60 lines, simplified — no change detection)
- Add `scripts/test-mac.sh` — macOS build + test script (~60 lines)
- Add `scripts/lsan_suppressions.txt` — LeakSanitizer suppressions for OpenSSL/libcurl false positives
- Add FetchContent fallback to `cmake/FindLlhttp.cmake` — on Ubuntu where llhttp has no apt package, fall back to FetchContent

## Capabilities

### New Capabilities
- `ci-pipeline`: Automated build and test on every push and PR, covering macOS/Linux and OpenSSL/mbedTLS with ASan
- `llhttp-fetchcontent`: `cmake/FindLlhttp.cmake` falls back to FetchContent when llhttp is not installed system-wide

### Modified Capabilities
<!-- None — existing module behavior unchanged -->

## Impact

- New files: `.github/workflows/ci.yml`, `scripts/test-linux.sh`, `scripts/test-mac.sh`, `scripts/lsan_suppressions.txt`
- Modified: `cmake/FindLlhttp.cmake`
- No API or library behavior changes
- CI will trigger on push/PR to `main`, filtering for `.c`/`.h`/`.cpp`/`CMakeLists.txt`/`.cmake` changes
