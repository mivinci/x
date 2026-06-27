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

set(X_TLS_BACKEND "auto" CACHE STRING "TLS backend: auto, openssl, mbedtls, none")
set(X_DEBUG_LEVEL 0 CACHE STRING "Debug logging level (0-3, higher means more verbose)")

if(X_BUILD_STATIC)
  set(X_LIB_TYPE STATIC)
else()
  set(X_LIB_TYPE SHARED)
endif()

# Pass debug level to libx C/C++ sources
add_compile_definitions(X_DEBUG_LEVEL=${X_DEBUG_LEVEL})

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

# ── Modules ───────────────────────────────────────────────────────────
add_subdirectory(libx)
