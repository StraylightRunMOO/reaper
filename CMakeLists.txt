# Reaper — CMake build
# A quad-color incremental, generational, non-moving garbage collector.
# Single-header C11 library; no runtime dependencies beyond libc.
#
# Typical usage:
#   cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   ctest --test-dir build --output-on-failure

cmake_minimum_required(VERSION 3.16)

project(reaper
    VERSION 1.0.0
    DESCRIPTION "Quad-color incremental, generational, non-moving GC for C11/C++17"
    HOMEPAGE_URL "https://github.com/StraylightRunMOO/reaper"
    LANGUAGES C CXX
)

# Interface library: consumers link StraylightRunMOO::reaper
add_library(reaper INTERFACE)
add_library(StraylightRunMOO::reaper ALIAS reaper)

target_include_directories(reaper INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# The library is C11; expose that requirement to downstream targets.
target_compile_features(reaper INTERFACE c_std_11)

# Build options: default ON when this is the top-level project.
option(REAPER_BUILD_TESTS       "Build the Reaper test suite"       "${PROJECT_IS_TOP_LEVEL}")
option(REAPER_BUILD_BENCHMARKS  "Build the Reaper benchmarks"       "${PROJECT_IS_TOP_LEVEL}")
option(REAPER_BUILD_EXAMPLES    "Build the Reaper example programs" "${PROJECT_IS_TOP_LEVEL}")

if(REAPER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(REAPER_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(REAPER_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ----------------------------------------------------------------------
# Installation / packaging
# ----------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS reaper
    EXPORT reaperTargets
)

install(FILES include/reaper.h include/reaper.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT reaperTargets
    FILE reaperTargets.cmake
    NAMESPACE StraylightRunMOO::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/reaper
)

include(CMakePackageConfigHelpers)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/reaperConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/reaperConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/reaper
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/reaperConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/reaperConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/reaperConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/reaper
)
