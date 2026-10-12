#
# GoogleTest comes from wherever it is installed, and from its release archive where it is not.
# Nothing of it is installed along with the library either way.
#
include(FetchContent)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/releases/download/v1.18.0/googletest-1.18.0.tar.gz
    URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
    FIND_PACKAGE_ARGS NAMES GTest)

set(INSTALL_GTEST OFF)
FetchContent_MakeAvailable(googletest)

# --------------------------------------------------------------------------------------------------
#
# The tests ask a DNS server of their own on the loopback interface, see fake_dns_server.hpp, so
# they need no network and say the same wherever they run.
#
add_executable(
    cares_asio_tests
    async_test.cpp
    error_test.cpp
    fake_dns_server.cpp
    fake_dns_server.hpp
    https_test.cpp
    resolver_fixture.hpp
    resolver_test.cpp
    run.hpp
    server_list_test.cpp
    test_certificate.cpp
    test_certificate.hpp
    tls_test.cpp)

#
# What is not part of the interface is not exported from a shared library either, so the one
# internal piece with tests of its own is compiled into them.
#
target_sources(cares_asio_tests PRIVATE ../src/server_list.cpp)
target_include_directories(cares_asio_tests PRIVATE ../src)

target_compile_definitions(cares_asio_tests PRIVATE BOOST_ASIO_NO_DEPRECATED)

# the fake server speaks TLS as well, with the same library the resolver was built against
target_link_libraries(
    cares_asio_tests PRIVATE cares_asio::cares_asio GTest::gmock GTest::gtest_main
                             ${cares_asio_tls_libraries} Threads::Threads)

include(GoogleTest)
gtest_discover_tests(cares_asio_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)

# --------------------------------------------------------------------------------------------------

#
# Every public header has to compile when it is the only one included.
#
add_test(NAME headers.self_contained
         COMMAND "${CMAKE_COMMAND}" --build "${PROJECT_BINARY_DIR}" --config $<CONFIG> --target
                 all_verify_interface_header_sets)

if (CARES_ASIO_INSTALL)
    include(cmake_test.cmake)
endif()
