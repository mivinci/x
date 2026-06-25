# FindUsrsctp.cmake
# ---------------
# Find the usrsctp library (user-space SCTP implementation).
#
# This module defines:
#   Usrsctp_FOUND        - True if the library was found
#   Usrsctp::Usrsctp     - Imported target for linking
#   Usrsctp_INCLUDE_DIRS - Include directories
#   Usrsctp_LIBRARIES    - Libraries to link against

# Try pkg-config first
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PC_USRSCTP QUIET usrsctp libusrsctp)
endif()

find_path(Usrsctp_INCLUDE_DIRS
  NAMES usrsctp.h
  HINTS
    ${PC_USRSCTP_INCLUDE_DIRS}
    /opt/homebrew/opt/libusrsctp/include/usrsctp
    /opt/homebrew/opt/libusrsctp/include
    /usr/local/opt/libusrsctp/include/usrsctp
    /usr/local/opt/libusrsctp/include
    /usr/local/include/usrsctp
    /usr/include/usrsctp
)

find_library(Usrsctp_LIBRARIES
  NAMES usrsctp
  HINTS
    ${PC_USRSCTP_LIBRARY_DIRS}
    /opt/homebrew/opt/libusrsctp/lib
    /usr/local/opt/libusrsctp/lib
    /usr/local/lib
    /usr/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Usrsctp
  REQUIRED_VARS Usrsctp_LIBRARIES Usrsctp_INCLUDE_DIRS
  VERSION_VAR PC_USRSCTP_VERSION
)

if(Usrsctp_FOUND AND NOT TARGET Usrsctp::Usrsctp)
  add_library(Usrsctp::Usrsctp UNKNOWN IMPORTED)
  set_target_properties(Usrsctp::Usrsctp PROPERTIES
    IMPORTED_LOCATION "${Usrsctp_LIBRARIES}"
    INTERFACE_INCLUDE_DIRECTORIES "${Usrsctp_INCLUDE_DIRS}"
  )
endif()

mark_as_advanced(Usrsctp_INCLUDE_DIRS Usrsctp_LIBRARIES)
