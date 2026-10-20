# Copyright (c) 2026 Bertold Van den Bergh <vandenbergh@bertold.org>
#
# This file is part of lpc4370sdr.
#
# libairspy compatibility library: libairspy.so.0 / airspy.dll with the libairspy 1.0.12 API,
# on top of liblpc4370sdr (built in, one file to drop in). Installed out of the way of a real
# libairspy: point LD_LIBRARY_PATH (or the program's directory on Windows) at it.

add_library(airspy-compat SHARED ${CMAKE_CURRENT_SOURCE_DIR}/airspy_compat.c ${CMAKE_CURRENT_SOURCE_DIR}/airspy_iqconverter.c ${c_sources})
set_target_properties(airspy-compat PROPERTIES OUTPUT_NAME airspy VERSION 1.0.12 SOVERSION 0)
if(WIN32)
    # airspy.dll, the name programs for libairspy load (not libairspy.dll)
    set_target_properties(airspy-compat PROPERTIES PREFIX "")
endif()
target_include_directories(airspy-compat PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(airspy-compat ${LIBUSB_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})
# Export the libairspy API only: liblpc4370sdr is built in hidden, so it neither clashes with nor
# binds to a liblpc4370sdr or libairspy loaded into the same program
target_compile_definitions(airspy-compat PRIVATE LPC4370SDR_NO_EXPORT)
set_target_properties(airspy-compat PROPERTIES C_VISIBILITY_PRESET hidden)
if(UNIX AND NOT APPLE)
    set_target_properties(airspy-compat PROPERTIES LINK_FLAGS "-Wl,--no-undefined")
endif()

install(TARGETS airspy-compat
        LIBRARY DESTINATION lib${LIB_SUFFIX}/lpc4370sdr-airspy
        RUNTIME DESTINATION bin/lpc4370sdr-airspy
        COMPONENT airspy-compat
        )
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/airspy.h ${CMAKE_CURRENT_SOURCE_DIR}/airspy_commands.h
        DESTINATION include/lpc4370sdr-airspy/libairspy
        COMPONENT airspy-compat
        )
