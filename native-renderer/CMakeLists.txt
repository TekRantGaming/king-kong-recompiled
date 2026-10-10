# The native renderer: backend libraries, the D3D hook table, the
# rexgpu-native plugin and their tests.
#
# Two ways in:
# - Standalone (Linux cloud build, or any machine with the SDK bundle):
#     cmake -S native-renderer -B build -G Ninja -DCMAKE_PREFIX_PATH=<sdk bundle>
#   builds the libraries, the plugin (when rexglue is found) and the tests.
# - From kk/CMakeLists.txt after find_package(rexglue): add_subdirectory on
#   this folder with NR_BUILD_TESTS OFF builds only the plugin next to
#   king_kong.exe (see plugin/CMakeLists.txt).
cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(kk_native_renderer LANGUAGES C CXX)
    set(NR_STANDALONE ON)
else()
    set(NR_STANDALONE OFF)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# The static libraries end up inside the plugin (a shared library).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(NR_BUILD_TESTS "Build the native renderer tests" ${NR_STANDALONE})
option(NR_BUILD_PLUGIN "Build rexgpu-native (needs the ReXGlue SDK)" ON)

set(NR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

if(NR_BUILD_PLUGIN AND NOT TARGET rex::runtime)
    find_package(rexglue CONFIG QUIET)
    if(NOT TARGET rex::runtime)
        message(STATUS "native-renderer: ReXGlue SDK not found, the plugin is skipped")
        set(NR_BUILD_PLUGIN OFF)
    endif()
endif()

include(cmake/nvrhi.cmake)
include(cmake/kk_libs.cmake)

add_subdirectory(backend)
add_subdirectory(hooks)
if(NR_BUILD_PLUGIN)
    add_subdirectory(plugin)
endif()

if(NR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(backend/tests)
endif()
