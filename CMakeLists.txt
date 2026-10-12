cmake_minimum_required(VERSION 3.25...4.4)

project(
    cares_asio
    VERSION 0.1.0
    DESCRIPTION "c-ares name resolution as Boost.Asio asynchronous operations"
    HOMEPAGE_URL "https://github.com/pgit/c-ares-asio"
    LANGUAGES CXX)

# there are no C++ modules in here, so there is nothing for CMake to scan the sources for
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# --------------------------------------------------------------------------------------------------
#
# Options. Tests and examples are the business of whoever works on the library itself, so they
# are only on by default where this is the top-level project -- not where it came in through
# add_subdirectory() or FetchContent.
#
# Nothing in here sets a compiler, a standard library or warning flags: that is up to the one who
# builds, and CMakePresets.json has what this repository is developed with.
#
if (PROJECT_IS_TOP_LEVEL AND (NOT DEFINED BUILD_TESTING OR BUILD_TESTING))
    set(cares_asio_tests_default ON)
else()
    set(cares_asio_tests_default OFF)
endif()

option(CARES_ASIO_BUILD_TESTS "Build the tests of cares_asio" ${cares_asio_tests_default})
option(CARES_ASIO_BUILD_EXAMPLES "Build the examples of cares_asio" ${PROJECT_IS_TOP_LEVEL})
option(CARES_ASIO_INSTALL "Generate the install rules of cares_asio" ${PROJECT_IS_TOP_LEVEL})

#
# c-ares has no DNS over TLS of its own -- see src/tls_transport.hpp. Either library will do: the
# transport sticks to the OpenSSL API that AWS-LC implements as well, so the choice is made here
# and nowhere else.
#
set(CARES_ASIO_TLS "OpenSSL" CACHE STRING "TLS library to build against: OpenSSL or AWS-LC")
set_property(CACHE CARES_ASIO_TLS PROPERTY STRINGS OpenSSL AWS-LC)

# --------------------------------------------------------------------------------------------------
#
# Dependencies. The versions are kept in variables because the package configuration file asks
# for the same ones again, on behalf of whoever links the installed library.
#
set(CARES_ASIO_BOOST_VERSION 1.88)
set(CARES_ASIO_CARES_VERSION 1.34) # ares_process_fds() was added in 1.34
set(CARES_ASIO_OPENSSL_VERSION 3.2) # SSL_get0_group_name() was added in 3.2

find_package(Boost ${CARES_ASIO_BOOST_VERSION} REQUIRED)
find_package(Threads REQUIRED)
find_package(c-ares ${CARES_ASIO_CARES_VERSION} CONFIG REQUIRED)

#
# https://github.com/c-ares/c-ares/security/advisories/GHSA-6wfj-rwm7-3542 is a use-after-free in
# the completion of a query, which an application calling ares_cancel() can trigger -- and
# cancelling an operation of the resolver does exactly that.
#
if (c-ares_VERSION VERSION_LESS 1.34.7)
    message(WARNING "c-ares ${c-ares_VERSION} is affected by GHSA-6wfj-rwm7-3542, a use-after-free "
                    "that cancelling a lookup can trigger. It is fixed in c-ares 1.34.7.")
endif()

if (CARES_ASIO_TLS STREQUAL "OpenSSL")
    find_package(OpenSSL ${CARES_ASIO_OPENSSL_VERSION} REQUIRED)
    set(cares_asio_tls_libraries OpenSSL::SSL OpenSSL::Crypto)
    message(STATUS "cares_asio: using TLS library OpenSSL ${OPENSSL_VERSION}")
elseif (CARES_ASIO_TLS STREQUAL "AWS-LC")
    # ssl_ROOT says where it is installed, and finds 'crypto' for ssl-config.cmake as well
    find_package(ssl CONFIG REQUIRED)
    set(cares_asio_tls_libraries AWS::ssl)
    message(STATUS "cares_asio: using TLS library AWS-LC (${ssl_DIR})")
else()
    message(FATAL_ERROR "CARES_ASIO_TLS is '${CARES_ASIO_TLS}', expected OpenSSL or AWS-LC")
endif()

# --------------------------------------------------------------------------------------------------
#
# The library. It is static unless BUILD_SHARED_LIBS says otherwise.
#
add_library(cares_asio)
add_library(cares_asio::cares_asio ALIAS cares_asio)

set(cares_asio_generated_include_dir "${CMAKE_CURRENT_BINARY_DIR}/include")

configure_file(cmake/version.hpp.in "${cares_asio_generated_include_dir}/cares_asio/version.hpp"
               @ONLY)

include(GenerateExportHeader)
generate_export_header(
    cares_asio
    BASE_NAME CARES_ASIO
    EXPORT_FILE_NAME "${cares_asio_generated_include_dir}/cares_asio/export.hpp")

target_sources(
    cares_asio
    PRIVATE src/log.hpp
            src/resolver.cpp
            src/server_list.cpp
            src/server_list.hpp
            src/tls_transport.cpp
            src/tls_transport.hpp
    PUBLIC FILE_SET HEADERS
           BASE_DIRS include "${cares_asio_generated_include_dir}"
           FILES include/cares_asio.hpp
                 include/cares_asio/error.hpp
                 include/cares_asio/resolver.hpp
                 "${cares_asio_generated_include_dir}/cares_asio/export.hpp"
                 "${cares_asio_generated_include_dir}/cares_asio/version.hpp")

target_compile_features(cares_asio PUBLIC cxx_std_20)

# our own sources stay clear of the deprecated ASIO interfaces
target_compile_definitions(cares_asio PRIVATE BOOST_ASIO_NO_DEPRECATED)

get_target_property(cares_asio_type cares_asio TYPE)
if (cares_asio_type STREQUAL "STATIC_LIBRARY")
    target_compile_definitions(cares_asio PUBLIC CARES_ASIO_STATIC_DEFINE)
endif()

#
# The status codes of c-ares are part of the interface, the TLS library is not. For a static
# library the private ones still end up on the link line of the consumer, which is why the
# package configuration file has to find them again.
#
target_link_libraries(
    cares_asio
    PUBLIC Boost::headers c-ares::cares
    PRIVATE ${cares_asio_tls_libraries} Threads::Threads)

#
# No promise of a stable ABI before 1.0, so the minor version is part of the soname until then.
#
set_target_properties(
    cares_asio
    PROPERTIES VERSION ${PROJECT_VERSION}
               SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
               CXX_EXTENSIONS OFF
               CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN ON
               VERIFY_INTERFACE_HEADER_SETS ${PROJECT_IS_TOP_LEVEL})

# --------------------------------------------------------------------------------------------------

if (CARES_ASIO_INSTALL)
    include(cmake/install.cmake)
endif()

if (CARES_ASIO_BUILD_EXAMPLES)
    add_subdirectory(example)
endif()

if (CARES_ASIO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
