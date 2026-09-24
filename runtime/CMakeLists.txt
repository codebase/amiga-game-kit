# AGK runtime: small helpers every AGK game links (serial debug channel).
#   add_subdirectory(${AGK_KIT}/runtime agk)
#   target_link_libraries(mygame agk)
option(AGK_SERIAL "Enable the AGK serial debug channel" ON)

add_library(agk STATIC src/debug.c)
target_include_directories(agk PUBLIC include)
target_link_libraries(agk PUBLIC ace)
if(AGK_SERIAL)
	target_compile_definitions(agk PUBLIC AGK_SERIAL_ENABLED)
endif()
