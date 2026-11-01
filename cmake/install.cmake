#
# Install rules, and the package that find_package(c_ares_async) picks up afterwards.
#
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(C_ARES_ASYNC_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/c_ares_async"
    CACHE STRING "Where to install the CMake package of c_ares_async, relative to the prefix")

install(
    TARGETS c_ares_async
    EXPORT c_ares_async-targets
    RUNTIME COMPONENT c_ares_async_runtime
    LIBRARY COMPONENT c_ares_async_runtime NAMELINK_COMPONENT c_ares_async_development
    ARCHIVE COMPONENT c_ares_async_development
    FILE_SET HEADERS COMPONENT c_ares_async_development)

install(
    EXPORT c_ares_async-targets
    NAMESPACE c_ares_async::
    DESTINATION "${C_ARES_ASYNC_INSTALL_CMAKEDIR}"
    COMPONENT c_ares_async_development)

configure_package_config_file(
    cmake/c_ares_async-config.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/c_ares_async-config.cmake"
    INSTALL_DESTINATION "${C_ARES_ASYNC_INSTALL_CMAKEDIR}"
    NO_SET_AND_CHECK_MACRO)

#
# 0.2 may well break what 0.1 promised, so a consumer asking for 0.1 does not get 0.2.
#
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/c_ares_async-config-version.cmake"
    COMPATIBILITY SameMinorVersion)

install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/c_ares_async-config.cmake"
          "${CMAKE_CURRENT_BINARY_DIR}/c_ares_async-config-version.cmake"
    DESTINATION "${C_ARES_ASYNC_INSTALL_CMAKEDIR}"
    COMPONENT c_ares_async_development)

install(
    FILES LICENSE_1_0.txt
    DESTINATION "${CMAKE_INSTALL_DOCDIR}"
    COMPONENT c_ares_async_runtime)
