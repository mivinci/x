## Context

The root `CMakeLists.txt` was extracted from a larger `tvkproxy` project where it was a subdirectory. It relied on the parent project's `cmake/Functions.cmake` for `x_source_group` and `x_add_benchmark` helper macros, and on the parent's `project()` directive. Now that libx is a standalone repo, it needs these locally.

## Goals / Non-Goals

**Goals:**
- Make `cmake -B build` succeed on a clean checkout
- Provide `x_source_group` and `x_add_benchmark` as local CMake modules
- Remove all "tvkproxy" references from CMake comments

**Non-Goals:**
- Changing any library source code
- Changing any sub-module CMakeLists.txt files (they already use these macros correctly)
- Adding or removing build options

## Decisions

### 1. Create `cmake/Functions.cmake` and `include()` from root

The root `CMakeLists.txt` will `include(cmake/Functions.cmake)` before `add_subdirectory()`. Because CMake function scope is global to the file that includes them and all subdirectories, all module CMakeLists.txt files will have access to `x_source_group` and `x_add_benchmark`.

**Alternative considered**: Inlining the functions in the root CMakeLists.txt. Rejected — a separate file is cleaner and matches the structure used by the sibling projects.

### 2. CMake minimum version: 3.16

Set `cmake_minimum_required(VERSION 3.16)` — this is widely available (Ubuntu 20.04+, macOS system CMake) and supports all features used (`add_compile_definitions`, `add_link_options`, `FetchContent`).

### 3. `x_add_benchmark` vs Google Benchmark dependency

The current `x_add_benchmark` macro links against `GBenchmark::benchmark_main`. The `x/base/CMakeLists.txt` benchmark targets use this macro. No `find_package(benchmark)` is currently present — benchmarks require `-DX_BUILD_BENCHMARKS=ON` and GBenchmark installed. This is existing behavior and not changed here.

## Risks / Trade-offs

- **Missing GTest on first build**: cmake configure will warn if GTest is not found. This is existing behavior — developers need `brew install googletest` or equivalent.
- **No `GITHUB_MIRROR` env var**: The `x_github_url` helper function in `Functions.cmake` is not currently used by any module CMakeLists.txt, but is included for completeness since it exists in the sibling project. Harmless to include.
