# Copyright 2012 Jared Boone
# Copyright 2013/2014/2024 Benjamin Vernoux
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

set(INSTALL_DEFAULT_BINDIR "bin" CACHE STRING "Appended to CMAKE_INSTALL_PREFIX")

if(MSVC)
add_library(libgetopt_static STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../getopt/getopt.c
)
endif()

add_executable(lpc4370sdr_gpio lpc4370sdr_gpio.c)
install(TARGETS lpc4370sdr_gpio RUNTIME DESTINATION ${INSTALL_DEFAULT_BINDIR})

add_executable(lpc4370sdr_gpiodir lpc4370sdr_gpiodir.c)
install(TARGETS lpc4370sdr_gpiodir RUNTIME DESTINATION ${INSTALL_DEFAULT_BINDIR})

add_executable(lpc4370sdr_lib_version lpc4370sdr_lib_version.c)
install(TARGETS lpc4370sdr_lib_version RUNTIME DESTINATION ${INSTALL_DEFAULT_BINDIR})

add_executable(lpc4370sdr_si5351c lpc4370sdr_si5351c.c)
install(TARGETS lpc4370sdr_si5351c RUNTIME DESTINATION ${INSTALL_DEFAULT_BINDIR})

add_executable(lpc4370sdr_r820t lpc4370sdr_r820t.c)
install(TARGETS lpc4370sdr_r820t RUNTIME DESTINATION ${INSTALL_DEFAULT_BINDIR})

add_executable(lpc4370sdr_spiflash lpc4370sdr_spiflash.c)
install(TARGETS lpc4370sdr_spiflash RUNTIME DESTINATION ${INSTALL_DEFAULT_BINDIR})

add_executable(lpc4370sdr_calibrate lpc4370sdr_calibrate.c)
install(TARGETS lpc4370sdr_calibrate RUNTIME DESTINATION ${INSTALL_DEFAULT_BINDIR})

add_executable(lpc4370sdr_info lpc4370sdr_info.c)
install(TARGETS lpc4370sdr_info RUNTIME DESTINATION ${INSTALL_DEFAULT_BINDIR})

add_executable(lpc4370sdr_rx lpc4370sdr_rx.c)
install(TARGETS lpc4370sdr_rx RUNTIME DESTINATION ${INSTALL_DEFAULT_BINDIR})

add_executable(lpc4370sdr_debug lpc4370sdr_debug.c)
install(TARGETS lpc4370sdr_debug RUNTIME DESTINATION ${INSTALL_DEFAULT_BINDIR})

# POSIX only (unistd.h, usleep(), clock_gettime()): MinGW has them, MSVC does not
if(NOT MSVC)
add_executable(lpc4370sdr_extcheck lpc4370sdr_extcheck.c)
install(TARGETS lpc4370sdr_extcheck RUNTIME DESTINATION ${INSTALL_DEFAULT_BINDIR})
endif()

if(NOT liblpc4370sdr_SOURCE_DIR)
include_directories(${LIBLPC4370SDR_INCLUDE_DIR})
LIST(APPEND TOOLS_LINK_LIBS ${LIBLPC4370SDR_LIBRARIES})
else()
LIST(APPEND TOOLS_LINK_LIBS lpc4370sdr)
endif()

if(MSVC)
LIST(APPEND TOOLS_LINK_LIBS libgetopt_static)
endif()

# lpc4370sdr_rx locks with pthreads outside Windows; lpc4370sdr_extcheck's clock_gettime() is in winpthreads on MinGW
find_package(Threads)

target_link_libraries(lpc4370sdr_gpio ${TOOLS_LINK_LIBS})
target_link_libraries(lpc4370sdr_gpiodir ${TOOLS_LINK_LIBS})
target_link_libraries(lpc4370sdr_lib_version ${TOOLS_LINK_LIBS})
target_link_libraries(lpc4370sdr_si5351c ${TOOLS_LINK_LIBS})
target_link_libraries(lpc4370sdr_r820t ${TOOLS_LINK_LIBS})
target_link_libraries(lpc4370sdr_spiflash ${TOOLS_LINK_LIBS})
target_link_libraries(lpc4370sdr_calibrate ${TOOLS_LINK_LIBS})
target_link_libraries(lpc4370sdr_info ${TOOLS_LINK_LIBS})
target_link_libraries(lpc4370sdr_rx ${TOOLS_LINK_LIBS} ${CMAKE_THREAD_LIBS_INIT})
target_link_libraries(lpc4370sdr_debug ${TOOLS_LINK_LIBS})
if(NOT MSVC)
target_link_libraries(lpc4370sdr_extcheck ${TOOLS_LINK_LIBS} ${CMAKE_THREAD_LIBS_INIT})
endif()
