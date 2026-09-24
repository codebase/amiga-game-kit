cmake_minimum_required(VERSION 3.14.0)
project(bobs LANGUAGES C)

if(NOT AMIGA)
	message(SEND_ERROR "This project only compiles for Amiga - use 'agk build'")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DAMIGA -Wall -Wextra -fomit-frame-pointer")

# ACE build options - uncomment to change (defaults: all off).
# set(ACE_DEBUG ON)                # ACE logging + sanity checks (slow)
# set(ACE_BOB_PRISTINE_BUFFER ON)  # BOBs restore from a clean background copy;
#                                  # bobManagerCreate() then takes an extra argument
# set(ACE_BOB_WRAP_Y ON)           # BOBs wrap vertically (scrolling buffers)
# set(ACE_USE_ECS_FEATURES ON)
# AGK debug output: "host" (default) is free and read by agk run/test in the
# emulator; "serial" uses the real serial port for debugging on real hardware.
# set(AGK_DEBUG_CHANNEL serial CACHE STRING "" FORCE)

# tools/build passes AGK_KIT and ACE_DIR; these defaults are for manual builds.
set(AGK_KIT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "Amiga Game Kit root")
set(ACE_DIR "${AGK_KIT}/third_party/ACE" CACHE PATH "ACE source dir")
add_subdirectory(${ACE_DIR} ace)
include_directories(${ACE_DIR}/include)
add_subdirectory(${AGK_KIT}/runtime agk)

file(GLOB_RECURSE SOURCES src/*.c)
add_executable(bobs ${SOURCES})
target_link_libraries(bobs ace agk)
