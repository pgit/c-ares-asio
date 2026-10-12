#
# A consumer of cares_asio, standing in for a project that has nothing to do with this one. It
# sets no language standard and names no dependency of the library: all of that has to come with
# the one target it links. See ../cmake_test.cmake for how it is run.
#
cmake_minimum_required(VERSION 3.25...4.4)

project(cares_asio_cmake_test LANGUAGES CXX)

set(CARES_ASIO_CONSUME "find_package" CACHE STRING "find_package or add_subdirectory")

if (CARES_ASIO_CONSUME STREQUAL "find_package")
    find_package(cares_asio 0.1 CONFIG REQUIRED)
elseif (CARES_ASIO_CONSUME STREQUAL "add_subdirectory")
    add_subdirectory(../.. cares_asio)
else()
    message(FATAL_ERROR "CARES_ASIO_CONSUME is '${CARES_ASIO_CONSUME}'")
endif()

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE cares_asio::cares_asio)

enable_testing()
add_test(NAME consumer COMMAND consumer)
