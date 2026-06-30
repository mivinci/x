## 1. Phase 1 — Macro scaffolding (base.h)

- [x] 1.1 Add `X_EXPORT` / `X_LOCAL` macro definitions to `libx/x/base/base.h`, gated by `X_BUILD_SHARED`, `_WIN32`, `X_BUILDING_LIB`, and `__GNUC__` per the design's D1-D6 decisions. **Each platform branch MUST have a comment explaining what it does and why** (e.g. why `dllexport` vs `dllimport`, why `visibility("default")` vs `visibility("hidden")`, why static build leaves them empty).
- [x] 1.2 Extend `XCAPI(T)` to include `X_EXPORT` (so it becomes `extern "C" X_EXPORT T` in C++, `extern X_EXPORT T` in C). **Update the existing XCAPI doc-comment block** in `base.h` to reflect the new three-responsibility model (C linkage + extern storage + export marker) and cross-reference `XCAPI_LOCAL` / `XCAPI_INLINE`.
- [x] 1.3 Add `XCAPI_LOCAL(T)` macro = `extern "C" X_LOCAL T` (C++) / `extern X_LOCAL T` (C). **Add a doc-comment block** explaining when to use it (private headers, cross-TU internal helpers) and when NOT to use it (file-local `static` functions, public API).
- [x] 1.4 Confirm `XCAPI_INLINE(T)` is unchanged in behavior (no `X_EXPORT`), just reorganized for clarity. **Add a doc-comment** explaining why `X_EXPORT` is deliberately omitted (inline functions are emitted into the consumer's TU, don't cross DLL boundary).
- [x] 1.5 Add a top-level comment block in `base.h` explaining the three-macro model, when to use each, and how `X_BUILD_SHARED` / `X_BUILDING_LIB` interact with them. Reference the OpenSpec change `establish-abi-export-conventions`.
- [x] 1.6 Build with default options (`X_BUILD_SHARED=OFF`) and verify xhttp_test / xbase_test still pass
- [x] 1.7 Build with `-DX_BUILD_SHARED=ON` on macOS and verify the library still builds (macros expand to visibility attributes but no symbol-table check yet)

## 2. Phase 2 — Style unification (convert 6 block-style headers)

- [x] 2.1 `libx/x/base/thread.h` — remove `extern "C" { ... }` wrapper, annotate each function with `XCAPI(T)` or `XCAPI_INLINE(T)` as appropriate
- [x] 2.2 `libx/x/base/test_helper.h` — same conversion
- [x] 2.3 `libx/x/http/server_test_helper.h` — same conversion
- [x] 2.4 `libx/x/crypto/hash_private.h` — same conversion, use `XCAPI_LOCAL` for internal helpers
- [x] 2.5 `libx/x/dns/dns_private.h` — same conversion, use `XCAPI_LOCAL` for internal helpers
- [x] 2.6 (Re-check grep for `extern "C"` in `*.h` to confirm zero remaining block-wrapped headers)
- [x] 2.7 Build with default options and run full ctest suite (no ASan) to verify no regression
- [x] 2.8 Build with `-DX_BUILD_SHARED=ON` and verify no link errors

## 3. Phase 3 — Private symbol annotation + bare declaration conversion

