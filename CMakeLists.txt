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

# ares_process_fds() was added in 1.34
find_package(c-ares 1.34 CONFIG REQUIRED)

# c-ares has no DNS over TLS of its own -- see src/tls_transport.hpp. Either library will do: the
# transport sticks to the OpenSSL API that AWS-LC implements as well, so the choice is made here
# and nowhere else. Everything that needs TLS links 'tls'.
set(TLS_LIBRARY "AWS-LC" CACHE STRING "TLS library to build against: OpenSSL or AWS-LC")
set_property(CACHE TLS_LIBRARY PROPERTY STRINGS OpenSSL AWS-LC)

add_library(tls INTERFACE)

if (TLS_LIBRARY STREQUAL "OpenSSL")
    find_package(OpenSSL REQUIRED)
    target_link_libraries(tls INTERFACE OpenSSL::SSL OpenSSL::Crypto)
    message(STATUS "Using TLS library: OpenSSL ${OPENSSL_VERSION}")
elseif (TLS_LIBRARY STREQUAL "AWS-LC")
    set(BORINGSSL_ROOT /opt/boringssl CACHE PATH "Installation prefix of BoringSSL / AWS-LC")
    list(PREPEND CMAKE_PREFIX_PATH ${BORINGSSL_ROOT}) # ssl-config.cmake finds 'crypto' through it
    find_package(ssl CONFIG REQUIRED)
    target_link_libraries(tls INTERFACE AWS::ssl)
    message(STATUS "Using TLS library: AWS-LC (${ssl_DIR})")
else()
    message(FATAL_ERROR "TLS_LIBRARY is '${TLS_LIBRARY}', expected OpenSSL or AWS-LC")
endif()

# --------------------------------------------------------------------------------------------------

add_subdirectory(src)
