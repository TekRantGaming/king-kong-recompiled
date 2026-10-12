# The temporal upscalers' SDKs (docs/upscalers.md, "Building"). Included from backend/CMakeLists.txt
# with the nr_renderer target. Every SDK is optional: what is not found is left out (the upscaler
# reports "not built in" and the choice falls back), and nothing changes without an upscaler.
#
#   AMD FidelityFX API (FSR 3.1, FSR 4): the API headers are vendored (MIT, thirdparty/FidelityFX-API);
#     the signed DLLs are loaded at run time. NR_FFX_SDK_DIR: the FidelityFX SDK 1.1.4 (its
#     PrebuiltSignedDLL folder: amd_fidelityfx_vk.dll, amd_fidelityfx_dx12.dll); NR_FSR4_SDK_DIR: the
#     FSR SDK 2.x Kits/FidelityFX folder (signedbin: amd_fidelityfx_loader_dx12.dll,
#     amd_fidelityfx_upscaler_dx12.dll).
#   NVIDIA DLSS: NR_DLSS_SDK_DIR, the DLSS SDK (include/, lib/Windows_x86_64/x64/nvsdk_ngx_d.lib,
#     lib/Windows_x86_64/rel/nvngx_dlss.dll). Its licence allows the object code only: not in git.
#   Intel XeSS: NR_XESS_SDK_DIR, the XeSS SDK (inc/xess/, bin/libxess.dll), loaded at run time. Binary
#     redistribution only: not in git.
#
# The defaults are tools/upscaler-sdks/<fsr3|fsr4|dlss|xess> beside the repository's tools (a
# git-ignored folder, like tools/rexglue: junctions or copies of the SDKs as downloaded).
#
# The runtime files go beside the game with the plugin (the global property NR_UPSCALER_RUNTIME_FILES,
# plugin/CMakeLists.txt): the launcher's updater installs files beside king_kong.exe only.

set(_nr_up_default "${NR_ROOT}/../tools/upscaler-sdks")
set(NR_FFX_SDK_DIR "${_nr_up_default}/fsr3" CACHE PATH "AMD FidelityFX SDK 1.1.x (FSR 3.1: PrebuiltSignedDLL)")
set(NR_FSR4_SDK_DIR "${_nr_up_default}/fsr4" CACHE PATH "AMD FSR SDK 2.x Kits/FidelityFX (FSR 4: signedbin)")
set(NR_DLSS_SDK_DIR "${_nr_up_default}/dlss" CACHE PATH "NVIDIA DLSS SDK")
set(NR_XESS_SDK_DIR "${_nr_up_default}/xess" CACHE PATH "Intel XeSS SDK")

set(_nr_up_runtime "")
set(NR_HAS_FFX 0)
set(NR_HAS_DLSS 0)
set(NR_HAS_XESS 0)

if(WIN32)
    # The adapter's vendor (upscaler.cpp) on D3D12.
    target_link_libraries(nr_renderer PUBLIC dxgi)

    # AMD: the vendored API headers; the DLLs, when found, go beside the game.
    if(EXISTS "${NR_ROOT}/thirdparty/FidelityFX-API/ffx_api/ffx_api.h")
        set(NR_HAS_FFX 1)
        target_include_directories(nr_renderer PRIVATE "${NR_ROOT}/thirdparty/FidelityFX-API")
        foreach(_f amd_fidelityfx_vk.dll amd_fidelityfx_dx12.dll)
            if(EXISTS "${NR_FFX_SDK_DIR}/PrebuiltSignedDLL/${_f}")
                list(APPEND _nr_up_runtime "${NR_FFX_SDK_DIR}/PrebuiltSignedDLL/${_f}")
            endif()
        endforeach()
        foreach(_f amd_fidelityfx_loader_dx12.dll amd_fidelityfx_upscaler_dx12.dll)
            if(EXISTS "${NR_FSR4_SDK_DIR}/signedbin/${_f}")
                list(APPEND _nr_up_runtime "${NR_FSR4_SDK_DIR}/signedbin/${_f}")
            endif()
        endforeach()
    endif()

    # NVIDIA: the NGX loader library (dynamic CRT) and its headers, from the SDK folder.
    if(EXISTS "${NR_DLSS_SDK_DIR}/include/nvsdk_ngx.h" AND EXISTS "${NR_DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
        set(NR_HAS_DLSS 1)
        target_include_directories(nr_renderer PRIVATE "${NR_DLSS_SDK_DIR}/include")
        # (version: the DLL's file version for the log.)
        target_link_libraries(nr_renderer PUBLIC "${NR_DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib" version)
        if(EXISTS "${NR_DLSS_SDK_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll")
            list(APPEND _nr_up_runtime "${NR_DLSS_SDK_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll")
        endif()
    endif()

    # Intel: headers only at build time; libxess.dll at run time.
    if(EXISTS "${NR_XESS_SDK_DIR}/inc/xess/xess.h")
        set(NR_HAS_XESS 1)
        target_include_directories(nr_renderer PRIVATE "${NR_XESS_SDK_DIR}/inc")
        if(EXISTS "${NR_XESS_SDK_DIR}/bin/libxess.dll")
            list(APPEND _nr_up_runtime "${NR_XESS_SDK_DIR}/bin/libxess.dll")
        endif()
    endif()
endif()

target_compile_definitions(nr_renderer PUBLIC NR_HAS_FFX=${NR_HAS_FFX} NR_HAS_DLSS=${NR_HAS_DLSS} NR_HAS_XESS=${NR_HAS_XESS})
set_property(GLOBAL PROPERTY NR_UPSCALER_RUNTIME_FILES "${_nr_up_runtime}")
message(STATUS "native-renderer: upscalers FSR ${NR_HAS_FFX}, DLSS ${NR_HAS_DLSS}, XeSS ${NR_HAS_XESS}; beside the game: ${_nr_up_runtime}")
