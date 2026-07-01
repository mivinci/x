## ADDED Requirements

### Requirement: CI builds and tests on push and PR

The project SHALL have a GitHub Actions workflow at `.github/workflows/ci.yml` that builds and tests the full project on every push and pull request targeting the `main` branch, filtered to C/C++ source and build file changes.

#### Scenario: Workflow triggers on push to main

- **WHEN** a commit is pushed to the `main` branch modifying `.c`, `.h`, `.cpp`, `CMakeLists.txt`, or `.cmake` files
- **THEN** the CI workflow runs automatically

#### Scenario: Workflow triggers on PR

- **WHEN** a pull request is opened or updated targeting `main` with C/C++ or build file changes
- **THEN** the CI workflow runs automatically

### Requirement: OS and TLS backend matrix

The CI workflow SHALL test the project on a matrix of two operating systems (ubuntu-latest, macos-latest) and two TLS backends (openssl, mbedtls), for a total of 4 jobs.

#### Scenario: All matrix combinations build and test

- **WHEN** CI runs on ubuntu-latest with openssl backend
- **THEN** `cmake -B build -DX_TLS_BACKEND=openssl` configures successfully and all `ctest` tests pass

#### Scenario: mbedTLS combinations build and test

- **WHEN** CI runs on ubuntu-latest with mbedtls backend
- **THEN** `cmake -B build -DX_TLS_BACKEND=mbedtls` configures successfully and all `ctest` tests pass

### Requirement: ASan enabled on all CI jobs

All CI jobs SHALL build and test with AddressSanitizer and LeakSanitizer enabled, using a suppression file for known third-party false positives.

#### Scenario: ASan detects memory errors

- **WHEN** a CI build compiles with ASan flags and runs ctest
- **THEN** memory errors in library code cause test failures, while suppressed third-party leaks (OpenSSL, libcurl) do not

### Requirement: Simplified test scripts

The project SHALL provide `scripts/test-linux.sh` and `scripts/test-mac.sh` that each perform a trivial full build + test run without module change detection.

#### Scenario: test-linux.sh builds and tests everything

- **WHEN** `scripts/test-linux.sh -t openssl -j 4 -B build` is executed
- **THEN** CMake configures with the given TLS backend, the full project builds, and all ctest tests run

#### Scenario: test-mac.sh builds and tests everything

- **WHEN** `scripts/test-mac.sh -t mbedtls -j 4 -B build` is executed
- **THEN** CMake configures with the given TLS backend, the full project builds, and all ctest tests run

### Requirement: Build cache

The CI workflow SHALL use `actions/cache@v4` to cache the build directory, keyed on OS, TLS backend, and CMakeLists.txt file hash.

#### Scenario: Cache hit avoids recompilation

- **WHEN** CI runs twice without source changes
- **THEN** the second run uses cached build artifacts and completes faster than a full rebuild
