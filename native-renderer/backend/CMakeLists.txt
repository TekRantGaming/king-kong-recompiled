# Backend libraries:
#   nr_draw      the draw-state tracker, the NrApi binding and the primitive
#                expansion: no NVRHI, no SDK (unit-tested on its own)
#   nr_renderer  the NVRHI renderer (test pictures, placeholder draws, the
#                game renderer when the resources and shader libraries are
#                available) and the compiled shaders
# The SDK-specific parts (host_device.h, vulkan_host.cpp, d3d12_host.cpp,
# backend.cpp) are compiled into the plugin (plugin/CMakeLists.txt).

add_library(nr_draw STATIC
    draw_state.cpp
    frame_log.cpp
    api_binding.cpp
    primitive.cpp
)
target_include_directories(nr_draw PUBLIC "${NR_ROOT}")
target_compile_features(nr_draw PUBLIC cxx_std_20)

add_library(nr_renderer STATIC
    renderer.cpp
    shaders.cpp
    log.cpp
)
target_include_directories(nr_renderer PUBLIC "${NR_ROOT}")
target_link_libraries(nr_renderer PUBLIC nr_draw)
# Phase 2: the game renderer (real shaders, textures, state), when the
# resources and shader libraries could be built (cmake/kk_libs.cmake).
if(NR_GAME_RENDERER)
    target_sources(nr_renderer PRIVATE
        game_renderer.cpp
        game_resources.cpp
        game_passes.cpp
        game_frame_log.cpp
        shader_library.cpp
    )
    target_link_libraries(nr_renderer PUBLIC kknr_resources kkshaders)
    target_compile_definitions(nr_renderer PUBLIC NR_GAME_RENDERER=1)
else()
    target_compile_definitions(nr_renderer PUBLIC NR_GAME_RENDERER=0)
endif()
if(NR_HAS_VULKAN)
    target_sources(nr_renderer PRIVATE vulkan_dispatch.cpp)
    target_link_libraries(nr_renderer PUBLIC nvrhi_vk Vulkan::Headers)
    target_compile_definitions(nr_renderer PUBLIC NR_HAS_VULKAN=1 VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1)
else()
    target_compile_definitions(nr_renderer PUBLIC NR_HAS_VULKAN=0)
endif()
if(NR_HAS_D3D12)
    target_link_libraries(nr_renderer PUBLIC nvrhi_d3d12)
    target_compile_definitions(nr_renderer PUBLIC NR_HAS_D3D12=1)
else()
    target_compile_definitions(nr_renderer PUBLIC NR_HAS_D3D12=0)
endif()
# After the backends: they use the common library but do not link it
# themselves, and static link order matters.
target_link_libraries(nr_renderer PUBLIC nvrhi)
