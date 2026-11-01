#
# The shortest program that resolves a name, which is also the one the README shows. There is one
# for each runtime, as that is what the two spellings of the API are there to be looked at.
#
string(TOLOWER ${CARES_ASYNC_API} cares_async_api_dir)

add_executable(cares_async_example_minimal ${cares_async_api_dir}/minimal.cpp)
set_target_properties(cares_async_example_minimal PROPERTIES OUTPUT_NAME minimal)
target_link_libraries(cares_async_example_minimal PRIVATE cares_async::cares_async)

#
# The command line builds on either runtime: what it needs of its event loop is in a header of
# its own, one per runtime.
#
add_subdirectory(resolve)
