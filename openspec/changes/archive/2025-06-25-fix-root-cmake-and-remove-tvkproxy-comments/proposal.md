## Why

The root `CMakeLists.txt` is incomplete — it's missing `project()` and `cmake_minimum_required()` directives, and references a `x_source_group` helper function that was defined externally in the parent `tvkproxy` project's `cmake/Functions.cmake`. Running `cmake -B build` immediately fails with "Unknown CMake command" errors. Additionally, comments in the root CMakeLists.txt still reference "tvkproxy" from the old project name.

## What Changes

- Add `cmake_minimum_required()` and `project()` directives to the root `CMakeLists.txt`
- Create `cmake/Functions.cmake` with the `x_source_group` and `x_add_benchmark` helper macros that all sub-module CMakeLists.txt files depend on
- Include `cmake/Functions.cmake` from the root `CMakeLists.txt`
- Replace all "tvkproxy" references in CMakeLists.txt comments with "libx"

## Capabilities

### New Capabilities
- `cmake-functions-module`: A `cmake/Functions.cmake` file providing `x_source_group` (organizes sources in IDE folder trees) and `x_add_benchmark` (convenience macro for creating benchmark executables) helper macros, shared across all module CMakeLists.txt files.

### Modified Capabilities
<!-- None — this change fixes build infrastructure, not library behavior -->

## Impact

- Root `CMakeLists.txt`: Restructured with proper CMake boilerplate, updated comments, added `include(cmake/Functions.cmake)`
- New file: `cmake/Functions.cmake` (extracted from sibling `mivinci/cmake/Functions.cmake`)
- No API changes, no library behavior changes, no dependency changes
