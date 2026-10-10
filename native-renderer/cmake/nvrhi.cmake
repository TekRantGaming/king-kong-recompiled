# NVRHI (vendored, MIT) as static libraries, never downloading anything.
#
# Host APIs:
# - With the SDK: REX_HAS_D3D12 / REX_HAS_VULKAN from rex::runtime's interface
#   definitions (the Windows bundle is D3D12 only, the Linux bundle Vulkan
#   only).
# - Without it (tests only): Vulkan on Linux, D3D12 on Windows.
# NR_FORCE_VULKAN=ON adds Vulkan anyway (needs Vulkan-Headers, see below).
#
# Vulkan-Headers: the Vulkan::Headers target when it exists (the Linux SDK
# bundle provides it), else find_package(VulkanHeaders CONFIG), else the
# folder in NR_VULKAN_HEADERS (a Vulkan-Headers include directory). NVRHI
# needs headers 1.3.318 or newer, newer than Ubuntu 24.04's.
#
# Outputs: NR_HAS_D3D12, NR_HAS_VULKAN, targets nvrhi, nvrhi_vk, nvrhi_d3d12.

set(NR_VULKAN_HEADERS "" CACHE PATH "Vulkan-Headers include folder (when no Vulkan::Headers target is found)")
option(NR_FORCE_VULKAN "Build the Vulkan backend even when the SDK bundle has no Vulkan" OFF)

set(NR_HAS_D3D12 OFF)
set(NR_HAS_VULKAN OFF)
if(TARGET rex::runtime)
    get_target_property(_nr_rex_defs rex::runtime INTERFACE_COMPILE_DEFINITIONS)
    if("REX_HAS_D3D12=1" IN_LIST _nr_rex_defs)
        set(NR_HAS_D3D12 ON)
    endif()
    if("REX_HAS_VULKAN=1" IN_LIST _nr_rex_defs)
        set(NR_HAS_VULKAN ON)
    endif()
elseif(WIN32)
    set(NR_HAS_D3D12 ON)
else()
    set(NR_HAS_VULKAN ON)
endif()
if(NR_FORCE_VULKAN)
    set(NR_HAS_VULKAN ON)
endif()
if(NOT NR_HAS_D3D12 AND NOT NR_HAS_VULKAN)
    message(FATAL_ERROR "native-renderer: neither D3D12 nor Vulkan is available")
endif()

set(NVRHI_WITH_DX11 OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX12 ${NR_HAS_D3D12} CACHE BOOL "" FORCE)
set(NVRHI_WITH_VULKAN ${NR_HAS_VULKAN} CACHE BOOL "" FORCE)
set(NVRHI_WITH_VALIDATION ON CACHE BOOL "" FORCE)
set(NVRHI_WITH_RTXMU OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_AFTERMATH OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_NVAPI OFF CACHE BOOL "" FORCE)
set(NVRHI_INSTALL OFF CACHE BOOL "" FORCE)
set(NVRHI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(NVRHI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NVRHI_FETCH_VULKAN_HEADERS OFF CACHE BOOL "" FORCE)
set(NVRHI_FETCH_DIRECTX_HEADERS OFF CACHE BOOL "" FORCE)

if(NR_HAS_D3D12 AND NOT TARGET Microsoft::DirectX-Headers)
    # NVRHI includes <directx/d3d12.h> from Microsoft's DirectX-Headers and uses
    # preview-only names (the linear algebra barriers, ID3D12DevicePreview)
    # that the retail Windows SDK's d3d12.h lacks, so the headers of the tag
    # NVRHI asks for (v1.717.0-preview) are vendored in thirdparty/DirectX-Headers.
    # Only <directx/...> is on the include path: plain <d3d12.h> (the SDK's
    # headers) stays the Windows SDK's.
    add_library(nr_directx_headers INTERFACE)
    target_include_directories(nr_directx_headers SYSTEM INTERFACE "${NR_ROOT}/thirdparty/DirectX-Headers/include")
    add_library(Microsoft::DirectX-Headers ALIAS nr_directx_headers)
    add_library(nr_directx_guids STATIC "${NR_ROOT}/thirdparty/DirectX-Headers/src/dxguids.cpp")
    target_link_libraries(nr_directx_guids PUBLIC nr_directx_headers dxguid)
    add_library(Microsoft::DirectX-Guids ALIAS nr_directx_guids)
endif()

if(NR_HAS_VULKAN AND NOT TARGET Vulkan::Headers)
    if(NR_VULKAN_HEADERS)
        add_library(nr_vulkan_headers INTERFACE)
        target_include_directories(nr_vulkan_headers INTERFACE "${NR_VULKAN_HEADERS}")
        add_library(Vulkan::Headers ALIAS nr_vulkan_headers)
    else()
        find_package(VulkanHeaders CONFIG REQUIRED)
    endif()
endif()

add_subdirectory("${NR_ROOT}/thirdparty/nvrhi" "${CMAKE_BINARY_DIR}/nvrhi" EXCLUDE_FROM_ALL)
message(STATUS "native-renderer: D3D12 ${NR_HAS_D3D12}, Vulkan ${NR_HAS_VULKAN}")
