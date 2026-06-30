## ADDED Requirements

### Requirement: Single linkage macro convention for public API
All public function and variable declarations in libx and libdlproxy headers SHALL use the `XCAPI(T)` macro. Headers SHALL NOT wrap declarations in `extern "C" { ... }` blocks.

#### Scenario: New public function declaration
- **WHEN** a new public function `xFooBar` is declared in any `libx/x/**/*.h` or `libdlproxy/*.h`
- **THEN** the declaration MUST be written as `XCAPI(return-type) xFooBar(params);`
- **AND** the header MUST NOT use an `extern "C" { ... }` block to establish C linkage

#### Scenario: Public variable declaration
- **WHEN** a new public variable `gFoo` is declared in any header
- **THEN** the declaration MUST be written as `XCAPI(const xFoo) gFoo;` or `XCAPI(xFoo) gFoo;`
- **AND** the macro MUST expand to include `extern` so the declaration is not a tentative definition

#### Scenario: Existing block-style header is converted
- **WHEN** the 6 currently-block-style headers (`thread.h`, `test_helper.h`, `server_test_helper.h`, `hash_private.h`, `dns_private.h`, `dns_private.h`) are touched
- **THEN** all `extern "C" { ... }` blocks MUST be removed
- **AND** every previously-block-wrapped function/variable declaration MUST be rewritten with `XCAPI(T)` or `XCAPI_LOCAL(T)`

### Requirement: `XCAPI` macro provides C linkage, extern storage, and export marking
The `XCAPI(T)` macro in `libx/x/base/base.h` SHALL expand to a declaration that carries C linkage, `extern` storage class, and the platform-appropriate export attribute. In C++ the expansion SHALL be `extern "C" X_EXPORT T`. In C the expansion SHALL be `extern X_EXPORT T`.

#### Scenario: Macro expansion in C++ translation unit
- **WHEN** a C++ translation unit includes `base.h`
- **AND** `X_BUILD_SHARED` is undefined
- **THEN** `XCAPI(int) xFoo(void);` MUST expand to `extern "C" int xFoo(void);`
- **AND** `XCAPI(const xFoo) gFoo;` MUST expand to `extern "C" const xFoo gFoo;`

#### Scenario: Macro expansion in C translation unit
- **WHEN** a C translation unit includes `base.h`
- **AND** `X_BUILD_SHARED` is undefined
- **THEN** `XCAPI(int) xFoo(void);` MUST expand to `extern int xFoo(void);`
- **AND** `XCAPI(const xFoo) gFoo;` MUST expand to `extern const xFoo gFoo;`

#### Scenario: Macro expansion with shared library on GCC/Clang
- **WHEN** `X_BUILD_SHARED` is defined
- **AND** the compiler is GCC or Clang
- **THEN** `X_EXPORT` MUST expand to `__attribute__((visibility("default")))`
- **AND** `XCAPI(int) xFoo(void);` in C++ MUST expand to `extern "C" __attribute__((visibility("default"))) int xFoo(void);`

#### Scenario: Macro expansion on Windows when building the library
- **WHEN** the platform is Windows (`_WIN32` defined)
- **AND** both `X_BUILD_SHARED` and `X_BUILDING_LIB` are defined
- **THEN** `X_EXPORT` MUST expand to `__declspec(dllexport)`

#### Scenario: Macro expansion on Windows when consuming the library
- **WHEN** the platform is Windows (`_WIN32` defined)
- **AND** `X_BUILD_SHARED` is defined
- **AND** `X_BUILDING_LIB` is NOT defined
- **THEN** `X_EXPORT` MUST expand to `__declspec(dllimport)`

#### Scenario: Static build on Windows
- **WHEN** the platform is Windows (`_WIN32` defined)
- **AND** `X_BUILD_SHARED` is NOT defined
- **THEN** `X_EXPORT` MUST expand to empty
- **AND** `XCAPI(int) xFoo(void);` MUST expand to `extern "C" int xFoo(void);` in C++

