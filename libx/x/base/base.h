/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * base.h - Base definitions and macros
 */

#ifndef XBASE_BASE_H
#define XBASE_BASE_H

/* ═══════════════════════════════════════════════════════════════════════════
 *  ABI & symbol-visibility macros
 *
 *  Three macros govern every public/private/inline API declaration in libx
 *  and libdlproxy:
 *
 *    XCAPI(T)         — public API. Exported in shared builds.
 *    XCAPI_LOCAL(T)   — private API. C-linkage (called across TUs within the
 *                       library) but hidden from the dynamic symbol table.
 *    XCAPI_INLINE(T)  — inline helpers. Emitted into the consumer's TU,
 *                       never imported across a DLL boundary.
 *
 *  Each macro carries three responsibilities:
 *    1. C linkage        — `extern "C"` in C++, no-op in C.
 *    2. extern storage   — prevents tentative definitions for variables
 *                          (required under `-fno-common`, default since GCC 10).
 *    3. Visibility/export — `X_EXPORT` (public), `X_LOCAL` (hidden), or empty
 *                          (inline / static builds).
 *
 *  When to use each:
 *    - Public header (libx/x/ headers, libdlproxy/ headers):
 *        XCAPI(xFoo) xFooCreate(void);
 *    - Private header (*_private.h), function called from other TUs:
 *        XCAPI_LOCAL(int) xFooImpl_(void);
 *    - Private header, file-local function:
 *        static int xFooLocal_(void);   // do NOT use XCAPI_LOCAL
 *    - Inline function in any header:
 *        XCAPI_INLINE(int) xFooFoo(int x) { return x + 1; }
 *
 *  Build flags that drive the expansion:
 *    X_BUILD_SHARED    — CMake option. When ON, enables visibility control
 *                        (POSIX `-fvisibility=hidden`) and Windows
 *                        dllexport/dllimport machinery. Default OFF to
 *                        preserve byte-identical behavior with existing
 *                        static builds.
 *    X_BUILDING_LIB    — Defined PRIVATE on libx/libdlproxy targets while
 *                        the library itself is being compiled. Switches
 *                        X_EXPORT from `dllimport` to `dllexport` on
 *                        Windows. MUST NOT propagate to consumers (CMake
 *                        PRIVATE scope) — otherwise consumers would emit
 *                        dllexport and fail to link.
 *
 *  See: openspec/changes/establish-abi-export-conventions/
 * ═══════════════════════════════════════════════════════════════════════════
 */

/* ── X_EXPORT / X_LOCAL ──────────────────────────────────────────────────
 * Platform-specific export attribute. Selected per (platform, build mode):
 *
 *   Windows + X_BUILD_SHARED + X_BUILDING_LIB  → dllexport  (library exports)
 *   Windows + X_BUILD_SHARED (consumer)        → dllimport  (consumer imports)
 *   Windows + static                          → empty      (static link)
 *   GCC/Clang + X_BUILD_SHARED                → visibility("default")
 *   GCC/Clang + static                        → empty
 *
 * On Windows, dllimport on the consumer side lets the compiler call through
 * the Import Address Table directly (small perf win) and is required for
 * data symbols. On GCC/Clang, visibility("default") overrides the
 * -fvisibility=hidden default so the symbol becomes public.
 */
#if defined(_WIN32)
  #if defined(X_BUILD_SHARED)
    #if defined(X_BUILDING_LIB)
      /* Building the library itself: export symbols. */
      #define X_EXPORT __declspec(dllexport)
    #else
      /* Consuming the library: import symbols through the IAT. */
      #define X_EXPORT __declspec(dllimport)
    #endif
  #else
    /* Static build (Windows default): no export markers needed. */
    #define X_EXPORT
  #endif
  /* Windows has no "hidden" attribute — non-exported symbols simply don't
   * carry dllexport. X_LOCAL is a no-op here. */
  #define X_LOCAL
