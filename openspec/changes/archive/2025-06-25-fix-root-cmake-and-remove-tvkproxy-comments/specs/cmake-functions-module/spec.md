## ADDED Requirements

### Requirement: cmake-functions-module

The project SHALL provide a `cmake/Functions.cmake` file containing reusable CMake function and macro definitions that are included from the root `CMakeLists.txt` and available to all sub-modules via `add_subdirectory()` scope propagation.

#### Scenario: x_source_group organizes target sources for IDE

- **WHEN** a module CMakeLists.txt calls `x_source_group(<target> "${CMAKE_CURRENT_SOURCE_DIR}")` after `add_library()`
- **THEN** the target's sources are organized in the IDE (Xcode, Visual Studio) folder tree matching their on-disk directory structure

#### Scenario: x_add_benchmark creates a benchmark executable

- **WHEN** a module CMakeLists.txt calls `x_add_benchmark(<name> SOURCES <src> LIBS <libs>)`
- **THEN** an executable target `<name>` is created linked against `<libs>` and `GBenchmark::benchmark_main`

### Requirement: root-cmake-standalone

The root `CMakeLists.txt` SHALL be a standalone build file that configures successfully without any parent project context.

#### Scenario: cmake configure succeeds on clean checkout

- **WHEN** a developer runs `cmake -B build` in the repository root
- **THEN** cmake configures without errors (no missing `project()`, no unknown command `x_source_group`)

#### Scenario: project identity is libx

- **WHEN** cmake finishes configuration
- **THEN** the project name is reported as `libx` (not `tvkproxy` or `Project`)

#### Scenario: cmake minimum version is declared

- **WHEN** cmake processes the root CMakeLists.txt
- **THEN** no warning about a missing `cmake_minimum_required()` call is emitted