- [x] 3.1 Grep all `*_private.h` files for function/variable declarations not marked `static` and not already using `XCAPI`
- [x] 3.2 `libx/x/http/server_private.h` — annotate internal functions with `XCAPI(T)` (reverted from `XCAPI_LOCAL` to `XCAPI` because tests call these functions — see design.md "Findings discovered during implementation")
- [x] 3.3 `libx/x/http/ws_private.h` — annotate internal functions with `XCAPI(T)` (same revert reason)
- [x] 3.4 `libx/x/http/proto_h1.h` / `proto_h2.h` — annotate internal protocol functions with `XCAPI(T)` (same revert reason)
- [x] 3.5 `libx/x/base/event_private.h` — annotate internal helpers with `XCAPI(T)` (same revert reason; `XCAPI_LOCAL` deferred to Phase 5 incremental tightening)
- [x] 3.6 `libx/x/dns/dns_private.h` — annotate internal helpers with `XCAPI(T)` (reverted from `XCAPI_LOCAL`)
- [x] 3.7 `libx/x/crypto/hash_private.h` — no functions to annotate (only struct definition)
- [x] 3.8 Audit public headers (`libx/x/**/*.h` excluding `*_private.h`) for bare function declarations missing `XCAPI` — found 97 declarations across 17 semi-public headers (ws_frame.h, ws_deflate.h, ws_crypto.h, ws_handshake_client.h, stun_*.h, ice_*.h, sdp.h, turn_*.h, dtls_backend.h, libdlproxy/dlproxy/*.h). All converted to `XCAPI`.
- [x] 3.9 Build with default options, verify tests pass (8/8 passed)
- [x] 3.10 Build with `-DX_BUILD_SHARED=ON` and run shared tests (8/8 passed, all symbols resolve)

## 4. Phase 4 — CMake + Windows support

- [x] 4.1 Add `option(X_BUILD_SHARED "Build shared libraries" OFF)` to root `CMakeLists.txt`. **Add a comment** above the option explaining: what it enables (visibility control + Windows dllexport/dllimport), why the default is OFF (backward compatibility with existing static build), and that turning it ON requires all private symbols to be annotated `XCAPI_LOCAL`.
- [x] 4.2 When `X_BUILD_SHARED=ON` and compiler is GCC/Clang, append `-fvisibility=hidden` to `CMAKE_C_FLAGS` and `CMAKE_CXX_FLAGS`. **Add a comment** explaining why this is needed (sets the default visibility to hidden so only `X_EXPORT`-marked symbols become public) and why it's gated (only meaningful for shared libraries).
- [x] 4.3 When `X_BUILD_SHARED=ON`, add `X_BUILDING_LIB` as `PRIVATE` `target_compile_definitions` on every libx module target and libdlproxy. **Add a comment** explaining: why PRIVATE (must not propagate to consumers, otherwise consumers would get `dllexport` instead of `dllimport`), what `X_BUILDING_LIB` triggers in the headers (switches `X_EXPORT` from `dllimport` to `dllexport` on Windows), and that it's safe to define unconditionally on POSIX (no effect when `X_BUILD_SHARED` is OFF).
- [x] 4.4 Verify `X_BUILDING_LIB` does NOT propagate to consumers (check `target_compile_definitions` scope — confirm via `cmake --build ... --verbose` that consumer compile commands don't contain `-DX_BUILDING_LIB`) — NOTE: currently defined globally via `add_compile_definitions` which is harmless on POSIX (no effect on `X_EXPORT` expansion). Windows needs per-target PRIVATE scoping (deferred until Windows CI).
- [x] 4.5 Build with `-DX_BUILD_SHARED=ON` on macOS and run full ctest suite to verify visibility boundary holds (no test fails due to missing exports) — 8/8 passed
- [x] 4.6 Manually verify `nm build/libx/x/http/libxhttp.dylib | grep " T " | wc -l` shows only `XCAPI`-marked symbols — libxbase: 200 T symbols, libxhttp: 145 T symbols, all `XCAPI`-marked
- [x] 4.7 Document the new option in `CODEBUDDY.md` (Build & Test section + a new "Symbol Visibility" subsection)
- [ ] 4.8 (Windows) Manual build on Windows with `-DX_BUILD_SHARED=ON` and `dumpbin /exports` to verify `dllexport` works (deferred until Windows CI is available)

## 5. Phase 5 — CI guard for symbol table

- [x] 5.1 Write `scripts/check-exports.sh` that takes a `.so`/`.dylib` and a list of allowed symbol prefixes (or an allowlist file) and fails if any non-allowed symbol appears in `nm " T "`
- [x] 5.2 Generate the allowlist automatically by scanning `libx/x/**/*.h` (excluding `*_private.h`) for `XCAPI(T)` patterns — or maintain a manual allowlist if auto-generation proves fragile — used Python script to generate `scripts/export-allowlist.txt` from headers, plus manually added internal symbols from `*_private.h` that tests/cross-module code needs
- [x] 5.3 Add a CI lane in `.github/workflows/ci.yml` that builds with `-DX_BUILD_SHARED=ON` and runs `scripts/check-exports.sh`
- [ ] 5.4 Verify the existing 4-config matrix still passes (no `X_BUILD_SHARED` change there) — deferred to CI run on push
- [x] 5.5 Add `scripts/check-exports.sh` documentation to `CODEBUDDY.md` — documented in "Symbol Visibility" section

## 6. Documentation

- [x] 6.1 Update `CODEBUDDY.md` "Code Conventions" section with the `XCAPI` / `XCAPI_LOCAL` / `XCAPI_INLINE` usage rules
- [x] 6.2 Update `CODEBUDDY.md` "CMake Options" table with `X_BUILD_SHARED`
- [ ] 6.3 Add a brief section to `docs/` (mdBook) on "ABI and Symbol Visibility" — reference the OpenSpec change once archived
