cmake_minimum_required(VERSION 3.30...4.4)

project(wlpuppet
    VERSION 0.1.0
    DESCRIPTION "Automated testing and remote control of Wayland clients"
    LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CMakeDependentOption)

option(WLPUPPET_BUILD_TESTS "Build the unit and integration tests" ${PROJECT_IS_TOP_LEVEL})
option(WLPUPPET_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# Language standards are requested per target through compile features
# (cxx_std_23 / c_std_23); these only stop CMake from silently decaying to an
# older standard or enabling GNU extensions.
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_EXTENSIONS OFF)
# No C++20 modules here; skip the dependency scanning pass.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

include(WlpuppetWarnings)
include(WlpuppetDependencies)
include(WlpuppetProtocols)

add_subdirectory(lib/core)
add_subdirectory(lib/mcp)
add_subdirectory(apps/wlpuppet-mcp)

if(WLPUPPET_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
