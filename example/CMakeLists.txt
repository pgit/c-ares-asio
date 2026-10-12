#
# The shortest program that resolves a name, which is also the one the README shows.
#
add_executable(cares_asio_example_minimal minimal.cpp)
set_target_properties(cares_asio_example_minimal PROPERTIES OUTPUT_NAME minimal)
target_link_libraries(cares_asio_example_minimal PRIVATE cares_asio::cares_asio)

add_subdirectory(resolve)
