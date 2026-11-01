#
# The library the way a consumer gets it: test/cmake_test is a project of its own that is
# configured, built and run from scratch, once against an installed c_ares_async that it finds with
# find_package() and once with the sources pulled in through add_subdirectory(). Neither sees
# anything of this build but what a consumer would.
#
set(cmake_test_source_dir "${CMAKE_CURRENT_SOURCE_DIR}/cmake_test")
set(cmake_test_binary_dir "${CMAKE_CURRENT_BINARY_DIR}/cmake_test")
set(cmake_test_prefix "${cmake_test_binary_dir}/prefix")

#
# The consumer has to be built with the same toolchain and find the same dependencies. That goes
# through an initial cache rather than the command line, where lists and flags with spaces in them
# would need quoting twice over.
#
set(cmake_test_cache "${cmake_test_binary_dir}/initial-cache.cmake")
set(cmake_test_cache_content "")
foreach (variable IN ITEMS
         CMAKE_BUILD_TYPE
         CMAKE_CXX_COMPILER
         CMAKE_CXX_FLAGS
         CMAKE_EXE_LINKER_FLAGS
         CMAKE_SHARED_LINKER_FLAGS
         CMAKE_PREFIX_PATH
         CMAKE_TOOLCHAIN_FILE
         BUILD_SHARED_LIBS
         C_ARES_ASYNC_TLS
         Boost_DIR
         c-ares_DIR
         OPENSSL_ROOT_DIR
         ssl_DIR
         crypto_DIR)
    if (DEFINED ${variable})
        string(APPEND cmake_test_cache_content
               "set(${variable} [==[${${variable}}]==] CACHE STRING \"\")\n")
    endif()
endforeach()
file(GENERATE OUTPUT "${cmake_test_cache}" CONTENT "${cmake_test_cache_content}")

add_test(NAME cmake_test.install
         COMMAND "${CMAKE_COMMAND}" --install "${PROJECT_BINARY_DIR}" --prefix
                 "${cmake_test_prefix}" --config $<CONFIG>)
set_tests_properties(cmake_test.install PROPERTIES FIXTURES_SETUP c_ares_async_installed)

foreach (how IN ITEMS find_package add_subdirectory)
    add_test(
        NAME cmake_test.${how}
        COMMAND
            "${CMAKE_CTEST_COMMAND}" --build-and-test "${cmake_test_source_dir}"
            "${cmake_test_binary_dir}/${how}" --build-generator "${CMAKE_GENERATOR}"
            --build-options -C "${cmake_test_cache}" "-DC_ARES_ASYNC_CONSUME=${how}"
            "-Dc_ares_async_ROOT=${cmake_test_prefix}" --test-command "${CMAKE_CTEST_COMMAND}"
            --output-on-failure)
    set_tests_properties(cmake_test.${how} PROPERTIES LABELS cmake TIMEOUT 600)
endforeach()

set_tests_properties(cmake_test.install PROPERTIES LABELS cmake)
set_tests_properties(cmake_test.find_package PROPERTIES FIXTURES_REQUIRED c_ares_async_installed)
