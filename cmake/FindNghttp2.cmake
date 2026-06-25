# FindNghttp2.cmake
# ----------------
# Find nghttp2 library (HTTP/2 C library).
#
# This module defines:
#   Nghttp2_FOUND        - True if the library was found
#   Nghttp2::Nghttp2     - Imported target for linking
#   Nghttp2_INCLUDE_DIRS - Include directories
#   Nghttp2_LIBRARIES    - Libraries to link against

# Try pkg-config first
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PC_NGHTTP2 QUIET libnghttp2)
endif()

find_path(Nghttp2_INCLUDE_DIRS
  NAMES nghttp2/nghttp2.h
  HINTS
    ${PC_NGHTTP2_INCLUDE_DIRS}
    /opt/homebrew/opt/libnghttp2/include
    /opt/homebrew/opt/nghttp2/include
    /usr/local/opt/libnghttp2/include
    /usr/local/opt/nghttp2/include
    /usr/local/include
    /usr/include
)

find_library(Nghttp2_LIBRARIES
  NAMES nghttp2 libnghttp2
  HINTS
    ${PC_NGHTTP2_LIBRARY_DIRS}
    /opt/homebrew/opt/libnghttp2/lib
    /opt/homebrew/opt/nghttp2/lib
    /usr/local/opt/libnghttp2/lib
    /usr/local/opt/nghttp2/lib
    /usr/local/lib
    /usr/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Nghttp2
  REQUIRED_VARS Nghttp2_LIBRARIES Nghttp2_INCLUDE_DIRS
  VERSION_VAR PC_NGHTTP2_VERSION
)

if(Nghttp2_FOUND AND NOT TARGET Nghttp2::Nghttp2)
  add_library(Nghttp2::Nghttp2 UNKNOWN IMPORTED)
  set_target_properties(Nghttp2::Nghttp2 PROPERTIES
    IMPORTED_LOCATION "${Nghttp2_LIBRARIES}"
    INTERFACE_INCLUDE_DIRECTORIES "${Nghttp2_INCLUDE_DIRS}"
  )
endif()

mark_as_advanced(Nghttp2_INCLUDE_DIRS Nghttp2_LIBRARIES)
