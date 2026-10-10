# The resources library (stream 03, native-renderer/resources) and the shader
# translator (stream 02, native-renderer/shaders) as libraries of this build,
# for the game renderer. Their own CMake projects build the same sources with
# their tools and tests; here only the libraries.
#
# Needs fmt, xxHash (xxh3.h) and dxcapi.h, which the ReXGlue SDK provides
# (fmt::fmt, and its include folder through rex::runtime). Without the SDK the
# game renderer is left out (NR_GAME_RENDERER OFF) and the placeholder
# pipeline remains.
#
# Outputs: NR_GAME_RENDERER, targets kknr_resources, kkshaders.

set(NR_GAME_RENDERER OFF)
if(TARGET rex::runtime AND TARGET fmt::fmt)
    set(NR_GAME_RENDERER ON)

    add_library(kknr_resources STATIC
        ${NR_ROOT}/resources/src/xenos.cpp
        ${NR_ROOT}/resources/src/tiling.cpp
        ${NR_ROOT}/resources/src/guest_texture.cpp
        ${NR_ROOT}/resources/src/endian.cpp
        ${NR_ROOT}/resources/src/texture_convert.cpp
        ${NR_ROOT}/resources/src/buffers.cpp
        ${NR_ROOT}/resources/src/texture_cache.cpp
        ${NR_ROOT}/resources/src/render_targets.cpp
        ${NR_ROOT}/resources/src/resolve.cpp
    )
    target_include_directories(kknr_resources PUBLIC ${NR_ROOT}/resources/include)
    target_compile_features(kknr_resources PUBLIC cxx_std_20)

    # The SDK's include folder (xxh3.h, dxc/dxcapi.h) without linking it here.
    # The Linux SDK install has no dxcapi.h: NR_DXC_DIR names a DXC release
    # (include/dxc/dxcapi.h with WinAdapter.h, lib/libdxcompiler.so), which the
    # tests also load the compiler from.
    set(NR_DXC_DIR "" CACHE PATH "DXC release folder (include/dxc, lib or bin)")
    get_target_property(_nr_rex_includes rex::runtime INTERFACE_INCLUDE_DIRECTORIES)
    set(_nr_dxc_include "")
    foreach(_dir IN LISTS _nr_rex_includes)
        if(EXISTS "${_dir}/dxc/dxcapi.h")
            set(_nr_dxc_include "${_dir}/dxc")
        endif()
    endforeach()
    if(NR_DXC_DIR AND EXISTS "${NR_DXC_DIR}/include/dxc/dxcapi.h")
        set(_nr_dxc_include "${NR_DXC_DIR}/include/dxc")
    elseif(NR_DXC_DIR AND EXISTS "${NR_DXC_DIR}/inc/dxcapi.h")
        set(_nr_dxc_include "${NR_DXC_DIR}/inc")
    endif()
    if(NOT _nr_dxc_include)
        message(STATUS "native-renderer: no dxcapi.h in the SDK; set NR_DXC_DIR to a DXC release. "
                       "The game renderer is left out.")
        set(NR_GAME_RENDERER OFF)
    endif()
endif()
if(NR_GAME_RENDERER)

    set(_nr_xenos_dir "${NR_ROOT}/thirdparty/XenosRecomp/XenosRecomp")
    add_library(nr_xenosrecomp STATIC "${_nr_xenos_dir}/shader_recompiler.cpp")
    target_include_directories(nr_xenosrecomp PUBLIC "${_nr_xenos_dir}" ${_nr_rex_includes})
    target_link_libraries(nr_xenosrecomp PUBLIC fmt::fmt)
    target_precompile_headers(nr_xenosrecomp PRIVATE "${_nr_xenos_dir}/pch.h")
    target_compile_features(nr_xenosrecomp PUBLIC cxx_std_20)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(nr_xenosrecomp PRIVATE -Wno-switch -Wno-unused-variable)
    endif()
    if(WIN32)
        target_compile_definitions(nr_xenosrecomp PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    endif()

    # The prelude (hlsl/kk_common.hlsli) embedded as a byte array.
    set(_nr_prelude "${NR_ROOT}/shaders/hlsl/kk_common.hlsli")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_nr_prelude}")
    file(READ "${_nr_prelude}" _nr_prelude_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _nr_prelude_bytes "${_nr_prelude_hex}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/kkshaders_prelude.cpp.new"
        "// Generated from hlsl/kk_common.hlsli.\nnamespace kkshaders {\nextern const char kPreludeText[] = {${_nr_prelude_bytes}0x00};\n}\n")
    configure_file("${CMAKE_CURRENT_BINARY_DIR}/kkshaders_prelude.cpp.new"
        "${CMAKE_CURRENT_BINARY_DIR}/kkshaders_prelude.cpp" COPYONLY)

    add_library(kkshaders STATIC
        ${NR_ROOT}/shaders/src/container.cpp
        ${NR_ROOT}/shaders/src/translator.cpp
        ${NR_ROOT}/shaders/src/compiler.cpp
        ${NR_ROOT}/shaders/src/cache.cpp
        ${NR_ROOT}/shaders/src/vertex_patch.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/kkshaders_prelude.cpp")
    target_include_directories(kkshaders PUBLIC ${NR_ROOT}/shaders/include PRIVATE "${_nr_dxc_include}")
    target_link_libraries(kkshaders PUBLIC nr_xenosrecomp ${CMAKE_DL_LIBS})
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(kkshaders PRIVATE -Wno-missing-braces)
        if(NOT WIN32)
            target_compile_options(kkshaders PRIVATE -fms-extensions -Wno-language-extension-token -Wno-ignored-attributes)
        endif()
    endif()
endif()
message(STATUS "native-renderer: game renderer ${NR_GAME_RENDERER}")
