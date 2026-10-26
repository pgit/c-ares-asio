#
# Install rules, and the package that find_package(cares_async) picks up afterwards.
#
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CARES_ASYNC_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/cares_async"
    CACHE STRING "Where to install the CMake package of cares_async, relative to the prefix")

install(
    TARGETS cares_async
    EXPORT cares_async-targets
    RUNTIME COMPONENT cares_async_runtime
    LIBRARY COMPONENT cares_async_runtime NAMELINK_COMPONENT cares_async_development
    ARCHIVE COMPONENT cares_async_development
    FILE_SET HEADERS COMPONENT cares_async_development)

install(
    EXPORT cares_async-targets
    NAMESPACE cares_async::
    DESTINATION "${CARES_ASYNC_INSTALL_CMAKEDIR}"
    COMPONENT cares_async_development)

configure_package_config_file(
    cmake/cares_async-config.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/cares_async-config.cmake"
    INSTALL_DESTINATION "${CARES_ASYNC_INSTALL_CMAKEDIR}"
    NO_SET_AND_CHECK_MACRO)

#
# 0.2 may well break what 0.1 promised, so a consumer asking for 0.1 does not get 0.2.
#
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/cares_async-config-version.cmake"
    COMPATIBILITY SameMinorVersion)

install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/cares_async-config.cmake"
          "${CMAKE_CURRENT_BINARY_DIR}/cares_async-config-version.cmake"
    DESTINATION "${CARES_ASYNC_INSTALL_CMAKEDIR}"
    COMPONENT cares_async_development)

install(
    FILES LICENSE_1_0.txt
    DESTINATION "${CMAKE_INSTALL_DOCDIR}"
    COMPONENT cares_async_runtime)
