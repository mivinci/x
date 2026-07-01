## Why

The current flat layout (`x/`, `bench/` at repo root) doesn't scale well. When `xpp` (C++ wrapper) and future tools (e.g., CLI executables) are added, the root becomes cluttered. Grouping library code under `libx/` follows the curl project pattern and makes room for peer directories.

## What Changes

- Move `x/` → `libx/` (all library source, CMakeLists.txt, headers, tests)
- Move `bench/` → `libx/bench/` (benchmarks live with the library)
- Update root `CMakeLists.txt`: `add_subdirectory(x/...)` → `add_subdirectory(libx/...)`
- Update `.gitignore`: `build` → `build`, `build-*`
- Update CI: `ci.yml` paths from `x/` → `libx/`, test scripts paths
- Update docs: cross-reference paths from `x/` → `libx/`
- Update `CODEBUDDY.md`: all path references

## Capabilities

### New Capabilities
<!-- None — this is a structural refactor, no new functionality -->

### Modified Capabilities
<!-- None — API and behavior unchanged -->

## Impact

- **BREAKING**: All `#include <x/...>` paths unchanged (the `include_directories()` in CMake is already set to `${CMAKE_CURRENT_SOURCE_DIR}`, so headers are still found as `<x/base/event.h>` regardless of the directory rename)
- Every file referencing `x/` or `bench/` as source paths (CMakeLists.txt, CI workflows, test scripts, docs, CODEBUDDY.md)
- After move: root directory contains only `libx/`, `docs/`, `cmake/`, `scripts/`, `.github/`, and top-level config files
