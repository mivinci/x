cmake_minimum_required(VERSION 3.16)
project(libx C CXX)

# libx — C foundation libraries
#
# Self-contained: includes its own test programs and owns
# all library-build toggles under the X_* namespace. Can be consumed as
# a sub-project; the parent project sees only libx application toggles.

# ── CMake helpers ──────────────────────────────────────────────────────
list(APPEND CMAKE_PREFIX_PATH /opt/homebrew)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include_directories("${CMAKE_CURRENT_SOURCE_DIR}")
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/libx")
include(cmake/Functions.cmake)

# ── Library-build toggles ─────────────────────────────────────────────
option(X_BUILD_STATIC     "Build libx as static libraries"           OFF)
option(X_BUILD_TESTS      "Build libx tests"                          ON)

# X_BUILD_SHARED: When ON, enables symbol-visibility control (POSIX
# -fvisibility=hidden) and Windows dllexport/dllimport machinery.
# Default OFF to preserve byte-identical behavior with existing static
# builds — turning ON requires all private symbols to be annotated
# XCAPI_LOCAL (or kept as XCAPI if called from tests or other modules).
# See: openspec/changes/establish-abi-export-conventions/
option(X_BUILD_SHARED     "Build with explicit symbol export (shared-lib ABI)" OFF)

set(X_TLS_BACKEND "auto" CACHE STRING "TLS backend: auto, openssl, mbedtls, none")
set(X_DEBUG_LEVEL 0 CACHE STRING "Debug logging level (0-3, higher means more verbose)")

if(X_BUILD_STATIC)
  set(X_LIB_TYPE STATIC)
else()
  set(X_LIB_TYPE SHARED)
endif()

# Pass debug level to libx C/C++ sources
add_compile_definitions(X_DEBUG_LEVEL=${X_DEBUG_LEVEL})

# When X_BUILD_SHARED is ON:
#   1. Define X_BUILD_SHARED so headers see it (controls X_EXPORT/X_LOCAL
#      macro expansions in base.h).
#   2. Define X_BUILDING_LIB so headers switch X_EXPORT from dllimport
#      to dllexport on Windows. On POSIX this is a no-op (visibility is
#      controlled by -fvisibility=hidden + visibility attributes, not by
#      X_BUILDING_LIB). Defined globally — on POSIX this is harmless.
#      NOTE: On Windows with shared builds, X_BUILDING_LIB must only be
#      defined on library targets (PRIVATE), not on test executables.
#      Windows shared-build support is deferred until Windows CI is set up.
#   3. Add -fvisibility=hidden so the default visibility is hidden; only
#      XCAPI-marked symbols (which carry visibility("default")) become
#      public. Without this flag, X_LOCAL/visibility("hidden") is a no-op
#      and all symbols are exported — defeating the purpose of the
#      XCAPI/XCAPI_LOCAL distinction.
if(X_BUILD_SHARED)
  add_compile_definitions(X_BUILD_SHARED X_BUILDING_LIB)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fvisibility=hidden)
    message(STATUS "X_BUILD_SHARED=ON: symbol visibility control enabled (-fvisibility=hidden)")
  endif()
endif()

if(X_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  # Promote IMPORTED targets to GLOBAL so sibling subdirs can
  # link against them too.
  foreach(_tgt GTest::gtest GTest::gtest_main)
    if(TARGET ${_tgt})
      get_target_property(_aliased ${_tgt} ALIASED_TARGET)
      if(_aliased)
        set(_real ${_aliased})
      else()
        set(_real ${_tgt})
      endif()
      get_target_property(_is_global ${_real} IMPORTED_GLOBAL)
      if(NOT _is_global)
        set_target_properties(${_real} PROPERTIES IMPORTED_GLOBAL TRUE)
      endif()
    endif()
  endforeach()
endif()

# ── External dependencies ─────────────────────────────────────────────
find_package(Libcurl REQUIRED)
find_package(benchmark QUIET)

# Promote IMPORTED targets to GLOBAL so sibling subdirs can link against them.
if(benchmark_FOUND)
  foreach(_tgt benchmark::benchmark benchmark::benchmark_main)
    if(TARGET ${_tgt})
      get_target_property(_aliased ${_tgt} ALIASED_TARGET)
      if(_aliased)
        set(_real ${_aliased})
      else()
        set(_real ${_tgt})
      endif()
      get_target_property(_is_global ${_real} IMPORTED_GLOBAL)
      if(NOT _is_global)
        set_target_properties(${_real} PROPERTIES IMPORTED_GLOBAL TRUE)
      endif()
    endif()
  endforeach()
endif()
find_package(benchmark QUIET)

# Filter non-existent paths from Libcurl::Libcurl's include directories.
# Homebrew's curl cmake config may reference an Xcode SDK path that
# doesn't exist on the current runner (different Xcode version).
if(TARGET Libcurl::Libcurl)
  get_target_property(_curl_inc Libcurl::Libcurl INTERFACE_INCLUDE_DIRECTORIES)
  if(_curl_inc)
    set(_filtered "")
    foreach(_dir ${_curl_inc})
      if(EXISTS "${_dir}")
        list(APPEND _filtered "${_dir}")
      endif()
    endforeach()
    set_target_properties(Libcurl::Libcurl PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${_filtered}")
  endif()
endif()

# ── Modules ───────────────────────────────────────────────────────────
add_subdirectory(libx)
add_subdirectory(libxpp)
add_subdirectory(libdlproxy)
