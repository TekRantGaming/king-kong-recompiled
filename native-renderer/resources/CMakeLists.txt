# King Kong native renderer: guest resource conversion (brief 03).
# A standalone library with unit tests that need neither the game nor the SDK:
#   build.bat            configure, build and run the tests (Visual Studio 2022 clang + Ninja, like kk/)
#   kknr_resources       the library (texture formats, tiling, conversion, buffers, caches, render targets)
#   kknr_resources_tests the unit tests (ctest, or run the exe)
#   kknr_texdump         converts a texture dump (KK_DEV_TEX_DUMP) to host data, DDS and PNG (dev tool)
#   kknr_bfscan          lists the texture formats stored in a KKTextures.bf (dev tool, reads game data)
cmake_minimum_required(VERSION 3.25)
project(kknr_resources LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(kknr_resources STATIC
    src/xenos.cpp
    src/tiling.cpp
    src/guest_texture.cpp
    src/endian.cpp
    src/texture_convert.cpp
    src/buffers.cpp
    src/texture_cache.cpp
    src/render_targets.cpp
)
target_include_directories(kknr_resources PUBLIC include)
if(MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(kknr_resources PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif()

option(KKNR_RESOURCES_TOOLS "Build the developer tools" ON)
if(KKNR_RESOURCES_TOOLS)
    add_executable(kknr_texdump tools/texdump.cpp tools/image_io.cpp)
    target_link_libraries(kknr_texdump PRIVATE kknr_resources)
    add_executable(kknr_bfscan tools/bfscan.cpp tools/image_io.cpp)
    target_link_libraries(kknr_bfscan PRIVATE kknr_resources)
endif()

enable_testing()
add_executable(kknr_resources_tests
    tests/test_main.cpp
    tests/test_tiling.cpp
    tests/test_convert.cpp
    tests/test_buffers.cpp
    tests/test_cache.cpp
    tests/test_render_targets.cpp
)
target_link_libraries(kknr_resources_tests PRIVATE kknr_resources)
add_test(NAME kknr_resources_tests COMMAND kknr_resources_tests)
