## Why

libx and libdlproxy headers mix two styles for C/C++ ABI linkage: the `XCAPI(T)` macro (per-symbol, 50+ headers) and `extern "C" { ... }` blocks (6 headers). Neither style controls symbol visibility — every non-static symbol is exported by default, bloating dynamic symbol tables, exposing internal implementation details as implicit ABI, and leaving Windows `dllimport`/`dllexport` completely unhandled. As the library grows and Windows dynamic-library support becomes relevant, this becomes a recurring source of linkage surprises and an obstacle to producing a clean public ABI.

## What Changes

- **Single linkage style**: Retire `extern "C" { ... }` blocks in the 6 headers that use them (`thread.h`, `test_helper.h`, `server_test_helper.h`, `hash_private.h`, `dns_private.h`, `dns_private.h`); convert all declarations to `XCAPI(T)` / `XCAPI_LOCAL(T)` / `XCAPI_INLINE(T)`.
- **Visibility-aware `XCAPI` macros**: Upgrade `base.h` to define `X_EXPORT` / `X_LOCAL` for GCC/Clang (`visibility("default")` / `visibility("hidden")`) and Windows (`__declspec(dllexport)` / `__declspec(dllimport)` based on `X_BUILDING_LIB`). `XCAPI(T)` becomes `extern "C" X_EXPORT T` in C++ and `extern X_EXPORT T` in C.
- **New `XCAPI_LOCAL(T)` macro**: For internal functions/variables that still need C linkage but should not appear in the dynamic symbol table. Applied to private-header declarations (e.g. `xHttpConnFlushWriteInternal`).
- **`X_BUILD_SHARED` CMake option**: When ON, compiles with `-fvisibility=hidden` on GCC/Clang so only `XCAPI`-marked symbols are exported. Default OFF to preserve current static-library behavior on Windows and match the documented default.
- **Windows dynamic-library support**: `X_BUILDING_LIB` is defined as a PRIVATE compile definition on libx/libdlproxy targets when built shared, so the same headers produce `dllexport` during library build and `dllimport` for consumers.
- **`XCAPI_INLINE(T)` unchanged in spirit**: Stays `extern "C" inline T` (C++) / `static inline T` (C), without `X_EXPORT` — inline functions are emitted into the consumer's TU and do not need cross-DLL export.
- **Single global `X_BUILDING_LIB`**: Defined while compiling libx or libdlproxy themselves, undefined for consumers. Avoids per-module build flags. **BREAKING** for anyone who currently relies on the implicit default export of every internal symbol (no public API breakage; only internal symbol visibility changes, and only when `X_BUILD_SHARED=ON`).

## Capabilities

### New Capabilities
- `abi-export-conventions`: A single convention for declaring C/C++ public/private/inline API surface across libx and libdlproxy — macro design, visibility rules, CMake options, Windows `dllimport`/`dllexport` handling, and the migration of existing headers.

### Modified Capabilities
<!-- None — no existing spec in openspec/specs/ describes ABI or export behavior today. -->

## Impact

- **Headers** (`libx/x/**/*.h`, `libdlproxy/*.h`): ~50+ headers already using `XCAPI` keep working unchanged. 6 block-style headers are converted. Private headers gain `XCAPI_LOCAL` on internal functions.
- **`libx/x/base/base.h`**: Macro definitions extended with `X_EXPORT`, `X_LOCAL`, `XCAPI_LOCAL`. Backward-compatible when `X_BUILD_SHARED` is OFF (macros expand to empty).
- **`CMakeLists.txt`** (root and per-module): Add `X_BUILD_SHARED` option, wire `-fvisibility=hidden` and `X_BUILDING_LIB` PRIVATE definition on library targets.
- **`CODEBUDDY.md`**: Document the new `X_BUILD_SHARED` option and the meaning of `XCAPI` / `XCAPI_LOCAL` / `XCAPI_INLINE`.
- **No source-level API breakage**: Existing user code using `XCAPI(int) xFooBar(...);` continues to compile and link unchanged.
- **ABI**: When `X_BUILD_SHARED=OFF` (default), no behavior change. When `X_BUILD_SHARED=ON`, the dynamic symbol table shrinks to only `XCAPI`-marked symbols — a new constraint that could surface previously-implicit dependencies (e.g. consumers calling `xHttpConnFlushWriteInternal` directly would fail to link).
- **CI**: Existing 4-config matrix (Linux/macOS × openssl/mbedtls) continues to pass with `X_BUILD_SHARED=OFF`. A new CI lane with `X_BUILD_SHARED=ON` should be added to guard the visibility boundary.
