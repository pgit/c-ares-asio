cmake_minimum_required(VERSION 3.25...4.4)

project(
    c_ares_async
    VERSION 0.1.0
    DESCRIPTION "c-ares name resolution as asynchronous operations, on Boost.Asio or corosio"
    HOMEPAGE_URL "https://github.com/pgit/c-ares-async"
    LANGUAGES CXX)

if (WIN32)
    message(FATAL_ERROR "c_ares_async waits on its sockets as POSIX descriptors, which Windows "
                        "does not have.")
endif()

# there are no C++ modules in here, so there is nothing for CMake to scan the sources for
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# --------------------------------------------------------------------------------------------------
#
# Options. Tests, examples and documentation are the business of whoever works on the library
# itself, so they are only on by default where this is the top-level project -- not where it came
# in through add_subdirectory() or FetchContent.
#
# Nothing in here sets a compiler, a standard library or warning flags: that is up to the one who
# builds, and CMakePresets.json has what this repository is developed with.
#

#
# The I/O runtime, one per build tree: ASIO (completion tokens, on Boost.Asio) or COROSIO
# (coroutines only, on capy/corosio). The code tests C_ARES_ASYNC_ASIO or C_ARES_ASYNC_COROSIO,
# exactly one of which is 1, from the generated c_ares_async/config.hpp. See doc/corosio.md.
#
set(C_ARES_ASYNC_API "ASIO" CACHE STRING "I/O runtime to build on: ASIO or COROSIO")
set_property(CACHE C_ARES_ASYNC_API PROPERTY STRINGS ASIO COROSIO)

#
# capy and corosio are built from pinned sources as part of a COROSIO build and are not installed,
# so there is nothing an installed COROSIO build could be linked against.
#
if (C_ARES_ASYNC_API STREQUAL "COROSIO")
    set(c_ares_async_install_default OFF)
else()
    set(c_ares_async_install_default ${PROJECT_IS_TOP_LEVEL})
endif()

if (PROJECT_IS_TOP_LEVEL AND (NOT DEFINED BUILD_TESTING OR BUILD_TESTING))
    set(c_ares_async_tests_default ON)
else()
    set(c_ares_async_tests_default OFF)
endif()

option(C_ARES_ASYNC_BUILD_TESTS "Build the tests of c_ares_async" ${c_ares_async_tests_default})
option(C_ARES_ASYNC_BUILD_EXAMPLES "Build the examples of c_ares_async" ${PROJECT_IS_TOP_LEVEL})
option(C_ARES_ASYNC_BUILD_DOCS "Add the 'docs' target, where MrDocs is there to make them"
       ${PROJECT_IS_TOP_LEVEL})
option(C_ARES_ASYNC_INSTALL "Generate the install rules of c_ares_async"
       ${c_ares_async_install_default})

#
# c-ares has no DNS over TLS of its own -- see src/tls_transport.hpp. Either library will do: the
# transport sticks to the OpenSSL API that AWS-LC implements as well, so the choice is made here
# and nowhere else.
#
set(C_ARES_ASYNC_TLS "OpenSSL" CACHE STRING "TLS library to build against: OpenSSL or AWS-LC")
set_property(CACHE C_ARES_ASYNC_TLS PROPERTY STRINGS OpenSSL AWS-LC)

# --------------------------------------------------------------------------------------------------
#
# Dependencies. The versions are kept in variables because the package configuration file asks
# for the same ones again, on behalf of whoever links the installed library.
#
set(C_ARES_ASYNC_BOOST_VERSION 1.88)
set(C_ARES_ASYNC_CARES_VERSION 1.34) # ares_process_fds() was added in 1.34
set(C_ARES_ASYNC_OPENSSL_VERSION 3.2) # SSL_get0_group_name() was added in 3.2

find_package(Threads REQUIRED)
find_package(c-ares ${C_ARES_ASYNC_CARES_VERSION} CONFIG REQUIRED)

