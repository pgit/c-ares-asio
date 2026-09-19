cmake_minimum_required(VERSION 3.30)
project(c_ares_asio VERSION 0.0.1)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED On)
set(CMAKE_EXPORT_COMPILE_COMMANDS On)

add_compile_options(-stdlib=libc++)
add_link_options(-stdlib=libc++)

# we don't use fmtlib any more
add_compile_definitions(SPDLOG_USE_STD_FORMAT)

# don't allow use of deprecated ASIO interfaces
add_compile_definitions(BOOST_ASIO_NO_DEPRECATED)

if (CMAKE_BUILD_TYPE MATCHES Debug)
    # https://www.boost.org/doc/libs/1_89_0/doc/html/boost_asio/overview/core/buffers.html#boost_asio.overview.core.buffers.buffer_debugging
    # add_compile_definitions(BOOST_ASIO_ENABLE_BUFFER_DEBUGGING)

    # ASIO handler tracking output (very noisy)
    # add_compile_definitions(BOOST_ASIO_ENABLE_HANDLER_TRACKING)

    # address sanitizer
    # add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    # add_link_options(-fsanitize=address)
endif()

# --------------------------------------------------------------------------------------------------

find_package(Boost 1.88 COMPONENTS program_options REQUIRED)
find_package(Threads REQUIRED)
find_package(spdlog REQUIRED)

# --------------------------------------------------------------------------------------------------

add_subdirectory(src)