### Requirement: `XCAPI_LOCAL(T)` macro for private C-linkage symbols
A new `XCAPI_LOCAL(T)` macro SHALL be defined in `base.h`. It SHALL provide C linkage and `extern` storage class, but apply the platform-appropriate hidden-visibility attribute. It SHALL be used for functions and variables declared in private headers (`*_private.h`) that need to be called from multiple TUs within the library but MUST NOT appear in the dynamic symbol table.

#### Scenario: Macro expansion in C++ with shared library
- **WHEN** `X_BUILD_SHARED` is defined and the compiler is GCC/Clang
- **THEN** `XCAPI_LOCAL(int) xFooImpl(void);` MUST expand to `extern "C" __attribute__((visibility("hidden"))) int xFooImpl(void);`

#### Scenario: Macro expansion when building static
- **WHEN** `X_BUILD_SHARED` is NOT defined
- **THEN** `XCAPI_LOCAL(T)` MUST expand identically to `XCAPI(T)` minus the export marker (i.e. `extern "C" T` in C++, `extern T` in C)
- **AND** the symbol MUST be visible in the symbol table (no `visibility("hidden")` attribute applied)

#### Scenario: Private function in shared library is hidden
- **WHEN** a function is declared with `XCAPI_LOCAL(T)` in a private header
- **AND** the library is built with `X_BUILD_SHARED=ON` on GCC/Clang
- **THEN** the resulting `.so`/`.dylib` MUST NOT export that symbol
- **AND** `nm libx.so | grep " T <symbol>"` MUST return no entries

### Requirement: `XCAPI_INLINE(T)` macro for inline C-linkage functions
The `XCAPI_INLINE(T)` macro SHALL expand to `extern "C" inline T` in C++ and `static inline T` in C. It SHALL NOT apply `X_EXPORT` regardless of build mode.

#### Scenario: Macro expansion in C++
- **WHEN** a C++ translation unit includes `base.h`
- **THEN** `XCAPI_INLINE(int) xQuux(void) { ... }` MUST expand to `extern "C" inline int xQuux(void) { ... }`

#### Scenario: Macro expansion in C
- **WHEN** a C translation unit includes `base.h`
- **THEN** `XCAPI_INLINE(int) xQuux(void) { ... }` MUST expand to `static inline int xQuux(void) { ... }`

#### Scenario: No export marker on inline functions
- **WHEN** `X_BUILD_SHARED` is defined
- **AND** a function is declared with `XCAPI_INLINE(T)`
- **THEN** the expansion MUST NOT contain `__declspec(dllexport)`, `__declspec(dllimport)`, or `__attribute__((visibility("default")))`

### Requirement: `X_BUILD_SHARED` CMake option
A CMake option `X_BUILD_SHARED` SHALL be added to the root `CMakeLists.txt`. Its default SHALL be `OFF`. When `ON`, the build system SHALL compile all libx and libdlproxy targets with `-fvisibility=hidden` (GCC/Clang) and define `X_BUILDING_LIB` as a `PRIVATE` compile definition on those targets.

#### Scenario: Default build (static)
- **WHEN** `cmake -B build` is invoked without `-DX_BUILD_SHARED=ON`
- **THEN** `X_BUILD_SHARED` MUST be `OFF`
- **AND** no `-fvisibility=hidden` flag MUST be added
- **AND** `X_BUILDING_LIB` MUST NOT be defined
- **AND** all existing `XCAPI` declarations MUST produce identical object code to before this change

#### Scenario: Shared library build on Linux/macOS
- **WHEN** `cmake -B build -DX_BUILD_SHARED=ON` is invoked on Linux or macOS
- **THEN** CMake MUST add `-fvisibility=hidden` to `CMAKE_C_FLAGS` and `CMAKE_CXX_FLAGS`
- **AND** every libx / libdlproxy target MUST have `X_BUILDING_LIB` defined as `PRIVATE`
- **AND** the resulting `.so`/`.dylib` MUST export only `XCAPI`-marked symbols

