# FindMbedTLS.cmake
# ------------------
# Find mbedTLS library (lightweight TLS library).
#
# This module defines:
#   MbedTLS_FOUND        - True if the library was found
#   MbedTLS::MbedTLS     - Imported target for linking
#   MbedTLS_INCLUDE_DIRS - Include directories
#   MbedTLS_LIBRARIES    - Libraries to link against

find_path(MbedTLS_INCLUDE_DIRS
  NAMES mbedtls/ssl.h
  PATHS
    /opt/homebrew/opt/mbedtls/include
    /usr/local/opt/mbedtls/include
    /usr/local/include
    /usr/include
)

find_library(MbedTLS_LIBRARY_mbedtls
  NAMES mbedtls
  PATHS
    /opt/homebrew/opt/mbedtls/lib
    /usr/local/opt/mbedtls/lib
    /usr/local/lib
    /usr/lib
)

find_library(MbedTLS_LIBRARY_mbedx509
  NAMES mbedx509
  PATHS
    /opt/homebrew/opt/mbedtls/lib
    /usr/local/opt/mbedtls/lib
    /usr/local/lib
    /usr/lib
)

find_library(MbedTLS_LIBRARY_mbedcrypto
  NAMES mbedcrypto
  PATHS
    /opt/homebrew/opt/mbedtls/lib
    /usr/local/opt/mbedtls/lib
    /usr/local/lib
    /usr/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MbedTLS
  REQUIRED_VARS MbedTLS_LIBRARY_mbedtls MbedTLS_LIBRARY_mbedx509 MbedTLS_LIBRARY_mbedcrypto MbedTLS_INCLUDE_DIRS
)

if(MbedTLS_FOUND AND NOT TARGET MbedTLS::MbedTLS)
  add_library(MbedTLS::MbedTLS UNKNOWN IMPORTED)
  set_target_properties(MbedTLS::MbedTLS PROPERTIES
    IMPORTED_LOCATION "${MbedTLS_LIBRARY_mbedtls}"
    INTERFACE_INCLUDE_DIRECTORIES "${MbedTLS_INCLUDE_DIRS}"
    INTERFACE_LINK_LIBRARIES "${MbedTLS_LIBRARY_mbedx509};${MbedTLS_LIBRARY_mbedcrypto}"
  )

  # Create separate targets for individual libs in case they're needed
  add_library(MbedTLS::mbedtls UNKNOWN IMPORTED)
  set_target_properties(MbedTLS::mbedtls PROPERTIES
    IMPORTED_LOCATION "${MbedTLS_LIBRARY_mbedtls}"
    INTERFACE_INCLUDE_DIRECTORIES "${MbedTLS_INCLUDE_DIRS}"
  )
  add_library(MbedTLS::mbedx509 UNKNOWN IMPORTED)
  set_target_properties(MbedTLS::mbedx509 PROPERTIES
    IMPORTED_LOCATION "${MbedTLS_LIBRARY_mbedx509}"
    INTERFACE_INCLUDE_DIRECTORIES "${MbedTLS_INCLUDE_DIRS}"
  )
  add_library(MbedTLS::mbedcrypto UNKNOWN IMPORTED)
  set_target_properties(MbedTLS::mbedcrypto PROPERTIES
    IMPORTED_LOCATION "${MbedTLS_LIBRARY_mbedcrypto}"
    INTERFACE_INCLUDE_DIRECTORIES "${MbedTLS_INCLUDE_DIRS}"
  )
endif()

mark_as_advanced(
  MbedTLS_INCLUDE_DIRS
  MbedTLS_LIBRARY_mbedtls
  MbedTLS_LIBRARY_mbedx509
  MbedTLS_LIBRARY_mbedcrypto
)
