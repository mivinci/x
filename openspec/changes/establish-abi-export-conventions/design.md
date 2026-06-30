## Context

libx exposes its API through ~50 public headers in `libx/x/**.h` plus a handful of private headers (`*_private.h`). libdlproxy mirrors the pattern with `libdlproxy/dlproxy.h`. The library is C99 at the core, but tests and some consumers compile as C++ — so every public declaration must carry C linkage.

Two mechanisms coexist today:

- **`XCAPI(T)` macro** (defined in `libx/x/base/base.h:26-36`): expands to `extern "C" T` in C++ and `extern T` in C. Adds `extern` so variable declarations don't become tentative definitions (required under `-fno-common` since GCC 10). Used by ~50 headers.
- **`extern "C" { ... }` block** wrapping the whole header: used by 6 headers (`thread.h`, `test_helper.h`, `server_test_helper.h`, `hash_private.h`, `dns_private.h`, `dns_private.h`).

Neither mechanism controls symbol visibility. Every non-static symbol is exported by default on every platform. Windows dynamic-library builds are unhandled — no `dllexport`/`dllimport`, no `X_BUILDING_LIB` flag, no way to distinguish "I am the library" from "I am consuming the library" at compile time.

Constraints:

- ~50 headers already use `XCAPI` — changing the macro definition must stay backward compatible.
- Default build mode is static library on Windows, shared on Unix (per `CODEBUDDY.md`).
- No public API/ABI breakage is acceptable in the default build mode.
- Tests compile as C++ and include both public and private headers.

## Goals / Non-Goals

**Goals:**
- Single convention for declaring public, private, and inline API surface across libx and libdlproxy.
- Explicit symbol export on POSIX (`visibility("default")`) and Windows (`dllexport`/`dllimport`).
- Private symbols hidden from the dynamic symbol table when `X_BUILD_SHARED=ON`.
- Windows dynamic-library support that works without per-call-site `#ifdef`s.
- Backward compatibility: `X_BUILD_SHARED=OFF` (default) produces identical behavior to today.
- CI guard preventing future internal symbols from leaking into the export table.

**Non-Goals:**
- Physical separation of public vs. private headers (e.g. moving public headers to `include/x/`). Visibility alone is enough; consumers who include `*_private.h` can compile but won't link against `XCAPI_LOCAL` symbols.
- Symbol versioning (`.symver` / `__asm__(".symver ...")`). Could be added later, not in scope.
- Changing the default build mode to shared on Windows.
- C++ name mangling for public APIs — public API stays C-linkage only.
- Per-module `X_BUILDING_XBASE` / `X_BUILDING_XHTTP` flags. A single global `X_BUILDING_LIB` is sufficient because libx and libdlproxy are built as one CMake project.

## Decisions

### D1: Upgrade `XCAPI` rather than retire it

**Choice:** Keep and extend `XCAPI`. Do not migrate to `extern "C" { ... }` blocks.

**Rationale:** `XCAPI` already carries three responsibilities: C linkage, `extern` storage class (avoids tentative definition for variables under `-fno-common`), and type-wrapper sugar. A block-style `extern "C" { }` cannot carry `extern` storage for variables, cannot cleanly host `inline` functions, and cannot host visibility attributes (which must be per-symbol). Adding visibility control on top of `XCAPI` is a one-macro extension; adding it on top of block-wrapped headers requires a second parallel mechanism.

**Alternatives considered:**
- *Retire `XCAPI`, use `extern "C" { ... }` blocks + per-symbol `X_EXPORT`.* Rejected: variables still need explicit `extern` (easy to forget, regresses `-fno-common` safety), inline functions need a carve-out from the block, and visibility is still per-symbol. Two mechanisms end up coexisting.
- *Retire `XCAPI`, use pure `__attribute__((visibility))` / `__declspec` per symbol without C-linkage macros.* Rejected: loses the C++/C portability switch and forces every public header to duplicate the `#ifdef __cplusplus extern "C"` dance.

### D2: New `XCAPI_LOCAL(T)` for private-but-C-linkage symbols

**Choice:** Introduce `XCAPI_LOCAL(T)` = `extern "C" X_LOCAL T` (C++), `extern X_LOCAL T` (C). `X_LOCAL` is `__attribute__((visibility("hidden")))` on GCC/Clang with `X_BUILD_SHARED=ON`, empty otherwise.

**Rationale:** Private headers like `server_private.h` declare functions consumed by other TUs in the same library (e.g. `xHttpConnFlushWriteInternal`). These need C linkage (tests compile as C++ and call them) but must not appear in the dynamic symbol table. `static` would be wrong (prevents cross-TU linking). `XCAPI_LOCAL` gives the right combination: C name, `extern` storage, hidden visibility.

