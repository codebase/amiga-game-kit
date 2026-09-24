# AGK runtime: small helpers every AGK game links.
#   add_subdirectory(${AGK_KIT}/runtime agk)
#   target_link_libraries(mygame agk)
option(AGK_SERIAL "Enable the AGK debug channel (off: every agk* call compiles to nothing)" ON)
# host:   hand strings straight to AGK's emulator via NOOP writes - costs almost
#         nothing, invisible on real hardware (default; what agk run/test read)
# serial: real serial port, 115200 baud - for debugging on real hardware; each
#         character costs CPU time (blocking, or an interrupt with agkDebugAsync)
set(AGK_DEBUG_CHANNEL "host" CACHE STRING "AGK debug channel: host or serial")
set_property(CACHE AGK_DEBUG_CHANNEL PROPERTY STRINGS host serial)

add_library(agk STATIC src/debug.c src/perf.c)
target_include_directories(agk PUBLIC include)
target_link_libraries(agk PUBLIC ace)
if(AGK_SERIAL)
	target_compile_definitions(agk PUBLIC AGK_SERIAL_ENABLED)
	if(AGK_DEBUG_CHANNEL STREQUAL "host")
		target_compile_definitions(agk PRIVATE AGK_CHANNEL_HOST)
	endif()
endif()
