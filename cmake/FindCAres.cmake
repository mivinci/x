# FindCAres.cmake
# ----------------
# Find c-ares library (async DNS resolver).
#
# This module defines:
#   CAres_FOUND        - True if the library was found
#   CAres::CAres       - Imported target for linking
#   CAres_INCLUDE_DIRS - Include directories
#   CAres_LIBRARIES    - Libraries to link against

include_guard(GLOBAL)

# Try pkg-config first
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PC_CARES QUIET libcares)
endif()

find_path(CAres_INCLUDE_DIRS
  NAMES ares.h
  HINTS
    ${PC_CARES_INCLUDE_DIRS}
    /opt/homebrew/include
    /usr/local/opt/c-ares/include
    /usr/local/include
)

find_library(CAres_LIBRARIES
  NAMES cares
  HINTS
    ${PC_CARES_LIBRARY_DIRS}
    /opt/homebrew/lib
    /usr/local/opt/c-ares/lib
    /usr/local/lib
)

if(CAres_INCLUDE_DIRS AND CAres_LIBRARIES)
  set(CAres_FOUND TRUE)
  if(NOT TARGET CAres::CAres)
    add_library(CAres::CAres UNKNOWN IMPORTED)
    set_target_properties(CAres::CAres PROPERTIES
      IMPORTED_LOCATION "${CAres_LIBRARIES}"
      INTERFACE_INCLUDE_DIRECTORIES "${CAres_INCLUDE_DIRS}"
    )
  endif()
endif()

mark_as_advanced(CAres_INCLUDE_DIRS CAres_LIBRARIES)
