# rexgpu-native: the native renderer as a ReXGlue GPU plugin.
#
# Included from kk/CMakeLists.txt (add_subdirectory) after the SDK has been
# found, so rex::runtime exists. Builds NVRHI (vendored, MIT) as static
# libraries and links it into rexgpu-native.dll next to king_kong.exe.
#
# Backends follow the SDK bundle: REX_HAS_D3D12 / REX_HAS_VULKAN come from
# rex::runtime's interface definitions (the Windows bundle is D3D12 only, the
# Linux bundle Vulkan only). KK_NATIVE_VULKAN_HEADERS=<dir> (a Vulkan-Headers
# include folder) adds a compile-only check of the Vulkan side on a bundle
# without Vulkan.

cmake_minimum_required(VERSION 3.25)

set(NR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(KK_NATIVE_VULKAN_HEADERS "" CACHE PATH
    "Vulkan-Headers include folder, to compile-check the Vulkan backend on a bundle without Vulkan")

get_target_property(_nr_rex_defs rex::runtime INTERFACE_COMPILE_DEFINITIONS)
set(NR_HAS_D3D12 OFF)
set(NR_HAS_VULKAN OFF)
if("REX_HAS_D3D12=1" IN_LIST _nr_rex_defs)
    set(NR_HAS_D3D12 ON)
endif()
if("REX_HAS_VULKAN=1" IN_LIST _nr_rex_defs)
    set(NR_HAS_VULKAN ON)
endif()
if(NOT NR_HAS_D3D12 AND NOT NR_HAS_VULKAN)
    message(FATAL_ERROR "rexgpu-native: the SDK bundle offers neither D3D12 nor Vulkan")
endif()

# The Vulkan NVRHI backend is built when the bundle has Vulkan, or for the
# compile check when a headers folder is given.
set(NR_BUILD_VULKAN ${NR_HAS_VULKAN})
set(NR_VULKAN_CHECK_ONLY OFF)
if(NOT NR_HAS_VULKAN AND KK_NATIVE_VULKAN_HEADERS)
    set(NR_BUILD_VULKAN ON)
    set(NR_VULKAN_CHECK_ONLY ON)
endif()

# ---------------------------------------------------------------- NVRHI ---
set(NVRHI_WITH_DX11 OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX12 ${NR_HAS_D3D12} CACHE BOOL "" FORCE)
set(NVRHI_WITH_VULKAN ${NR_BUILD_VULKAN} CACHE BOOL "" FORCE)
set(NVRHI_WITH_VALIDATION ON CACHE BOOL "" FORCE)
set(NVRHI_WITH_RTXMU OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_AFTERMATH OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_NVAPI OFF CACHE BOOL "" FORCE)
set(NVRHI_INSTALL OFF CACHE BOOL "" FORCE)
set(NVRHI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
# Never download: the header targets are provided below.
set(NVRHI_FETCH_VULKAN_HEADERS OFF CACHE BOOL "" FORCE)
set(NVRHI_FETCH_DIRECTX_HEADERS OFF CACHE BOOL "" FORCE)

if(NR_HAS_D3D12)
    # NVRHI includes <directx/d3d12.h> (DirectX-Headers); thirdparty/directx-shim
    # forwards that to the Windows SDK's d3d12.h. The GUIDs come from dxguid.
    add_library(nr_directx_headers INTERFACE)
    target_include_directories(nr_directx_headers INTERFACE "${NR_ROOT}/thirdparty/directx-shim")
    add_library(Microsoft::DirectX-Headers ALIAS nr_directx_headers)
    add_library(nr_directx_guids INTERFACE)
    target_link_libraries(nr_directx_guids INTERFACE dxguid)
    add_library(Microsoft::DirectX-Guids ALIAS nr_directx_guids)
endif()

if(NR_BUILD_VULKAN AND NOT TARGET Vulkan::Headers)
    if(NOT KK_NATIVE_VULKAN_HEADERS)
        message(FATAL_ERROR "rexgpu-native: the bundle has Vulkan but no Vulkan::Headers target")
    endif()
    add_library(nr_vulkan_headers INTERFACE)
    target_include_directories(nr_vulkan_headers INTERFACE "${KK_NATIVE_VULKAN_HEADERS}")
    add_library(Vulkan::Headers ALIAS nr_vulkan_headers)
endif()

add_subdirectory("${NR_ROOT}/thirdparty/nvrhi" nvrhi EXCLUDE_FROM_ALL)

# ----------------------------------------------------------- the plugin ---
set(NR_PLUGIN_SOURCES
    plugin_main.cpp
    native_graphics_system.cpp
    ring_skimmer.cpp
    ${NR_ROOT}/backend/backend.cpp
    ${NR_ROOT}/backend/draw_state.cpp
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
    # LoadGpuPlugin looks for librexgpu-native.so.
    set_target_properties(rexgpu-native PROPERTIES PREFIX "lib")
endif()
target_include_directories(rexgpu-native PRIVATE "${NR_ROOT}")
target_compile_definitions(rexgpu-native PRIVATE
    NR_HAS_D3D12=$<BOOL:${NR_HAS_D3D12}>
    NR_HAS_VULKAN=$<BOOL:${NR_HAS_VULKAN}>
)
# Everything PRIVATE: the plugin has no consumers. rexruntime already holds the
# UI providers and presenters (exported), so they are linked, not recompiled.
target_link_libraries(rexgpu-native PRIVATE rex::runtime nvrhi)
if(NR_HAS_D3D12)
    target_link_libraries(rexgpu-native PRIVATE nvrhi_d3d12 d3d12 dxgi dxguid)
endif()
if(NR_HAS_VULKAN)
    target_link_libraries(rexgpu-native PRIVATE nvrhi_vk Vulkan::Headers)
endif()

# Compile-only check of the Vulkan side on a bundle without Vulkan.
if(NR_VULKAN_CHECK_ONLY)
    add_library(rexgpu-native-vulkan-check OBJECT ${NR_ROOT}/backend/vulkan_host.cpp)
    target_include_directories(rexgpu-native-vulkan-check PRIVATE "${NR_ROOT}")
    target_compile_definitions(rexgpu-native-vulkan-check PRIVATE NR_HAS_D3D12=0 NR_HAS_VULKAN=1 REX_HAS_VULKAN=1)
    target_link_libraries(rexgpu-native-vulkan-check PRIVATE rex::runtime nvrhi nvrhi_vk Vulkan::Headers)
    add_dependencies(rexgpu-native rexgpu-native-vulkan-check)
endif()
