# This module defines:
#  SentencePiece_FOUND - True if SentencePiece was found
#  SentencePiece_INCLUDE_DIRS - The SentencePiece include directories
#  SentencePiece_LIBRARIES - The libraries needed to use SentencePiece

# Fall back to environment variables when CMake variables are not set.
# This lets `nix develop` export the paths and have cmake pick them up
# without requiring explicit -D flags.
if(NOT SentencePiece_INCLUDE_DIR AND DEFINED ENV{SentencePiece_INCLUDE_DIR})
    set(SentencePiece_INCLUDE_DIR "$ENV{SentencePiece_INCLUDE_DIR}" CACHE PATH "SentencePiece include directory")
endif()
if(NOT SentencePiece_LIBRARY_DIR AND DEFINED ENV{SentencePiece_LIBRARY_DIR})
    set(SentencePiece_LIBRARY_DIR "$ENV{SentencePiece_LIBRARY_DIR}" CACHE PATH "SentencePiece library directory")
endif()

# Search for the header file
find_path(SentencePiece_INCLUDE_DIR
    NAMES sentencepiece_processor.h # Common header for SentencePiece
    PATHS ${SentencePiece_INCLUDE_DIR}
    NO_DEFAULT_PATH
    DOC "SentencePiece include directory"
)

# Search for the library
find_library(SentencePiece_LIBRARY
    NAMES sentencepiece
    PATHS ${SentencePiece_LIBRARY_DIR}
    NO_DEFAULT_PATH
    DOC "SentencePiece library"
)

# Handle the REQUIRED and QUIET arguments and set SentencePiece_FOUND
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SentencePiece
    REQUIRED_VARS SentencePiece_LIBRARY SentencePiece_INCLUDE_DIR
    FAIL_MESSAGE "Could NOT find SentencePiece. Set SentencePiece_INCLUDE_DIR and SentencePiece_LIBRARY_DIR."
)

mark_as_advanced(SentencePiece_INCLUDE_DIR SentencePiece_LIBRARY)

# Set the output variables
if(SentencePiece_FOUND)
    set(SentencePiece_LIBRARIES ${SentencePiece_LIBRARY})
    set(SentencePiece_INCLUDE_DIRS ${SentencePiece_INCLUDE_DIR})
endif()
