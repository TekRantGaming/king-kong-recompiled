# King Kong native renderer: shader translator (Xbox 360 microcode to HLSL, DXIL and SPIR-V).
#
# Standalone: it does not need the game. Inputs:
#   KKSHADERS_SDK_DIR      a ReXGlue SDK source clone (fmt and xxHash from its thirdparty folder,
#                          header-only), or
#   KKSHADERS_REXGLUE_DIR  the rexglue binary bundle (include/fmt, include/xxh3.h)
#   KKSHADERS_DXC_DIR      a DirectX Shader Compiler release (include/dxc/dxcapi.h; the compiler
#                          library is loaded at run time from here unless the tools are told
#                          otherwise). On Windows the SDK's thirdparty/dxc headers also work.
#   KKSHADERS_SPIRV_VAL    spirv-val, for the tests (found on PATH or in the Vulkan SDK)
#   KKSHADERS_DATABASE     the game's Shaders/xeshaders.bin for the database tests (optional)
cmake_minimum_required(VERSION 3.20)
project(kkshaders LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(KKSHADERS_SDK_DIR "" CACHE PATH "ReXGlue SDK source tree (thirdparty/fmt, thirdparty/xxHash, thirdparty/dxc)")
set(KKSHADERS_REXGLUE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/rexglue/win-amd64" CACHE PATH "rexglue bundle (include/fmt, include/xxh3.h)")
set(KKSHADERS_DXC_DIR "" CACHE PATH "DirectX Shader Compiler release (include/dxc, bin or lib)")
set(KKSHADERS_DATABASE "${CMAKE_CURRENT_SOURCE_DIR}/../../kk/assets/Shaders/xeshaders.bin" CACHE FILEPATH "xeshaders.bin for the database tests")

# Third-party headers: fmt (header-only), xxHash (inline), dxcapi.h.
set(KK_FMT_INCLUDE "")
set(KK_XXHASH_INCLUDE "")
set(KK_DXC_INCLUDE "")
if(KKSHADERS_SDK_DIR)
    set(KK_FMT_INCLUDE "${KKSHADERS_SDK_DIR}/thirdparty/fmt/include")
    set(KK_XXHASH_INCLUDE "${KKSHADERS_SDK_DIR}/thirdparty/xxHash")
    if(WIN32)
        set(KK_DXC_INCLUDE "${KKSHADERS_SDK_DIR}/thirdparty/dxc/include")
    endif()
elseif(EXISTS "${KKSHADERS_REXGLUE_DIR}/include/xxh3.h")
    set(KK_FMT_INCLUDE "${KKSHADERS_REXGLUE_DIR}/include")
    set(KK_XXHASH_INCLUDE "${KKSHADERS_REXGLUE_DIR}/include")
    set(KK_DXC_INCLUDE "${KKSHADERS_REXGLUE_DIR}/include/dxc")
endif()
if(KKSHADERS_DXC_DIR AND EXISTS "${KKSHADERS_DXC_DIR}/include/dxc/dxcapi.h")
    set(KK_DXC_INCLUDE "${KKSHADERS_DXC_DIR}/include/dxc")   # Linux release
elseif(KKSHADERS_DXC_DIR AND EXISTS "${KKSHADERS_DXC_DIR}/inc/dxcapi.h")
    set(KK_DXC_INCLUDE "${KKSHADERS_DXC_DIR}/inc")           # Windows release
endif()
if(NOT EXISTS "${KK_FMT_INCLUDE}/fmt/core.h")
    message(FATAL_ERROR "fmt not found: set KKSHADERS_SDK_DIR (SDK clone) or KKSHADERS_REXGLUE_DIR")
endif()
if(NOT EXISTS "${KK_XXHASH_INCLUDE}/xxh3.h")
    message(FATAL_ERROR "xxHash not found: set KKSHADERS_SDK_DIR (SDK clone) or KKSHADERS_REXGLUE_DIR")
endif()
if(NOT EXISTS "${KK_DXC_INCLUDE}/dxcapi.h")
    message(FATAL_ERROR "dxcapi.h not found: set KKSHADERS_DXC_DIR to a DXC release")
endif()

add_library(kkshaders_deps INTERFACE)
target_include_directories(kkshaders_deps INTERFACE "${KK_FMT_INCLUDE}" "${KK_XXHASH_INCLUDE}")
target_compile_definitions(kkshaders_deps INTERFACE FMT_HEADER_ONLY=1 XXH_INLINE_ALL=1)
if(WIN32)
    target_compile_definitions(kkshaders_deps INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

# The translation core: XenosRecomp (vendored, MIT), reworked for this game.
set(XENOSRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/XenosRecomp/XenosRecomp")
add_library(xenosrecomp_core STATIC "${XENOSRECOMP_DIR}/shader_recompiler.cpp")
target_include_directories(xenosrecomp_core PUBLIC "${XENOSRECOMP_DIR}")
target_link_libraries(xenosrecomp_core PUBLIC kkshaders_deps)
target_precompile_headers(xenosrecomp_core PRIVATE "${XENOSRECOMP_DIR}/pch.h")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(xenosrecomp_core PRIVATE -Wno-switch -Wno-unused-variable)
endif()

# The prelude (hlsl/kk_common.hlsli) embedded as a byte array.
set(KK_PRELUDE "${CMAKE_CURRENT_SOURCE_DIR}/hlsl/kk_common.hlsli")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KK_PRELUDE}")
file(READ "${KK_PRELUDE}" KK_PRELUDE_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," KK_PRELUDE_BYTES "${KK_PRELUDE_HEX}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/prelude.cpp.new"
    "// Generated from hlsl/kk_common.hlsli.\nnamespace kkshaders {\nextern const char kPreludeText[] = {${KK_PRELUDE_BYTES}0x00};\n}\n")
configure_file("${CMAKE_CURRENT_BINARY_DIR}/prelude.cpp.new" "${CMAKE_CURRENT_BINARY_DIR}/prelude.cpp" COPYONLY)

add_library(kkshaders STATIC
    src/container.cpp
    src/translator.cpp
    src/compiler.cpp
    src/cache.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/prelude.cpp")
target_include_directories(kkshaders PUBLIC include PRIVATE "${KK_DXC_INCLUDE}")
target_link_libraries(kkshaders PUBLIC kkshaders_deps PRIVATE xenosrecomp_core ${CMAKE_DL_LIBS})
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(kkshaders PRIVATE -Wall -Wno-missing-braces)
    if(NOT WIN32)
        # dxcapi.h on Linux (WinAdapter.h) uses Microsoft extensions.
        target_compile_options(kkshaders PRIVATE -fms-extensions -Wno-language-extension-token -Wno-ignored-attributes)
    endif()
endif()

# The command-line tool.
add_executable(kkshaders_tool tool/main.cpp)
set_target_properties(kkshaders_tool PROPERTIES OUTPUT_NAME kkshaders)
target_link_libraries(kkshaders_tool PRIVATE kkshaders)
find_package(Threads REQUIRED)
target_link_libraries(kkshaders_tool PRIVATE Threads::Threads)

# Tests: a Xenos microcode assembler and a generated corpus (no game data needed); the
# database tests run when KKSHADERS_DATABASE exists.
find_program(KKSHADERS_SPIRV_VAL spirv-val HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
enable_testing()
add_executable(kkshaders_tests tests/xenos_asm.cpp tests/container_writer.cpp tests/corpus.cpp tests/tests.cpp)
target_include_directories(kkshaders_tests PRIVATE tests)
target_link_libraries(kkshaders_tests PRIVATE kkshaders xenosrecomp_core Threads::Threads)

set(KK_TEST_ARGS --dxc "${KKSHADERS_DXC_DIR}")
if(KKSHADERS_SPIRV_VAL)
    list(APPEND KK_TEST_ARGS --spirv-val "${KKSHADERS_SPIRV_VAL}")
endif()
add_test(NAME assembler COMMAND kkshaders_tests assembler)
add_test(NAME container COMMAND kkshaders_tests container)
add_test(NAME corpus COMMAND kkshaders_tests corpus ${KK_TEST_ARGS} --out "${CMAKE_CURRENT_BINARY_DIR}/corpus-out")
add_test(NAME cache COMMAND kkshaders_tests cache ${KK_TEST_ARGS} --out "${CMAKE_CURRENT_BINARY_DIR}/cache-out")
# The tool's database commands on a synthetic database and shader storage file.
set(KK_FIX "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
add_test(NAME fixtures COMMAND kkshaders_tests make-fixtures "${KK_FIX}")
add_test(NAME tool-db-check COMMAND kkshaders_tool db-check "${KK_FIX}/xeshaders.bin")
add_test(NAME tool-db-build COMMAND kkshaders_tool db-build "${KK_FIX}/xeshaders.bin" --out "${KK_FIX}/db-out" ${KK_TEST_ARGS})
add_test(NAME tool-db-structure COMMAND kkshaders_tool db-structure "${KK_FIX}/xeshaders.bin")
add_test(NAME tool-xsh COMMAND kkshaders_tool xsh "${KK_FIX}/fixture.xsh" --out "${KK_FIX}/xsh-out" ${KK_TEST_ARGS})
add_test(NAME tool-pack-lookup COMMAND kkshaders_tool pack-lookup "${KK_FIX}/db-out/kkshaders.pack")
set_tests_properties(tool-db-check tool-db-build tool-db-structure tool-xsh PROPERTIES DEPENDS fixtures)
set_tests_properties(tool-pack-lookup PROPERTIES DEPENDS tool-db-build)
if(EXISTS "${KKSHADERS_DATABASE}")
    add_test(NAME database COMMAND kkshaders_tool db-build "${KKSHADERS_DATABASE}"
        --out "${CMAKE_CURRENT_BINARY_DIR}/db-out" ${KK_TEST_ARGS})
endif()
