# FindLlhttp.cmake
# ----------------
# Find llhttp library (HTTP/1.1 parser).
#
# This module defines:
#   Llhttp_FOUND        - True if the library was found or fetched
#   Llhttp::Llhttp      - Imported target for linking
#   Llhttp_INCLUDE_DIRS - Include directories
#   Llhttp_LIBRARIES    - Libraries to link against

include_guard(GLOBAL)

# Try pkg-config first
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PC_LLHTTP QUIET libllhttp)
endif()

find_path(Llhttp_INCLUDE_DIRS
  NAMES llhttp.h
  HINTS
    ${PC_LLHTTP_INCLUDE_DIRS}
    /opt/homebrew/opt/llhttp/include
    /usr/local/opt/llhttp/include
    /usr/local/include
    /usr/include
)

find_library(Llhttp_LIBRARIES
  NAMES llhttp
  HINTS
    ${PC_LLHTTP_LIBRARY_DIRS}
    /opt/homebrew/opt/llhttp/lib
    /usr/local/opt/llhttp/lib
    /usr/local/lib
    /usr/lib
)

if(Llhttp_INCLUDE_DIRS AND Llhttp_LIBRARIES)
  set(Llhttp_FOUND TRUE)

  if(NOT TARGET Llhttp::Llhttp)
    add_library(Llhttp::Llhttp UNKNOWN IMPORTED)
    set_target_properties(Llhttp::Llhttp PROPERTIES
      IMPORTED_LOCATION "${Llhttp_LIBRARIES}"
      INTERFACE_INCLUDE_DIRECTORIES "${Llhttp_INCLUDE_DIRS}"
    )
  endif()
else()
  include(FetchContent)
  FetchContent_Declare(llhttp
    GIT_REPOSITORY https://github.com/nodejs/llhttp
    GIT_TAG        release/v9.2.0
  )
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(llhttp)

  if(TARGET llhttp_static)
    add_library(Llhttp::Llhttp ALIAS llhttp_static)
  elseif(TARGET llhttp)
    add_library(Llhttp::Llhttp ALIAS llhttp)
  else()
    message(FATAL_ERROR "llhttp FetchContent did not produce expected targets")
  endif()

  set(Llhttp_FOUND TRUE)
endif()

mark_as_advanced(Llhttp_INCLUDE_DIRS Llhttp_LIBRARIES)