if (C_ARES_ASYNC_API STREQUAL "ASIO")
    set(C_ARES_ASYNC_ASIO 1)
    set(C_ARES_ASYNC_COROSIO 0)
    set(c_ares_async_runtime_libraries Boost::headers)
    find_package(Boost ${C_ARES_ASYNC_BOOST_VERSION} REQUIRED)
    message(STATUS "c_ares_async: using I/O runtime Boost.Asio ${Boost_VERSION}")
elseif (C_ARES_ASYNC_API STREQUAL "COROSIO")
    set(C_ARES_ASYNC_ASIO 0)
    set(C_ARES_ASYNC_COROSIO 1)
    set(c_ares_async_runtime_libraries Boost::corosio Boost::capy)

    #
    # Pinned: both are pre-release and change their API on 'develop' without notice, so moving a
    # pin is a commit of its own. They are built as part of this build, from their sources, and
    # are not installed along with the library -- a consumer of an installed COROSIO build needs
    # them in its own build.
    #
    include(FetchContent)
    foreach (option TESTS EXAMPLES BENCH)
        set(BOOST_CAPY_BUILD_${option} OFF CACHE BOOL "" FORCE)
        set(BOOST_COROSIO_BUILD_${option} OFF CACHE BOOL "" FORCE)
    endforeach()
    FetchContent_Declare(
        capy
        GIT_REPOSITORY https://github.com/cppalliance/capy.git
        GIT_TAG 24c54af2cf967ccbf8d3809af4bcd9c8f61f553b
        EXCLUDE_FROM_ALL)
    FetchContent_Declare(
        corosio
        GIT_REPOSITORY https://github.com/cppalliance/corosio.git
        GIT_TAG 41d0de18b6e4fa1b397c38a866f2daf52c6c36b5
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(capy corosio)
    message(STATUS "c_ares_async: using I/O runtime capy/corosio")
else()
    message(FATAL_ERROR "C_ARES_ASYNC_API is '${C_ARES_ASYNC_API}', expected ASIO or COROSIO")
endif()

#
# https://github.com/c-ares/c-ares/security/advisories/GHSA-6wfj-rwm7-3542 is a use-after-free in
# the completion of a query, which an application calling ares_cancel() can trigger -- and
# cancelling an operation of the resolver does exactly that.
#
if (c-ares_VERSION VERSION_LESS 1.34.7)
    message(WARNING "c-ares ${c-ares_VERSION} is affected by GHSA-6wfj-rwm7-3542, a use-after-free "
                    "that cancelling a lookup can trigger. It is fixed in c-ares 1.34.7.")
endif()

if (C_ARES_ASYNC_TLS STREQUAL "OpenSSL")
    find_package(OpenSSL ${C_ARES_ASYNC_OPENSSL_VERSION} REQUIRED)
    set(c_ares_async_tls_libraries OpenSSL::SSL OpenSSL::Crypto)
    message(STATUS "c_ares_async: using TLS library OpenSSL ${OPENSSL_VERSION}")
elseif (C_ARES_ASYNC_TLS STREQUAL "AWS-LC")
    # ssl_ROOT says where it is installed, and finds 'crypto' for ssl-config.cmake as well
    find_package(ssl CONFIG REQUIRED)
    set(c_ares_async_tls_libraries AWS::ssl)
    message(STATUS "c_ares_async: using TLS library AWS-LC (${ssl_DIR})")
else()
    message(FATAL_ERROR "C_ARES_ASYNC_TLS is '${C_ARES_ASYNC_TLS}', expected OpenSSL or AWS-LC")
endif()

# --------------------------------------------------------------------------------------------------
#
# The library. It is static unless BUILD_SHARED_LIBS says otherwise.
#
add_library(c_ares_async)
add_library(c_ares_async::c_ares_async ALIAS c_ares_async)

set(c_ares_async_generated_include_dir "${CMAKE_CURRENT_BINARY_DIR}/include")

configure_file(cmake/version.hpp.in "${c_ares_async_generated_include_dir}/c_ares_async/version.hpp"
               @ONLY)

configure_file(cmake/config.hpp.in "${c_ares_async_generated_include_dir}/c_ares_async/config.hpp"
               @ONLY)

include(GenerateExportHeader)
generate_export_header(
    c_ares_async
    BASE_NAME C_ARES_ASYNC
    EXPORT_FILE_NAME "${c_ares_async_generated_include_dir}/c_ares_async/export.hpp")

#
# What belongs to one runtime only lives in asio/ or corosio/, under src/ as well as under
# include/c_ares_async/, and only the one of this build is compiled or installed.
#
string(TOLOWER ${C_ARES_ASYNC_API} c_ares_async_api_dir)

target_sources(
    c_ares_async
    PRIVATE src/${c_ares_async_api_dir}/event_loop.hpp
            src/event_loop.hpp
            src/log.hpp
            src/resolver.cpp
            src/server_list.cpp
            src/server_list.hpp
            src/socket_functions.cpp
            src/socket_functions.hpp
            src/tls_transport.cpp
            src/tls_transport.hpp
    PUBLIC FILE_SET HEADERS
           BASE_DIRS include "${c_ares_async_generated_include_dir}"
           FILES include/c_ares_async.hpp
                 include/c_ares_async/${c_ares_async_api_dir}/runtime.hpp
                 include/c_ares_async/error.hpp
                 include/c_ares_async/resolver.hpp
                 include/c_ares_async/runtime.hpp
                 "${c_ares_async_generated_include_dir}/c_ares_async/config.hpp"
                 "${c_ares_async_generated_include_dir}/c_ares_async/export.hpp"
                 "${c_ares_async_generated_include_dir}/c_ares_async/version.hpp")

target_compile_features(c_ares_async PUBLIC cxx_std_20)

# our own sources stay clear of the deprecated ASIO interfaces
if (C_ARES_ASYNC_ASIO)
    target_compile_definitions(c_ares_async PRIVATE BOOST_ASIO_NO_DEPRECATED)
endif()

get_target_property(c_ares_async_type c_ares_async TYPE)
if (c_ares_async_type STREQUAL "STATIC_LIBRARY")
    target_compile_definitions(c_ares_async PUBLIC C_ARES_ASYNC_STATIC_DEFINE)
endif()

#
# The status codes of c-ares are part of the interface, the TLS library is not. For a static
# library the private ones still end up on the link line of the consumer, which is why the
# package configuration file has to find them again.
#
target_link_libraries(
    c_ares_async
    PUBLIC ${c_ares_async_runtime_libraries} c-ares::cares
    PRIVATE ${c_ares_async_tls_libraries} Threads::Threads)

#
# No promise of a stable ABI before 1.0, so the minor version is part of the soname until then.
#
set_target_properties(
    c_ares_async
    PROPERTIES VERSION ${PROJECT_VERSION}
               SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
               CXX_EXTENSIONS OFF
               CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN ON
               VERIFY_INTERFACE_HEADER_SETS ${PROJECT_IS_TOP_LEVEL})

# --------------------------------------------------------------------------------------------------

if (C_ARES_ASYNC_INSTALL)
    if (C_ARES_ASYNC_COROSIO)
        message(FATAL_ERROR "C_ARES_ASYNC_INSTALL does not work with C_ARES_ASYNC_API=COROSIO: "
                            "capy and corosio are built from pinned sources here and not "
                            "installed, so the exported target would point at libraries that are "
                            "not there.")
    endif()

    include(cmake/install.cmake)
endif()

if (C_ARES_ASYNC_BUILD_EXAMPLES)
    add_subdirectory(example)
endif()

if (C_ARES_ASYNC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

if (C_ARES_ASYNC_BUILD_DOCS)
    add_subdirectory(doc)
endif()
