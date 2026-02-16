# FindOpus.cmake
# ---------------
# Find the Opus codec library.
#
# This module defines:
#   Opus_FOUND        - True if Opus was found
#   OPUS_INCLUDE_DIRS - Opus include directories
#   OPUS_LIBRARIES    - Libraries to link against

include(FindPackageHandleStandardArgs)

if (NOT WIN32)
    find_package(PkgConfig QUIET)
    if (PKG_CONFIG_FOUND)
        pkg_check_modules(PC_OPUS QUIET opus)
    endif()
endif()

find_path(OPUS_INCLUDE_DIR
    NAMES opus/opus.h
    HINTS
        ${Opus_DIR}/include
        ${PC_OPUS_INCLUDEDIR}
        ${PC_OPUS_INCLUDE_DIRS}
)

find_library(OPUS_LIBRARY
    NAMES opus
    HINTS
        ${Opus_DIR}/lib
        ${PC_OPUS_LIBDIR}
        ${PC_OPUS_LIBRARY_DIRS}
)

find_package_handle_standard_args(Opus
    REQUIRED_VARS OPUS_LIBRARY OPUS_INCLUDE_DIR
    FAIL_MESSAGE "Could NOT find Opus. Install libopus-dev or set Opus_DIR."
)

mark_as_advanced(OPUS_INCLUDE_DIR OPUS_LIBRARY)

if(Opus_FOUND)
    set(OPUS_LIBRARIES    ${OPUS_LIBRARY})
    set(OPUS_INCLUDE_DIRS ${OPUS_INCLUDE_DIR})
endif()
