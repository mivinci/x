## ADDED Requirements

### Requirement: Library source under libx/

All library source code (C headers, C sources, tests) and benchmarks SHALL reside under the `libx/` directory at the repository root.

#### Scenario: Library modules are under libx/x/

- **WHEN** the repository is cloned
- **THEN** `libx/x/base/`, `libx/x/http/`, etc. contain the library source files

#### Scenario: Benchmarks are under libx/bench/

- **WHEN** benchmarks are built with `-DX_BUILD_BENCHMARKS=ON`
- **THEN** the benchmark source files are found at `libx/bench/`

### Requirement: Include paths unchanged

The `#include <x/base/event.h>` include paths SHALL remain valid after the directory restructure, without any changes to source files.

#### Scenario: Source files compile without changes

- **WHEN** `cmake --build build` is run after the restructure
- **THEN** all source files compile without include path errors

### Requirement: Root CMakeLists.txt updated

The root `CMakeLists.txt` SHALL reference the new `libx/` paths for all `add_subdirectory()` calls.

#### Scenario: cmake configure succeeds

- **WHEN** `cmake -B build` is run after the restructure
- **THEN** cmake configures without errors, finding all subdirectories at their new locations

### Requirement: CI and scripts updated

All CI workflows, test scripts, and documentation SHALL reference the new `libx/` paths.

#### Scenario: test-mac.sh runs successfully

- **WHEN** `scripts/test-mac.sh -t openssl` is run after the restructure
- **THEN** the script builds and tests the library without path errors
