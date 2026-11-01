#
# The code that the programmer's guide shows is part of every build, as that is what keeps it true.
#
add_library(c_ares_async_doc_snippets OBJECT snippets.cpp)
target_link_libraries(c_ares_async_doc_snippets PRIVATE c_ares_async::c_ares_async)

#
# The 'docs' target: the guide and the reference, as HTML. Sphinx makes the pages, and Breathe gets
# what goes into the reference from Doxygen, which reads the comments in the public headers. It is
# not part of 'all', and not there at all where one of the two is not.
#
#    cmake --build build --target docs
#
# Doxygen does not compile the headers, so unlike the snippets the documentation depends on neither
# the runtime of the build nor on what that runtime needs. See Doxyfile.in.
#
find_program(DOXYGEN_EXECUTABLE doxygen DOC "Doxygen, which reads the comments in the headers")
find_program(SPHINX_EXECUTABLE sphinx-build DOC "Sphinx, which makes the documentation")

if (NOT DOXYGEN_EXECUTABLE OR NOT SPHINX_EXECUTABLE)
    message(STATUS "c_ares_async: no 'docs' target, as that takes both doxygen and sphinx-build")
    return()
endif()

configure_file(Doxyfile.in Doxyfile @ONLY)

set(C_ARES_ASYNC_DOCS_DIR "${CMAKE_CURRENT_BINARY_DIR}/html"
    CACHE PATH "Where the 'docs' target puts the documentation")

#
# With -E, as a warning in a page that a previous run has read already would not show otherwise:
# warnings are errors on both sides, which is where a public symbol without a comment shows, and a
# reference that goes nowhere.
#
add_custom_target(
    docs
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${C_ARES_ASYNC_DOCS_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}/doxygen"
    COMMAND "${DOXYGEN_EXECUTABLE}" "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
    COMMAND
        "${SPHINX_EXECUTABLE}" -q -E -W --keep-going -b html
        -d "${CMAKE_CURRENT_BINARY_DIR}/doctrees"
        -D "breathe_projects.c_ares_async=${CMAKE_CURRENT_BINARY_DIR}/doxygen/xml"
        -D "release=${PROJECT_VERSION}"
        -D "version=${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}"
        "${CMAKE_CURRENT_SOURCE_DIR}" "${C_ARES_ASYNC_DOCS_DIR}"
    COMMENT "Making the documentation in ${C_ARES_ASYNC_DOCS_DIR}"
    VERBATIM)
