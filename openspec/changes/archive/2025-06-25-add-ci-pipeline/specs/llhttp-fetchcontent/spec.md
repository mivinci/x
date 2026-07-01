## ADDED Requirements

### Requirement: FetchContent fallback for llhttp

The `cmake/FindLlhttp.cmake` module SHALL fall back to CMake FetchContent when llhttp is not found as a system-installed library, so that `find_package(Llhttp REQUIRED)` succeeds on Ubuntu (which has no llhttp apt package) without modifying `x/http/CMakeLists.txt`.

#### Scenario: System llhttp found first

- **WHEN** llhttp is installed system-wide (e.g., `brew install llhttp` on macOS)
- **THEN** `find_package(Llhttp)` uses the system installation without invoking FetchContent

#### Scenario: FetchContent fallback on Ubuntu

- **WHEN** llhttp is NOT installed system-wide (e.g., clean Ubuntu runner)
- **THEN** `find_package(Llhttp)` fetches and builds llhttp via FetchContent, and creates a `Llhttp::Llhttp` target

#### Scenario: FetchContent result is cached between builds

- **WHEN** FetchContent has already fetched and built llhttp in a previous cmake configure
- **THEN** subsequent cmake configures reuse the cached build without re-downloading
