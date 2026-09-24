cmake_minimum_required(VERSION 3.14.0)
project(hello LANGUAGES C)

if(NOT AMIGA)
	message(SEND_ERROR "This project only compiles for Amiga")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DAMIGA -Wall -Wextra -fomit-frame-pointer")

set(ACE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../third_party/ACE" CACHE PATH "ACE source dir")
add_subdirectory(${ACE_DIR} ace)
include_directories(${ACE_DIR}/include)

file(GLOB_RECURSE SOURCES src/*.c)
add_executable(hello ${SOURCES})
target_link_libraries(hello ace)
