#
# Copyright (c) 2012, Jared Boone <jared@sharebrained.com>
# Copyright (c) 2013, Michael Ossmann <mike@ossmann.com>
# Copyright (c) 2013, Youssef Touil <youssef@airspy.com>
# Copyright (c) 2013/2014, Benjamin Vernoux <bvernoux@airspy.com>
# Copyright (c) 2026 Bertold Van den Bergh <vandenbergh@bertold.org> (modifications for lpc4370sdr)
#
# This file is part of lpc4370sdr (derived from AirSpy, based on the HackRF project).
# 
# All rights reserved.
# 
# Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
# 
#     Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
#     Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the 
# 	documentation and/or other materials provided with the distribution.
#     Neither the name of Great Scott Gadgets nor the names of its contributors may be used to endorse or promote products derived from this software
# 	without specific prior written permission.
# 
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, 
# THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
# IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
# (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
# HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
# Based heavily upon the libftdi cmake setup.

# Targets
# lpc4370sdr.c includes the I/Q converters itself
set(c_sources ${CMAKE_CURRENT_SOURCE_DIR}/lpc4370sdr.c CACHE INTERNAL "List of C sources")
# The installed headers: the API only
set(c_headers ${CMAKE_CURRENT_SOURCE_DIR}/lpc4370sdr.h ${CMAKE_CURRENT_SOURCE_DIR}/lpc4370sdr_commands.h CACHE INTERNAL "List of C headers")

if(MINGW)
    # This gets us DLL resource information when compiling on MinGW.
    if(NOT CMAKE_RC_COMPILER)
        set(CMAKE_RC_COMPILER windres.exe)
    endif()

    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/lpc4370sdrrc.obj
                       DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/lpc4370sdr.h
                       COMMAND ${CMAKE_RC_COMPILER}
                            -D GCC_WINDRES
                            -I ${CMAKE_CURRENT_SOURCE_DIR}
                            -I ${CMAKE_CURRENT_BINARY_DIR}
                            -o ${CMAKE_CURRENT_BINARY_DIR}/lpc4370sdrrc.obj
                            -i ${CMAKE_CURRENT_SOURCE_DIR}/win32/lpc4370sdr.rc)
    set(LPC4370SDR_DLL_SRCS ${CMAKE_CURRENT_BINARY_DIR}/lpc4370sdrrc.obj)
endif(MINGW)

# Dynamic library
add_library(lpc4370sdr SHARED ${c_sources} ${LPC4370SDR_DLL_SRCS})
set_target_properties(lpc4370sdr PROPERTIES VERSION ${LPC4370SDR_VER_MAJOR}.${LPC4370SDR_VER_MINOR}.${LPC4370SDR_VER_REVISION})
set_target_properties(lpc4370sdr PROPERTIES SOVERSION 0)
# Export the API only (LPC4370SDR_API), and link every symbol the library uses
set_target_properties(lpc4370sdr PROPERTIES C_VISIBILITY_PRESET hidden)
if(UNIX AND NOT APPLE)
    set_target_properties(lpc4370sdr PROPERTIES LINK_FLAGS "-Wl,--no-undefined")
endif()

if( ${WIN32} )
set_target_properties(lpc4370sdr PROPERTIES
                      RUNTIME_OUTPUT_DIRECTORY_RELEASE ../../lpc4370sdr-tools/src
                      )
endif( ${WIN32} )

# Static library
add_library(lpc4370sdr-static STATIC ${c_sources})
if(MSVC)
	set_target_properties(lpc4370sdr-static PROPERTIES OUTPUT_NAME "lpc4370sdr_static")
else()
	set_target_properties(lpc4370sdr-static PROPERTIES OUTPUT_NAME "lpc4370sdr")
endif()

set_target_properties(lpc4370sdr-static PROPERTIES C_VISIBILITY_PRESET hidden)
set_target_properties(lpc4370sdr PROPERTIES CLEAN_DIRECT_OUTPUT 1)
set_target_properties(lpc4370sdr-static PROPERTIES CLEAN_DIRECT_OUTPUT 1)

# Dependencies
target_link_libraries(lpc4370sdr ${LIBUSB_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})
   
# For cygwin just force UNIX OFF and WIN32 ON
if( ${CYGWIN} )
  SET(UNIX OFF)
  SET(WIN32 ON)
endif( ${CYGWIN} )

if( ${UNIX} )
   install(TARGETS lpc4370sdr
           LIBRARY DESTINATION lib${LIB_SUFFIX}
           COMPONENT sharedlibs
           )
   install(TARGETS lpc4370sdr-static
           ARCHIVE DESTINATION lib${LIB_SUFFIX}
           COMPONENT staticlibs
           )
   install(FILES ${c_headers}
           DESTINATION include/${PROJECT_NAME}
           COMPONENT headers
           )
endif( ${UNIX} )

if( ${WIN32} )
   install(TARGETS lpc4370sdr
           DESTINATION bin
           COMPONENT sharedlibs
           )
   install(TARGETS lpc4370sdr-static
           DESTINATION bin
           COMPONENT staticlibs
           )
   install(FILES ${c_headers}
           DESTINATION include/${PROJECT_NAME}
           COMPONENT headers
           )
endif( ${WIN32} )
