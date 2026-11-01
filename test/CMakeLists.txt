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
# Most of them are written in the spelling that both runtimes have (resolver::resolve(), see
# resolver_fixture.hpp) and are built on either. What is about one runtime only -- completion
# tokens here, stop tokens there -- is in asio/ or corosio/, next to the event loop fixture of
# that runtime, and only the one of this build is compiled. The fake DNS server is Boost.Asio's
# in both.
#
add_executable(
    c_ares_async_tests
    error_test.cpp
    event_loop_fixture.hpp
    fake_dns_server.cpp
    fake_dns_server.hpp
    https_test.cpp
    resolver_fixture.hpp
    resolver_test.cpp
    server_list_test.cpp
    test_certificate.cpp
    test_certificate.hpp
    tls_test.cpp)

if (C_ARES_ASYNC_COROSIO)
    target_sources(c_ares_async_tests PRIVATE corosio/event_loop_fixture.hpp corosio/stop_test.cpp)
else()
    target_sources(c_ares_async_tests PRIVATE asio/async_test.cpp asio/event_loop_fixture.hpp
                                              asio/run.hpp)
endif()

#
# What is not part of the interface is not exported from a shared library either, so the one
# internal piece with tests of its own is compiled into them.
#
target_sources(c_ares_async_tests PRIVATE ../src/server_list.cpp)
target_include_directories(c_ares_async_tests PRIVATE ../src)

target_compile_definitions(c_ares_async_tests PRIVATE BOOST_ASIO_NO_DEPRECATED)

#
# The fake DNS server is written in Boost.Asio whichever runtime the library was built on, so the
# tests of a COROSIO build need Boost as well -- the library itself does not.
#
if (C_ARES_ASYNC_COROSIO)
    find_package(Boost ${C_ARES_ASYNC_BOOST_VERSION} REQUIRED)
endif()

# the fake server speaks TLS as well, with the same library the resolver was built against
target_link_libraries(
    c_ares_async_tests PRIVATE c_ares_async::c_ares_async Boost::headers GTest::gmock
                               GTest::gtest_main ${c_ares_async_tls_libraries} Threads::Threads)

include(GoogleTest)
gtest_discover_tests(c_ares_async_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)

# --------------------------------------------------------------------------------------------------

#
# Every public header has to compile when it is the only one included.
#
add_test(NAME headers.self_contained
         COMMAND "${CMAKE_COMMAND}" --build "${PROJECT_BINARY_DIR}" --config $<CONFIG> --target
                 all_verify_interface_header_sets)

if (C_ARES_ASYNC_INSTALL)
    include(cmake_test.cmake)
endif()
