# FindLlhttp.cmake
# ----------------
# Find llhttp library (HTTP/1.1 parser).
#
# This module defines:
#   Llhttp_FOUND        - True if the library was found
#   Llhttp::Llhttp      - Imported target for linking
#   Llhttp_INCLUDE_DIRS - Include directories
#   Llhttp_LIBRARIES    - Libraries to link against

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

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Llhttp
  REQUIRED_VARS Llhttp_LIBRARIES Llhttp_INCLUDE_DIRS
  VERSION_VAR PC_LLHTTP_VERSION
)

if(Llhttp_FOUND AND NOT TARGET Llhttp::Llhttp)
  add_library(Llhttp::Llhttp UNKNOWN IMPORTED)
  set_target_properties(Llhttp::Llhttp PROPERTIES
    IMPORTED_LOCATION "${Llhttp_LIBRARIES}"
    INTERFACE_INCLUDE_DIRECTORIES "${Llhttp_INCLUDE_DIRS}"
  )
endif()

mark_as_advanced(Llhttp_INCLUDE_DIRS Llhttp_LIBRARIES)