#### Scenario: Shared library build on Windows
- **WHEN** `cmake -B build -DX_BUILD_SHARED=ON` is invoked on Windows (`_WIN32`)
- **THEN** every libx / libdlproxy target MUST have `X_BUILDING_LIB` defined as `PRIVATE`
- **AND** the resulting `.dll` MUST export `XCAPI`-marked symbols via `__declspec(dllexport)`
- **AND** consumers linking against the `.dll` MUST see `__declspec(dllimport)` on those symbols

### Requirement: `X_BUILDING_LIB` is private to library targets
The `X_BUILDING_LIB` macro SHALL be applied as a `PRIVATE` `target_compile_definitions` on libx and libdlproxy targets. It SHALL NOT propagate to consumers via `INTERFACE` or `PUBLIC` definitions.

#### Scenario: Consumer of libx does not see X_BUILDING_LIB
- **WHEN** an external project links against libx via `target_link_libraries(consumer PRIVATE x)`
- **AND** `X_BUILD_SHARED=ON`
- **THEN** the consumer's compile commands MUST NOT contain `-DX_BUILDING_LIB`
- **AND** the consumer's `XCAPI` expansions MUST use `__declspec(dllimport)` (Windows) or `visibility("default")` (GCC/Clang)

### Requirement: Existing XCAPI usage remains source-compatible
All existing declarations written as `XCAPI(T) name;` in libx and libdlproxy SHALL continue to compile and link without modification when `X_BUILD_SHARED` is `OFF`.

#### Scenario: Existing source compiled with default options
- **WHEN** the library is built with default CMake options (no `X_BUILD_SHARED`)
- **THEN** every existing `XCAPI(T)` declaration MUST expand to the same token sequence as before this change
- **AND** every existing `XCAPI_INLINE(T)` declaration MUST expand to the same token sequence as before this change
- **AND** the resulting object files and libraries MUST be byte-identical in symbol visibility to before this change

### Requirement: Private header functions are annotated `XCAPI_LOCAL`
Functions and variables declared in `*_private.h` headers that are consumed across TUs SHALL be declared with `XCAPI_LOCAL(T)` rather than `XCAPI(T)` or unannotated.

#### Scenario: Function in private header
- **WHEN** a function is declared in `libx/x/http/server_private.h` (or any other `*_private.h`)
- **AND** the function is called from at least one other `.c` file in the same module
- **THEN** the declaration MUST use `XCAPI_LOCAL(T)` rather than `XCAPI(T)` or a bare `extern` declaration

#### Scenario: Static function in private header
- **WHEN** a function in a private header is declared `static` (file-scope only)
- **THEN** it SHALL NOT be annotated with `XCAPI_LOCAL` (it is already file-local)

### Requirement: CI guard for symbol table
A CI lane SHALL build with `X_BUILD_SHARED=ON` and verify that the resulting dynamic library exports only `XCAPI`-marked symbols. Any `XCAPI_LOCAL`-marked symbol appearing in the export table is a regression.

#### Scenario: Public symbol exported
- **WHEN** a function is declared with `XCAPI(T)` in a public header
- **AND** the library is built with `X_BUILD_SHARED=ON`
- **THEN** `nm libx.so | grep " T <function-name>"` MUST return exactly one entry

#### Scenario: Private symbol leaked
- **WHEN** a function is declared with `XCAPI_LOCAL(T)` in a private header
- **AND** the function appears in `nm libx.so | grep " T "`
- **THEN** the CI check MUST fail

#### Scenario: Inline function not in symbol table
- **WHEN** a function is declared with `XCAPI_INLINE(T)`
- **THEN** the function MAY appear in the symbol table as a weak/local symbol
- **AND** the CI check MUST NOT flag it as a regression
