#
# A consumer of c_ares_async, standing in for a project that has nothing to do with this one. It
# sets no language standard and names no dependency of the library: all of that has to come with
# the one target it links. See ../cmake_test.cmake for how it is run.
#
cmake_minimum_required(VERSION 3.25...4.4)

project(c_ares_async_cmake_test LANGUAGES CXX)

set(C_ARES_ASYNC_CONSUME "find_package" CACHE STRING "find_package or add_subdirectory")

if (C_ARES_ASYNC_CONSUME STREQUAL "find_package")
    find_package(c_ares_async 0.1 CONFIG REQUIRED)
elseif (C_ARES_ASYNC_CONSUME STREQUAL "add_subdirectory")
    add_subdirectory(../.. c_ares_async)
else()
    message(FATAL_ERROR "C_ARES_ASYNC_CONSUME is '${C_ARES_ASYNC_CONSUME}'")
endif()

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE c_ares_async::c_ares_async)

enable_testing()
add_test(NAME consumer COMMAND consumer)
