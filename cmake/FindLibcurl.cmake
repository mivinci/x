# FindLibcurl.cmake
# ------------------
# Find libcurl (URL transfer library).
#
# This module defines:
#   Libcurl_FOUND        - True if the library was found
#   Libcurl::Libcurl     - Imported target for linking
#   Libcurl_INCLUDE_DIRS - Include directories
#   Libcurl_LIBRARIES    - Libraries to link against

find_path(Libcurl_INCLUDE_DIRS
  NAMES curl/curl.h
  PATHS
    /opt/homebrew/opt/curl/include
    /usr/local/opt/curl/include
    /usr/local/include
    /usr/include
)

find_library(Libcurl_LIBRARIES
  NAMES curl
  PATHS
    /opt/homebrew/opt/curl/lib
    /opt/homebrew/lib
    /usr/local/opt/curl/lib
    /usr/local/lib
    /usr/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libcurl
  REQUIRED_VARS Libcurl_LIBRARIES Libcurl_INCLUDE_DIRS
)

if(Libcurl_FOUND AND NOT TARGET Libcurl::Libcurl)
  add_library(Libcurl::Libcurl UNKNOWN IMPORTED)
  set_target_properties(Libcurl::Libcurl PROPERTIES
    IMPORTED_LOCATION "${Libcurl_LIBRARIES}"
    INTERFACE_INCLUDE_DIRECTORIES "${Libcurl_INCLUDE_DIRS}"
  )

  # Link transitive homebrew static libcurl dependencies on macOS
  if(APPLE)
    find_library(COREFOUNDATION CoreFoundation REQUIRED)
    find_library(SYSTEMCONFIG SystemConfiguration REQUIRED)
    find_package(ZLIB REQUIRED)
    find_library(LIBZSTD NAMES zstd PATHS /opt/homebrew/lib /usr/local/lib)
    find_library(LIBPSL NAMES psl PATHS /opt/homebrew/lib /usr/local/lib)

    set_property(TARGET Libcurl::Libcurl APPEND PROPERTY
      INTERFACE_LINK_LIBRARIES
      ${COREFOUNDATION}
      ${SYSTEMCONFIG}
      ZLIB::ZLIB
    )

    if(LIBZSTD)
      set_property(TARGET Libcurl::Libcurl APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES "${LIBZSTD}")
    endif()
    if(LIBPSL)
      set_property(TARGET Libcurl::Libcurl APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES "${LIBPSL}")
    endif()
  endif()
endif()

mark_as_advanced(Libcurl_INCLUDE_DIRS Libcurl_LIBRARIES)
