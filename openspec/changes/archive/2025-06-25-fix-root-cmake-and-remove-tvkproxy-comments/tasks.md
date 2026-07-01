## 1. CMake Infrastructure

- [x] 1.1 Create `cmake/Functions.cmake` with `x_source_group`, `x_add_benchmark`, and `x_github_url` helper macros
- [x] 1.2 Add `cmake_minimum_required(VERSION 3.16)` and `project(libx C)` to root `CMakeLists.txt`
- [x] 1.3 Add `include(cmake/Functions.cmake)` to root `CMakeLists.txt` before `add_subdirectory()` calls
- [x] 1.4 Replace "tvkproxy" references in root `CMakeLists.txt` comments with "libx"

## 2. Verification

- [x] 2.1 Run `cmake -B build` and confirm no errors or warnings
- [x] 2.2 Run `cmake --build build` and confirm compilation succeeds
