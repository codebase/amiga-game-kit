cmake_minimum_required(VERSION 3.14.0)
project({{name}} LANGUAGES C)

if(NOT AMIGA)
	message(SEND_ERROR "This project only compiles for Amiga - use 'agk build'")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DAMIGA -Wall -Wextra -fomit-frame-pointer")

# tools/build passes AGK_KIT and ACE_DIR; these defaults are for manual builds.
set(AGK_KIT "{{kit}}" CACHE PATH "Amiga Game Kit root")
set(ACE_DIR "${AGK_KIT}/third_party/ACE" CACHE PATH "ACE source dir")
add_subdirectory(${ACE_DIR} ace)
include_directories(${ACE_DIR}/include)
add_subdirectory(${AGK_KIT}/runtime agk)

file(GLOB_RECURSE SOURCES src/*.c)
add_executable({{name}} ${SOURCES})
target_link_libraries({{name}} ace agk)
