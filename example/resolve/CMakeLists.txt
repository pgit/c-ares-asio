#
# The command line needs two libraries that cares_asio itself has no use for. Where they are
# missing it is this example that is left out, not the build that fails.
#
find_package(Boost ${CARES_ASIO_BOOST_VERSION} QUIET COMPONENTS program_options)
find_package(spdlog QUIET)

if (NOT TARGET Boost::program_options OR NOT TARGET spdlog::spdlog_header_only)
    message(STATUS "cares_asio: leaving out the 'resolve' example, which needs "
                   "Boost.ProgramOptions and spdlog")
    return()
endif()

add_executable(resolve main.cpp utils.cpp utils.hpp)
target_compile_features(resolve PRIVATE cxx_std_23)

# spdlog formats with <format>, there is no fmtlib in here
target_compile_definitions(resolve PRIVATE SPDLOG_USE_STD_FORMAT BOOST_ASIO_NO_DEPRECATED)

target_link_libraries(
    resolve PRIVATE cares_asio::cares_asio Boost::program_options spdlog::spdlog_header_only
                    Threads::Threads)
