# - Try to find the liblpc4370sdr library
# Once done this defines
#
#  LIBLPC4370SDR_FOUND - system has liblpc4370sdr
#  LIBLPC4370SDR_INCLUDE_DIR - the liblpc4370sdr include directory
#  LIBLPC4370SDR_LIBRARIES - Link these to use liblpc4370sdr

# Copyright (c) 2013  Benjamin Vernoux
# Copyright (c) 2026 Bertold Van den Bergh <vandenbergh@bertold.org> (modifications for lpc4370sdr)
#


if (LIBLPC4370SDR_INCLUDE_DIR AND LIBLPC4370SDR_LIBRARIES)

  # in cache already
  set(LIBLPC4370SDR_FOUND TRUE)

else (LIBLPC4370SDR_INCLUDE_DIR AND LIBLPC4370SDR_LIBRARIES)
  IF (NOT WIN32)
    # use pkg-config to get the directories and then use these values
    # in the FIND_PATH() and FIND_LIBRARY() calls
    find_package(PkgConfig)
    pkg_check_modules(PC_LIBLPC4370SDR QUIET liblpc4370sdr)
  ENDIF(NOT WIN32)

  FIND_PATH(LIBLPC4370SDR_INCLUDE_DIR
    NAMES lpc4370sdr.h
    HINTS $ENV{LIBLPC4370SDR_DIR}/include ${PC_LIBLPC4370SDR_INCLUDEDIR}
    PATHS /usr/local/include/liblpc4370sdr /usr/include/liblpc4370sdr /usr/local/include
    /usr/include ${CMAKE_SOURCE_DIR}/../liblpc4370sdr/src
    /opt/local/include/liblpc4370sdr
    ${LIBLPC4370SDR_INCLUDE_DIR}
  )

  set(liblpc4370sdr_library_names lpc4370sdr)

  FIND_LIBRARY(LIBLPC4370SDR_LIBRARIES
    NAMES ${liblpc4370sdr_library_names}
    HINTS $ENV{LIBLPC4370SDR_DIR}/lib ${PC_LIBLPC4370SDR_LIBDIR}
    PATHS /usr/local/lib /usr/lib /opt/local/lib ${PC_LIBLPC4370SDR_LIBDIR} ${PC_LIBLPC4370SDR_LIBRARY_DIRS} ${CMAKE_SOURCE_DIR}/../liblpc4370sdr/src
  )

  if(LIBLPC4370SDR_INCLUDE_DIR)
    set(CMAKE_REQUIRED_INCLUDES ${LIBLPC4370SDR_INCLUDE_DIR})
  endif()

  if(LIBLPC4370SDR_LIBRARIES)
    set(CMAKE_REQUIRED_LIBRARIES ${LIBLPC4370SDR_LIBRARIES})
  endif()

  include(FindPackageHandleStandardArgs)
  FIND_PACKAGE_HANDLE_STANDARD_ARGS(LIBLPC4370SDR DEFAULT_MSG LIBLPC4370SDR_LIBRARIES LIBLPC4370SDR_INCLUDE_DIR)

  MARK_AS_ADVANCED(LIBLPC4370SDR_INCLUDE_DIR LIBLPC4370SDR_LIBRARIES)

endif (LIBLPC4370SDR_INCLUDE_DIR AND LIBLPC4370SDR_LIBRARIES)