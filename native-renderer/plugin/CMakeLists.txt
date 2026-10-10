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
    ring_skimmer.cpp
    ${NR_ROOT}/backend/backend.cpp
)
if(NR_HAS_D3D12)
    list(APPEND NR_PLUGIN_SOURCES ${NR_ROOT}/backend/d3d12_host.cpp)
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
