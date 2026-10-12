#
# Install rules, and the package that find_package(cares_asio) picks up afterwards.
#
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CARES_ASIO_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/cares_asio"
    CACHE STRING "Where to install the CMake package of cares_asio, relative to the prefix")

install(
    TARGETS cares_asio
    EXPORT cares_asio-targets
    RUNTIME COMPONENT cares_asio_runtime
    LIBRARY COMPONENT cares_asio_runtime NAMELINK_COMPONENT cares_asio_development
    ARCHIVE COMPONENT cares_asio_development
    FILE_SET HEADERS COMPONENT cares_asio_development)

install(
    EXPORT cares_asio-targets
    NAMESPACE cares_asio::
    DESTINATION "${CARES_ASIO_INSTALL_CMAKEDIR}"
    COMPONENT cares_asio_development)

configure_package_config_file(
    cmake/cares_asio-config.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/cares_asio-config.cmake"
    INSTALL_DESTINATION "${CARES_ASIO_INSTALL_CMAKEDIR}"
    NO_SET_AND_CHECK_MACRO)

#
# 0.2 may well break what 0.1 promised, so a consumer asking for 0.1 does not get 0.2.
#
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/cares_asio-config-version.cmake"
    COMPATIBILITY SameMinorVersion)

install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/cares_asio-config.cmake"
          "${CMAKE_CURRENT_BINARY_DIR}/cares_asio-config-version.cmake"
    DESTINATION "${CARES_ASIO_INSTALL_CMAKEDIR}"
    COMPONENT cares_asio_development)