#elif defined(__GNUC__) || defined(__clang__)
  #if defined(X_BUILD_SHARED)
    /* Public symbol: override -fvisibility=hidden default. */
    #define X_EXPORT __attribute__((visibility("default")))
    /* Hidden symbol: explicitly keep out of the dynamic symbol table. */
    #define X_LOCAL  __attribute__((visibility("hidden")))
  #else
    /* Static build (Unix default): no export markers needed. */
    #define X_EXPORT
    #define X_LOCAL
  #endif
#else
  /* Unknown compiler: fall back to no markers. */
  #define X_EXPORT
  #define X_LOCAL
#endif

/* ── XCAPI(T) — public API ───────────────────────────────────────────────
 * Declares a public, exported, C-linkage symbol of type T.
 *
 * For function declarations, T is the return type.
 * For variable declarations, the macro always adds `extern`, so the
 * declaration does NOT become a tentative definition (required to
 * compile cleanly under GCC 10+ which defaults to `-fno-common`, and
 * to avoid multiple-definition errors when the header is included by
 * several translation units).
 *
 * Expansion:
 *   C++:  extern "C" X_EXPORT T          (e.g. extern "C" __declspec(dllimport) int xFoo())
 *   C:    extern X_EXPORT T
 *
 * Usage (variable):   XCAPI(const xFoo) gFoo;        // public global
 * Usage (function):   XCAPI(int)       xFooBar(void); // public function
 *
 * See also: XCAPI_LOCAL (private), XCAPI_INLINE (inline).
 */
#ifdef __cplusplus
#define XCAPI(T) extern "C" X_EXPORT T
#else
#define XCAPI(T) extern X_EXPORT T
#endif

/* ── XCAPI_LOCAL(T) — private internal API ───────────────────────────────
 * Declares a C-linkage symbol used across multiple TUs within the library
 * itself, but NOT part of the public ABI. When X_BUILD_SHARED=ON, the
 * symbol is hidden from the dynamic symbol table (POSIX) or simply not
 * exported (Windows).
 *
 * Use this for functions declared in `*_private.h` that are called from
 * other .c files in the same module. Do NOT use it for:
 *   - Public API           — use XCAPI
 *   - File-local functions — use `static` (file scope is already private)
 *   - Inline functions     — use XCAPI_INLINE
 *
 * Expansion:
 *   C++:  extern "C" X_LOCAL T
 *   C:    extern X_LOCAL T
 *
 * Usage: XCAPI_LOCAL(int) xFooImpl_(void);  // in foo_private.h
 */
#ifdef __cplusplus
#define XCAPI_LOCAL(T) extern "C" X_LOCAL T
#else
#define XCAPI_LOCAL(T) extern X_LOCAL T
#endif

/* ── XCAPI_INLINE(T) — inline C-linkage function ─────────────────────────
 * Declares an inline function with C linkage. X_EXPORT is deliberately
 * omitted: inline functions are emitted into the consumer's translation
 * unit and never imported across a DLL boundary, so dllexport/dllimport
 * would be meaningless (and dllimport on an inline function triggers
 * -Winline warnings and can break linking on Windows).
 *
 * Expansion:
 *   C++:  extern "C" inline T
 *   C:    static inline T
 *
 * Usage: XCAPI_INLINE(int) xFooFoo(int x) { return x + 1; }
 */
#ifdef __cplusplus
#define XCAPI_INLINE(T) extern "C" inline T
#else
#define XCAPI_INLINE(T) static inline T
#endif

#define XDEF_STRUCT(T) \
  typedef struct T T;  \
  struct T

#define XDEF_ENUM(T) \
  typedef int T;     \
  enum

#define XDEF_HANDLE(T)          typedef void *T
#define XDEF_HANDLE_EXPLICIT(T) typedef struct T *T

#include <stddef.h>

/**
 * @brief Obtain a pointer to the enclosing struct from a pointer to a member.
 * @param ptr    Pointer to the member field.
 * @param type   Type of the enclosing struct.
 * @param member Name of the member field inside @p type.
 */
#define xContainerOf(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

#ifndef __cplusplus
#ifdef __STDC_VERSION__ /* C99 and later */
#include <stdbool.h>
#elif !defined(bool)
#define bool  _Bool
#define true  1
#define false 0
#endif
#endif

#endif // XBASE_BASE_H
