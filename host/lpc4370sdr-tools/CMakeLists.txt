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

cmake_minimum_required(VERSION 3.5)
project(lpc4370sdr-tools C)
set(MAJOR_VERSION 0)
set(MINOR_VERSION 2)
set(PACKAGE lpc4370sdr-tools)
set(VERSION_STRING ${MAJOR_VERSION}.${MINOR_VERSION})
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
add_definitions(-D_FILE_OFFSET_BITS=64)

if(MSVC)
include_directories(getopt)
add_definitions(/D _CRT_SECURE_NO_WARNINGS)
else()
add_definitions(-Wall)
endif()

if(NOT liblpc4370sdr_SOURCE_DIR)
find_package(LIBLPC4370SDR REQUIRED)
include_directories(${LIBLPC4370SDR_INCLUDE_DIR})
else()
include_directories(${liblpc4370sdr_SOURCE_DIR}/src)
endif()

add_subdirectory(src)

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

########################################################################
# Install udev rules
########################################################################
option(INSTALL_UDEV_RULES "Install udev rules for lpc4370sdr boards" OFF)
if (INSTALL_UDEV_RULES)
    install (
        FILES 52-lpc4370sdr.rules
        DESTINATION "/etc/udev/rules.d"
        COMPONENT "udev"
        )
else (INSTALL_UDEV_RULES)
    message (STATUS "Udev rules not being installed, install them with -DINSTALL_UDEV_RULES=ON")
endif (INSTALL_UDEV_RULES)
