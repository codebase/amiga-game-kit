cmake_minimum_required(VERSION 3.14.0)
project(sidescroller LANGUAGES C)

if(NOT AMIGA)
	message(SEND_ERROR "This project only compiles for Amiga - use 'agk build'")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DAMIGA -Wall -Wextra -fomit-frame-pointer")

# Build options (ACE and AGK) go in agk.toml under [cmake], e.g.
#   [cmake]
#   ACE_BOB_PRISTINE_BUFFER = true
# or one-off: agk build -D ACE_DEBUG=ON. Don't set() them here: ACE declares
# them as cache variables, so a plain set() is ignored on the first configure.
# Useful ones (default):
#   ACE_DEBUG (OFF)                   ACE logging + sanity checks, slow
#   ACE_BOB_WRAP_Y (ON)               BOBs wrap vertically; OFF is faster for simple buffers
#   ACE_BOB_PRISTINE_BUFFER (OFF)     BOBs restore from a clean background copy;
#                                     bobManagerCreate() then takes an extra argument
#   ACE_SCROLLBUFFER_ENABLE_SCROLL_X/Y (ON)   ACE_SCROLLBUFFER_X/Y_MARGIN_SIZE (1)
#   ACE_SCROLLBUFFER_POT_BITMAP_HEIGHT (ON)   faster, more chip RAM
#   ACE_USE_ECS_FEATURES / ACE_USE_AGA_FEATURES (OFF)
#   AGK_DEBUG_CHANNEL (host)          "serial" for debugging on real hardware
# Full list: third_party/ACE/cmake/ace_config.cmake

# tools/build passes AGK_KIT and ACE_DIR; these defaults are for manual builds.
set(AGK_KIT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "Amiga Game Kit root")
set(ACE_DIR "${AGK_KIT}/third_party/ACE" CACHE PATH "ACE source dir")
add_subdirectory(${ACE_DIR} ace)
include_directories(${ACE_DIR}/include)
add_subdirectory(${AGK_KIT}/runtime agk)

file(GLOB_RECURSE SOURCES src/*.c)
# Art converted by agk (art/ -> build/art/art.c + art.h), if the project has any
if(EXISTS ${CMAKE_BINARY_DIR}/art/art.c)
	list(APPEND SOURCES ${CMAKE_BINARY_DIR}/art/art.c)
	include_directories(${CMAKE_BINARY_DIR}/art)
endif()
# Sound converted by agk (sound/ -> build/sound/sound.c + sound.h), if any
if(EXISTS ${CMAKE_BINARY_DIR}/sound/sound.c)
	list(APPEND SOURCES ${CMAKE_BINARY_DIR}/sound/sound.c)
	include_directories(${CMAKE_BINARY_DIR}/sound)
endif()
add_executable(sidescroller ${SOURCES})
target_link_libraries(sidescroller ace agk)
