#
# The 'docs' target: the reference documentation, as HTML, made by MrDocs from the comments in the
# public headers. It is not part of 'all', and not there at all where MrDocs is not.
#
#    cmake --build build --target docs
#
find_program(MRDOCS_EXECUTABLE mrdocs DOC "MrDocs, which makes the reference documentation")

if (NOT MRDOCS_EXECUTABLE)
    message(STATUS "cares_asio: no 'docs' target, as there is no mrdocs to make them with")
    return()
endif()

#
# The reference comes from an ASIO build. The comments it is made of are the same in a COROSIO
# one -- what differs are the type aliases of the runtime layer, which cares_asio/asio/runtime.hpp
# documents for both -- and the include directories of capy and corosio are build-interface
# generator expressions, which the compilation database below cannot be written with.
#
if (CARES_ASIO_COROSIO)
    message(STATUS "cares_asio: no 'docs' target in a COROSIO build, configure an ASIO one for it")
    return()
endif()

#
# MrDocs reads the headers the way a compiler does, and learns what a compiler is told from a
# compilation database. The one of the build will not do: it has the tests and the examples in it
# as well, which are nothing to document and take libraries the headers have no use for. So here
# is one with a single entry, for a source file that includes all there is.
#
# Directories that the compiler searches anyway are left out, as naming them again changes the
# order they are searched in -- which the standard library does not take well.
#
get_target_property(mrdocs_boost_includes Boost::headers INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(mrdocs_cares_includes c-ares::cares INTERFACE_INCLUDE_DIRECTORIES)

set(mrdocs_includes "${PROJECT_SOURCE_DIR}/include" "${cares_asio_generated_include_dir}"
                    ${mrdocs_boost_includes} ${mrdocs_cares_includes})
list(REMOVE_DUPLICATES mrdocs_includes)
if (CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
    list(REMOVE_ITEM mrdocs_includes ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES})
endif()

set(mrdocs_source "${CMAKE_CURRENT_SOURCE_DIR}/mrdocs.cpp")
set(mrdocs_arguments "${CMAKE_CXX_COMPILER}" -std=c++20 -DCARES_ASIO_STATIC_DEFINE)
foreach (directory IN LISTS mrdocs_includes)
    list(APPEND mrdocs_arguments "-I${directory}")
endforeach()
list(APPEND mrdocs_arguments -c "${mrdocs_source}")

list(JOIN mrdocs_arguments "\",\n      \"" mrdocs_arguments)
file(
    WRITE "${CMAKE_CURRENT_BINARY_DIR}/compile_commands.json"
    "[\n"
    "  {\n"
    "    \"directory\": \"${CMAKE_CURRENT_BINARY_DIR}\",\n"
    "    \"file\": \"${mrdocs_source}\",\n"
    "    \"arguments\": [\n      \"${mrdocs_arguments}\"\n    ]\n"
    "  }\n"
    "]\n")

set(CARES_ASIO_DOCS_DIR "${CMAKE_CURRENT_BINARY_DIR}/html"
    CACHE PATH "Where the 'docs' target puts the reference documentation")

add_custom_target(
    docs
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${CARES_ASIO_DOCS_DIR}"
    COMMAND
        "${MRDOCS_EXECUTABLE}" "--config=${CMAKE_CURRENT_SOURCE_DIR}/mrdocs.yml"
        "--compilation-database=${CMAKE_CURRENT_BINARY_DIR}/compile_commands.json"
        "--output=${CARES_ASIO_DOCS_DIR}"
    COMMENT "Making the reference documentation in ${CARES_ASIO_DOCS_DIR}"
    VERBATIM)
