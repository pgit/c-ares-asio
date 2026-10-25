#
# The shortest program that resolves a name, which is also the one the README shows. There is one
# for each runtime, as that is what the two spellings of the API are there to be looked at.
#
string(TOLOWER ${CARES_ASIO_API} cares_asio_api_dir)

add_executable(cares_asio_example_minimal ${cares_asio_api_dir}/minimal.cpp)
set_target_properties(cares_asio_example_minimal PROPERTIES OUTPUT_NAME minimal)
target_link_libraries(cares_asio_example_minimal PRIVATE cares_asio::cares_asio)

#
# The command line builds on either runtime: what it needs of its event loop is in a header of
# its own, one per runtime.
#
add_subdirectory(resolve)