**Alternatives considered:**
- *Use `static` for internal functions.* Rejected: `static` functions can't be called from other TUs; many internal functions in libx are defined in one `.c` file and declared in a shared private header.
- *Don't mark private functions at all, rely on `-fvisibility=hidden` default.* Rejected: public functions then need an explicit `X_EXPORT` to be visible, which inverts the default and breaks backward compatibility for `X_BUILD_SHARED=OFF`. We want `XCAPI` to remain the default-export path.

### D3: `XCAPI_INLINE(T)` without `X_EXPORT`

**Choice:** `XCAPI_INLINE(T)` expands to `extern "C" inline T` (C++) and `static inline T` (C). No `X_EXPORT`.

**Rationale:** Inline functions are emitted into the consumer's translation unit, not imported from a DLL. Adding `__declspec(dllexport)` to an inline function is legal but meaningless (the consumer won't inline through a DLL boundary anyway), and on Windows `dllimport` on inline functions triggers `-Winline` warnings and can produce link errors if the function is not also defined in the importing TU. Keeping `XCAPI_INLINE` free of export markers is the cleanest cross-platform behavior.

**Alternatives considered:**
- *`XCAPI_INLINE(T) = extern "C" inline X_EXPORT T`.* Rejected: no practical benefit on POSIX (inline functions don't show up in `.dynsym` anyway) and actively harmful on Windows.

### D4: Single global `X_BUILDING_LIB` flag

**Choice:** One global `X_BUILDING_LIB` macro, defined as a CMake `PRIVATE` compile definition on every libx / libdlproxy target. Consumers never define it.

**Rationale:** libx and libdlproxy are built together in one CMake project. A single flag correctly models "am I building the library right now" for all headers. Per-module flags (`X_BUILDING_XBASE`, `X_BUILDING_XHTTP`) would complicate the build graph for no gain — there is no scenario where libx is built but libdlproxy consumes libx's headers through the `X_BUILDING_LIB` path; libdlproxy always consumes libx as an external client.

**Alternatives considered:**
- *Per-module building flags.* Rejected: adds N compile definitions for zero behavior change.
- *Per-library `X_BUILDING_XLIBNAME` with `target_compile_definitions(... PRIVATE X_BUILDING_XLIBNAME)`.* Rejected: same as above with more ceremony.

### D5: Default `X_BUILD_SHARED=OFF`

**Choice:** The new `X_BUILD_SHARED` CMake option defaults to OFF. Existing `X_BUILD_STATIC` is orthogonal and unaffected.

**Rationale:** `CODEBUDDY.md` documents static as the Windows default; existing CI builds static; existing developers build dynamic on Unix by accident of the CMake default rather than by intent. Defaulting `X_BUILD_SHARED=OFF` means the new visibility machinery is inert by default — zero behavior change for current users. Opting into `X_BUILD_SHARED=ON` enables the visibility boundary and the Windows `dllexport`/`dllimport` machinery.

**Alternatives considered:**
- *Default `X_BUILD_SHARED=ON` to force the visibility boundary on.* Rejected: breaks Windows static build default and risks surfacing latent internal-symbol dependencies across the codebase in the same commit as the refactor.

### D6: `-fvisibility=hidden` only when `X_BUILD_SHARED=ON`

**Choice:** Add `-fvisibility=hidden` to `CMAKE_C_FLAGS` / `CMAKE_CXX_FLAGS` only when `X_BUILD_SHARED=ON` and the compiler is GCC/Clang.

**Rationale:** With `-fvisibility=hidden`, the default visibility becomes hidden, and only `X_EXPORT`-marked symbols become public. This is the standard mechanism for producing a clean ELF/Mach-O symbol table. Without it, `X_LOCAL` is a no-op attribute and nothing is actually hidden.

## Risks / Trade-offs

- **[Private function called from another TU but not marked `XCAPI_LOCAL`]** → With `X_BUILD_SHARED=ON`, the symbol is still exported by accident. Not a regression from today (today everything is exported), but the "clean symbol table" guarantee isn't held until all private functions are audited. **Mitigation**: Phase 5 adds a CI check that runs `nm libx.so | grep " T "` and compares against a generated allowlist of `XCAPI`-marked symbols. The audit is incremental: missing `XCAPI_LOCAL` is a leak, not a breakage.

- **[Inline function calls a `XCAPI_LOCAL` function]** → On Windows with `X_BUILD_SHARED=ON`, an inline function in a public header that calls a hidden symbol will fail to link for consumers. **Mitigation**: Audit public inline functions before turning on `X_BUILD_SHARED=ON` in CI. If any do this, either inline the callee or mark the caller `XCAPI` (non-inline).

- **[Consumer defines `X_BUILDING_LIB` accidentally]** → They'll get `dllexport` instead of `dllimport` on Windows, causing linker errors. **Mitigation**: Document that `X_BUILDING_LIB` is for library build only; CMake sets it `PRIVATE` so it can't leak to consumers via `target_link_libraries`.

- **[Migration churn in the 6 block-style headers]** → Mechanical conversion of `thread.h` and friends is large but straightforward. **Mitigation**: Phase 2 is a pure refactor with no behavior change — easy to review, easy to bisect.

- **[Existing CI doesn't exercise `X_BUILD_SHARED=ON`]** → The new visibility boundary could rot. **Mitigation**: Phase 5 adds a CI lane that builds with `X_BUILD_SHARED=ON` and runs the symbol-table check.

## Migration Plan

Five phases, each independently shippable:

1. **Phase 1 — Macro scaffolding.** Extend `base.h` with `X_EXPORT`, `X_LOCAL`, `XCAPI_LOCAL`. When `X_BUILD_SHARED` is undefined (the default), these expand to empty — zero behavior change. All existing code continues to compile and link identically.
2. **Phase 2 — Style unification.** Convert the 6 block-style headers to `XCAPI` / `XCAPI_LOCAL` / `XCAPI_INLINE`. Pure refactor, no behavior change at any build setting.
3. **Phase 3 — Private symbol annotation.** Scan private headers (`*_private.h`), mark internal functions `XCAPI_LOCAL`. Public API unchanged.
4. **Phase 4 — CMake + Windows.** Add `X_BUILD_SHARED` option, wire `-fvisibility=hidden` and `X_BUILDING_LIB` PRIVATE definition. Verify `x_build_shared=on` produces a clean `.so`/`.dylib` and a working Windows `.dll` (manual verification on Windows; no CI lane yet).
5. **Phase 5 — CI guard.** Add a CI lane with `X_BUILD_SHARED=ON` that runs `nm`/`dumpbin` symbol-table check against an allowlist generated from `XCAPI`-marked declarations.

Rollback: revert the CMake changes in Phase 4 — `X_BUILD_SHARED=OFF` restores current behavior. Phases 1-3 are inert without Phase 4 and can stay even on rollback.

**Documentation requirement for implementation**: Every macro definition in `base.h` (Phase 1) and every CMake block touched in Phase 4 MUST carry an explanatory comment. Macros should explain *what* each branch does and *why* that branch exists (e.g. why `dllexport` vs `dllimport`, why `visibility("default")` vs `visibility("hidden")`, why static build leaves them empty, why `X_BUILDING_LIB` is `PRIVATE`). CMake blocks should explain *why* each flag/option is gated the way it is. The bar is "a new contributor reading just the comment can understand the reasoning without reading the OpenSpec change".

## Open Questions

- Should `libdlproxy` gain its own `X_BUILDING_DLPROXY` in addition to `X_BUILDING_LIB`? Current design says no (single global flag), but if libdlproxy is ever extracted into a separate CMake project this needs revisiting.
- Should the CI symbol-table allowlist be generated from a header scan (fragile to macros) or maintained manually (drifts)? Punt to Phase 5.

## Findings discovered during implementation

### Cross-module private symbols cannot use `XCAPI_LOCAL`

**Finding:** `libx/x/net/transport_private.h` declares `xTransportPlainInit`, `xTransportTlsServerInit`, `xTransportTlsClientInit`. These are consumed by `xhttp/server.c` and `xhttp/ws_connect.c` (cross-module). When libx is built as multiple separate shared libraries (one per module, the current CMake setup), `XCAPI_LOCAL`'s `visibility("hidden")` makes the symbol invisible outside `libxnet.dylib`, so `libxhttp.dylib` can't link against it.

**Resolution:** Cross-module private symbols are declared `XCAPI` (exported), not `XCAPI_LOCAL`. Their "privacy" is enforced by convention (external users shouldn't include `*_private.h`) rather than by visibility. This applies to `transport_private.h`. The header comment is updated to explain this.

**General rule:** If a private-header function is called from a different module's `.c` file, it MUST be `XCAPI` (not `XCAPI_LOCAL`). `XCAPI_LOCAL` is only for intra-module private symbols.

**Future architecture option:** If libx is ever built as a single amalgamated shared library (all modules in one `.so`/`.dylib`), `XCAPI_LOCAL` would work for cross-module private symbols because they'd be in the same linking unit. That's a CMake change (build a single `libx` target instead of per-module targets) and is out of scope for this change.
