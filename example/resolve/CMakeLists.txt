#
# The command line needs two libraries that c_ares_async itself has no use for. Where they are
# missing it is this example that is left out, not the build that fails.
#
find_package(Boost ${C_ARES_ASYNC_BOOST_VERSION} QUIET COMPONENTS program_options)
find_package(spdlog QUIET)

if (NOT TARGET Boost::program_options OR NOT TARGET spdlog::spdlog_header_only)
    message(STATUS "c_ares_async: leaving out the 'resolve' example, which needs "
                   "Boost.ProgramOptions and spdlog")
    return()
endif()

#
# What the program needs of its event loop is in runtime.hpp, one file per runtime, so that the
# program itself is the same on either -- see ../../doc/corosio.md.
#
string(TOLOWER ${C_ARES_ASYNC_API} c_ares_async_api_dir)

add_executable(resolve main.cpp runtime.hpp utils.hpp ${c_ares_async_api_dir}/runtime.hpp)
target_compile_features(resolve PRIVATE cxx_std_23)

# spdlog formats with <format>, there is no fmtlib in here
target_compile_definitions(resolve PRIVATE SPDLOG_USE_STD_FORMAT)

if (C_ARES_ASYNC_ASIO)
    target_compile_definitions(resolve PRIVATE BOOST_ASIO_NO_DEPRECATED)
endif()

target_link_libraries(
    resolve PRIVATE c_ares_async::c_ares_async Boost::program_options spdlog::spdlog_header_only
                    Threads::Threads)
