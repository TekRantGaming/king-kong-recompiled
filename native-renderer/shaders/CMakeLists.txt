# King Kong native renderer: shader translator (Xbox 360 microcode to HLSL, DXIL and SPIR-V).
#
# Standalone: it does not need the game to be built. It uses the headers the rexglue
# bundle ships (fmt header-only, xxhash header-only, dxcapi.h) and loads the DirectX
# Shader Compiler at run time. SPIR-V comes from glslang built from the SDK's sources
# when KKSHADERS_GLSLANG_DIR points at them (optional).

cmake_minimum_required(VERSION 3.25)
project(kkshaders LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The rexglue bundle: fmt, xxhash and dxcapi headers.
set(KKSHADERS_REXGLUE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/rexglue/win-amd64" CACHE PATH
    "rexglue bundle (include/fmt, include/xxh3.h, include/dxc/dxcapi.h)")
if(NOT EXISTS "${KKSHADERS_REXGLUE_DIR}/include/dxc/dxcapi.h")
    message(FATAL_ERROR "rexglue bundle not found at ${KKSHADERS_REXGLUE_DIR} (set KKSHADERS_REXGLUE_DIR)")
endif()

# Optional SPIR-V back end: glslang (HLSL front end) plus SPIRV-Tools (legalisation and
# validation), built from the ReXGlue SDK's third-party sources. Empty = no SPIR-V.
set(KKSHADERS_GLSLANG_DIR "" CACHE PATH "glslang source tree (for example C:/rexsrc/thirdparty/glslang)")
set(KKSHADERS_SPIRV_TOOLS_DIR "" CACHE PATH "SPIRV-Tools source tree (optional, needs KKSHADERS_SPIRV_HEADERS_DIR)")
set(KKSHADERS_SPIRV_HEADERS_DIR "" CACHE PATH "SPIRV-Headers source tree")

add_library(kkshaders_deps INTERFACE)
target_include_directories(kkshaders_deps INTERFACE
    "${KKSHADERS_REXGLUE_DIR}/include"
    "${KKSHADERS_REXGLUE_DIR}/include/dxc")
target_compile_definitions(kkshaders_deps INTERFACE FMT_HEADER_ONLY=1 XXH_INLINE_ALL=1 NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

set(KKSHADERS_HAVE_SPIRV OFF)
if(KKSHADERS_GLSLANG_DIR)
    if(KKSHADERS_SPIRV_TOOLS_DIR AND KKSHADERS_SPIRV_HEADERS_DIR)
        set(SPIRV_HEADERS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
        set(SPIRV_HEADERS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        add_subdirectory("${KKSHADERS_SPIRV_HEADERS_DIR}" spirv-headers EXCLUDE_FROM_ALL)
        set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
        set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
        set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
        set(SPIRV-Headers_SOURCE_DIR "${KKSHADERS_SPIRV_HEADERS_DIR}" CACHE PATH "" FORCE)
        add_subdirectory("${KKSHADERS_SPIRV_TOOLS_DIR}" spirv-tools EXCLUDE_FROM_ALL)
        set(ENABLE_OPT ON CACHE BOOL "" FORCE)
        set(KKSHADERS_HAVE_SPIRV_TOOLS ON)
    else()
        set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
        set(KKSHADERS_HAVE_SPIRV_TOOLS OFF)
    endif()
    set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
    set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
    set(ENABLE_HLSL ON CACHE BOOL "" FORCE)
    set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(SKIP_GLSLANG_INSTALL ON CACHE BOOL "" FORCE)
    set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
    set(ENABLE_RTTI ON CACHE BOOL "" FORCE)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
    add_subdirectory("${KKSHADERS_GLSLANG_DIR}" glslang EXCLUDE_FROM_ALL)
    set(KKSHADERS_HAVE_SPIRV ON)
endif()

# The library: container adapter, translator, compile step, cache and pack.
add_library(kkshaders STATIC
    src/container.cpp
    src/translator.cpp
    src/compiler_dxc.cpp
    src/cache.cpp
    src/hlsl_prelude.cpp)
target_include_directories(kkshaders PUBLIC include PRIVATE src)
target_link_libraries(kkshaders PUBLIC kkshaders_deps)
if(KKSHADERS_HAVE_SPIRV)
    target_sources(kkshaders PRIVATE src/compiler_glslang.cpp)
    target_compile_definitions(kkshaders PUBLIC KKSHADERS_HAVE_SPIRV=1)
    target_link_libraries(kkshaders PRIVATE glslang SPIRV)
    target_include_directories(kkshaders PRIVATE "${KKSHADERS_GLSLANG_DIR}")
    if(KKSHADERS_HAVE_SPIRV_TOOLS)
        target_compile_definitions(kkshaders PRIVATE KKSHADERS_HAVE_SPIRV_TOOLS=1)
        target_link_libraries(kkshaders PRIVATE SPIRV-Tools-static)
        target_include_directories(kkshaders PRIVATE "${KKSHADERS_SPIRV_TOOLS_DIR}/include")
    endif()
endif()
if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(kkshaders PRIVATE /W3)
else()
    target_compile_options(kkshaders PRIVATE -Wall -Wno-unused-function -Wno-missing-braces)
endif()

# The command-line tool.
add_executable(kkshaders_tool tool/main.cpp)
set_target_properties(kkshaders_tool PROPERTIES OUTPUT_NAME kkshaders)
target_link_libraries(kkshaders_tool PRIVATE kkshaders)

# Tests (ctest): they need the shader database, given by KKSHADERS_DATABASE.
set(KKSHADERS_DATABASE "${CMAKE_CURRENT_SOURCE_DIR}/../../kk/assets/Shaders/xeshaders.bin" CACHE FILEPATH "xeshaders.bin for the tests")
enable_testing()
add_executable(kkshaders_tests tests/tests.cpp)
target_link_libraries(kkshaders_tests PRIVATE kkshaders)
add_test(NAME container COMMAND kkshaders_tests container "${KKSHADERS_DATABASE}")
add_test(NAME translate COMMAND kkshaders_tests translate "${KKSHADERS_DATABASE}")
add_test(NAME pack COMMAND kkshaders_tests pack "${KKSHADERS_DATABASE}" "${CMAKE_CURRENT_BINARY_DIR}/test-pack")
