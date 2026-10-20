# Copyright 2012 Jared Boone
# Copyright 2013/2014 Benjamin Vernoux
# Copyright (c) 2026 Bertold Van den Bergh <vandenbergh@bertold.org> (modifications for lpc4370sdr)
#
# This file is part of lpc4370sdr (derived from AirSpy, based on the HackRF project).
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2, or (at your option)
# any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; see the file COPYING.  If not, write to
# the Free Software Foundation, Inc., 51 Franklin Street,
# Boston, MA 02110-1301, USA.
#

# Based heavily upon the libftdi cmake setup.

cmake_minimum_required(VERSION 3.10)
project(liblpc4370sdr C)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/lpc4370sdr.h LPC4370SDR_H_CONTENTS)

STRING(REGEX MATCH "\#define LPC4370SDR_VER_MAJOR[ \t]+([0-9]+)" LPC4370SDR_VER_LINE ${LPC4370SDR_H_CONTENTS})
set(LPC4370SDR_VER_MJ ${CMAKE_MATCH_1})

STRING(REGEX MATCH "\#define LPC4370SDR_VER_MINOR[ \t]+([0-9]+)" LPC4370SDR_VER_LINE ${LPC4370SDR_H_CONTENTS})
set(LPC4370SDR_VER_MI ${CMAKE_MATCH_1})

STRING(REGEX MATCH "\#define LPC4370SDR_VER_REVISION[ \t]+([0-9]+)" LPC4370SDR_VER_LINE ${LPC4370SDR_H_CONTENTS})
set(LPC4370SDR_VER_RE ${CMAKE_MATCH_1})

set(LPC4370SDR_VER_MAJOR ${LPC4370SDR_VER_MJ})
set(LPC4370SDR_VER_MINOR ${LPC4370SDR_VER_MI})
set(LPC4370SDR_VER_REVISION ${LPC4370SDR_VER_RE})

set(PACKAGE liblpc4370sdr)
set(VERSION_STRING ${LPC4370SDR_VER_MAJOR}.${LPC4370SDR_VER_MINOR}.${LPC4370SDR_VER_REVISION})
set(VERSION ${VERSION_STRING})
set(CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/../cmake/modules)
# Standalone builds pick the C standard here (the top-level project sets it for both)
if(NOT lpc4370sdr_all_SOURCE_DIR AND NOT CMAKE_C_STANDARD)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_EXTENSIONS ON)
endif()
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(APPLE)
    set(GR_LIBRARY_DIR "lib")
    if(NOT CMAKE_INSTALL_NAME_DIR)
        set(CMAKE_INSTALL_NAME_DIR
            ${CMAKE_INSTALL_PREFIX}/${GR_LIBRARY_DIR} CACHE
            PATH "Library Install Name Destination Directory" FORCE)
    endif(NOT CMAKE_INSTALL_NAME_DIR)
    if(NOT CMAKE_INSTALL_RPATH)
        set(CMAKE_INSTALL_RPATH
            ${CMAKE_INSTALL_PREFIX}/${GR_LIBRARY_DIR} CACHE
            PATH "Library Install RPath" FORCE)
    endif(NOT CMAKE_INSTALL_RPATH)
    if(NOT CMAKE_BUILD_WITH_INSTALL_RPATH)
        set(CMAKE_BUILD_WITH_INSTALL_RPATH ON CACHE
            BOOL "Do Build Using Library Install RPath" FORCE)
    endif(NOT CMAKE_BUILD_WITH_INSTALL_RPATH)
endif(APPLE)

if(MSVC)
	set(THREADS_USE_PTHREADS_WIN32 true)
else()
	add_definitions(-Wall)

	INCLUDE(TestBigEndian)
	TEST_BIG_ENDIAN(BIGENDIAN)
	if(${BIGENDIAN})
		 add_definitions(-DLPC4370SDR_BIG_ENDIAN)
	endif(${BIGENDIAN})
endif()
find_package(LIBUSB REQUIRED)
find_package(Threads REQUIRED)

include_directories(${LIBUSB_INCLUDE_DIR} ${THREADS_PTHREADS_INCLUDE_DIR})

add_subdirectory(src)

option(BUILD_AIRSPY_COMPAT "Build libairspy.so.0 / airspy.dll: the libairspy API on top of liblpc4370sdr" OFF)
if(BUILD_AIRSPY_COMPAT)
	add_subdirectory(compat)
endif()

########################################################################
# Create Pkg Config File
########################################################################
FOREACH(inc ${LIBUSB_INCLUDE_DIR})
    LIST(APPEND LPC4370SDR_PC_CFLAGS "-I${inc}")
ENDFOREACH(inc)
# Static linking: libusb-1.0 comes in through Requires.private, the thread library here
if(CMAKE_THREAD_LIBS_INIT)
    LIST(APPEND LPC4370SDR_PC_LIBS ${CMAKE_THREAD_LIBS_INIT})
elseif(UNIX)
    LIST(APPEND LPC4370SDR_PC_LIBS "-pthread")
endif()

# use space-separation format for the pc file
STRING(REPLACE ";" " " LPC4370SDR_PC_CFLAGS "${LPC4370SDR_PC_CFLAGS}")
STRING(REPLACE ";" " " LPC4370SDR_PC_LIBS "${LPC4370SDR_PC_LIBS}")

# unset these vars to avoid hard-coded paths to cross environment
IF(CMAKE_CROSSCOMPILING)
    UNSET(LPC4370SDR_PC_CFLAGS)
ENDIF(CMAKE_CROSSCOMPILING)

set(prefix ${CMAKE_INSTALL_PREFIX})
set(exec_prefix \${prefix})
set(libdir \${exec_prefix}/lib${LIB_SUFFIX})
set(includedir \${prefix}/include)

CONFIGURE_FILE(
    ${CMAKE_CURRENT_SOURCE_DIR}/liblpc4370sdr.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/liblpc4370sdr.pc
@ONLY)

INSTALL(
    FILES ${CMAKE_CURRENT_BINARY_DIR}/liblpc4370sdr.pc
    DESTINATION lib${LIB_SUFFIX}/pkgconfig
)

########################################################################
# Create uninstall target
########################################################################
if(NOT lpc4370sdr_all_SOURCE_DIR)
configure_file(
    ${PROJECT_SOURCE_DIR}/../cmake/cmake_uninstall.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
@ONLY)

add_custom_target(uninstall
    ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
)
endif()
