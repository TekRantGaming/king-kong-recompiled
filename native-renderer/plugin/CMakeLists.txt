# rexgpu-native: the native renderer as a ReXGlue GPU plugin
# (rexgpu-native.dll on Windows, librexgpu-native.so on Linux).
#
# Reached through native-renderer/CMakeLists.txt, either standalone with the
# SDK bundle on CMAKE_PREFIX_PATH or from kk/CMakeLists.txt after the SDK has
# been found (add_subdirectory(../native-renderer native-renderer) with
# NR_BUILD_TESTS OFF). NVRHI and the backend libraries come from there.

set(NR_PLUGIN_SOURCES
    plugin_main.cpp
    native_graphics_system.cpp
    plugin_settings.cpp
    launcher_prewarm.cpp
    upscaler_query.cpp
    upscaler_check_child.cpp
    gpu_preference.cpp
    ring_skimmer.cpp
    photo_api.cpp
    ${NR_ROOT}/backend/backend.cpp
)
if(NR_HAS_D3D12)
    list(APPEND NR_PLUGIN_SOURCES ${NR_ROOT}/backend/d3d12_host.cpp)
endif()
if(NR_GAME_RENDERER)
    # The shader pack on the player's machine: the launcher's build, the run's lookup.
    list(APPEND NR_PLUGIN_SOURCES shader_pack_service.cpp builtin_shaders.cpp)
endif()
if(NR_HAS_VULKAN)
    list(APPEND NR_PLUGIN_SOURCES ${NR_ROOT}/backend/vulkan_host.cpp)
endif()

add_library(rexgpu-native SHARED ${NR_PLUGIN_SOURCES})
set_target_properties(rexgpu-native PROPERTIES
    DEBUG_POSTFIX d
    RELWITHDEBINFO_POSTFIX rd
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    PREFIX ""
)
if(UNIX)
    # LoadGpuPlugin looks for librexgpu-<name>.so.
    set_target_properties(rexgpu-native PROPERTIES PREFIX "lib")
    # Keep the static NVRHI / backend symbols inside the plugin.
    target_link_options(rexgpu-native PRIVATE "LINKER:--exclude-libs,ALL" "LINKER:--no-undefined")
endif()
target_include_directories(rexgpu-native PRIVATE "${NR_ROOT}")
# The SDK's headers are C++23 (std::byteswap), like kk.
target_compile_features(rexgpu-native PRIVATE cxx_std_23)
# Everything PRIVATE: the plugin has no consumers. rexruntime already holds the
# UI providers and presenters (exported), so they are linked, not recompiled.
target_link_libraries(rexgpu-native PRIVATE nr_renderer rex::runtime rex::renderdoc)
if(NR_HAS_D3D12)
    target_link_libraries(rexgpu-native PRIVATE d3d12 dxgi dxguid)
endif()
# From kk: copy the plugin beside the game after every plugin build (a change
# in the plugin alone does not relink the game, so a step on the game's target
# would leave a stale copy there).
if(NR_PLUGIN_COPY_BESIDE)
    add_custom_command(TARGET rexgpu-native POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:rexgpu-native>
                $<TARGET_FILE_DIR:${NR_PLUGIN_COPY_BESIDE}>)
endif()
# DXC comes with the port: the plugin builds the player's shader pack with it and compiles shaders
# missing from the pack (docs/shaders.md, "DXC with the port"). Only the compiler library is
# needed (it signs DXIL itself from DXC 1.8.2505 on; dxil.dll / libdxil.so are not shipped).
# NR_DXC_RUNTIME names the library; else it is looked for in NR_DXC_DIR and in $ENV{KK_DXC_DIR}
# (a DXC release folder). Copied beside the game with the plugin.
if(WIN32)
    set(_nr_dxc_lib dxcompiler.dll)
else()
    set(_nr_dxc_lib libdxcompiler.so)
endif()
set(NR_DXC_RUNTIME "" CACHE FILEPATH "DXC's compiler library (dxcompiler.dll / libdxcompiler.so) to put beside the game")
if(NOT NR_DXC_RUNTIME)
    foreach(_dir IN ITEMS "${NR_DXC_DIR}" "$ENV{KK_DXC_DIR}")
        if(_dir)
            foreach(_sub IN ITEMS "" "bin/x64" "bin" "lib")
                if(NOT NR_DXC_RUNTIME AND EXISTS "${_dir}/${_sub}/${_nr_dxc_lib}")
                    set(NR_DXC_RUNTIME "${_dir}/${_sub}/${_nr_dxc_lib}")
                endif()
            endforeach()
        endif()
    endforeach()
endif()
if(NR_PLUGIN_COPY_BESIDE AND NR_GAME_RENDERER)
    if(NR_DXC_RUNTIME AND EXISTS "${NR_DXC_RUNTIME}")
        message(STATUS "native-renderer: ${NR_DXC_RUNTIME} goes beside the game")
        add_custom_command(TARGET rexgpu-native POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NR_DXC_RUNTIME}"
                    $<TARGET_FILE_DIR:${NR_PLUGIN_COPY_BESIDE}>/${_nr_dxc_lib})
    else()
        message(WARNING "native-renderer: no ${_nr_dxc_lib} to put beside the game (set NR_DXC_RUNTIME or KK_DXC_DIR): "
                        "without it the launcher cannot build the shader pack and the game renderer has no shaders")
    endif()
endif()
# The upscalers' runtime files (cmake/upscalers.cmake, docs/upscalers.md "What a release ships"): beside
# the game, at the top level (the launcher's updater installs nothing in subfolders but glyphs/ and
# launcher_art/).
get_property(_nr_up_runtime GLOBAL PROPERTY NR_UPSCALER_RUNTIME_FILES)
if(NR_PLUGIN_COPY_BESIDE AND _nr_up_runtime)
    foreach(_f IN LISTS _nr_up_runtime)
        get_filename_component(_name "${_f}" NAME)
        add_custom_command(TARGET rexgpu-native POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_f}" $<TARGET_FILE_DIR:${NR_PLUGIN_COPY_BESIDE}>/${_name})
    endforeach()
endif()
