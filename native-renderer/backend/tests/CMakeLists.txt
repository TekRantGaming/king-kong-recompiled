# Tests and test tools (standalone build only).
#   nr_draw_tests    milestone 3 without a GPU: recorded call sequences through
#                    the hook table, the NrApi binding and the tracker
#   nr_render_tests  the standalone test host: NVRHI on its own Vulkan device
#                    (lavapipe), milestones 1-3 rendered offscreen and read back
#   nr_trace_replay  replays a KK_DEV_D3D_TRACE log (a tool for real traces)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(NR_HDR_SDK_SOURCE "" CACHE PATH "SDK source with HDR patch 0043, for production shader tests")
if(WIN32 AND NR_HAS_VULKAN AND NR_HAS_D3D12 AND EXISTS "${NR_HDR_SDK_SOURCE}/include/rex/ui/hdr.h")
    add_executable(nr_hdr_output_tests hdr_output_tests.cpp vk_test_device.cpp readback.cpp)
    target_include_directories(nr_hdr_output_tests BEFORE PRIVATE "${NR_HDR_SDK_SOURCE}/src")
    target_link_libraries(nr_hdr_output_tests PRIVATE nr_renderer rex::runtime d3d12 dxgi)
    target_compile_features(nr_hdr_output_tests PRIVATE cxx_std_23)
    add_test(NAME nr_hdr_output_tests COMMAND nr_hdr_output_tests)
endif()
add_executable(nr_setup_prewarm_availability_tests setup_prewarm_availability_tests.cpp)
target_include_directories(nr_setup_prewarm_availability_tests PRIVATE "${NR_ROOT}")
target_compile_features(nr_setup_prewarm_availability_tests PRIVATE cxx_std_23)
add_test(NAME nr_setup_prewarm_availability_tests COMMAND nr_setup_prewarm_availability_tests)
add_executable(nr_memory_budget_tests memory_budget_tests.cpp)
target_include_directories(nr_memory_budget_tests PRIVATE "${NR_ROOT}")
add_test(NAME nr_memory_budget_tests COMMAND nr_memory_budget_tests)
add_executable(nr_retire_budget_tests retire_budget_tests.cpp)
target_include_directories(nr_retire_budget_tests PRIVATE "${NR_ROOT}")
add_test(NAME nr_retire_budget_tests COMMAND nr_retire_budget_tests)
add_executable(nr_background_file_writer_tests background_file_writer_tests.cpp)
target_include_directories(nr_background_file_writer_tests PRIVATE "${NR_ROOT}")
target_compile_features(nr_background_file_writer_tests PRIVATE cxx_std_20)
add_test(NAME nr_background_file_writer_tests COMMAND nr_background_file_writer_tests)
if(WIN32 AND NR_BUILD_PLUGIN AND NR_GAME_RENDERER)
    add_executable(nr_guest_frame_sampler guest_frame_sampler.cpp)
    target_compile_features(nr_guest_frame_sampler PRIVATE cxx_std_23)
    add_executable(nr_shader_pack_service_tests shader_pack_service_tests.cpp)
    target_include_directories(nr_shader_pack_service_tests PRIVATE "${NR_ROOT}")
    target_compile_features(nr_shader_pack_service_tests PRIVATE cxx_std_23)
    target_link_libraries(nr_shader_pack_service_tests PRIVATE kkshaders rex::runtime)
    add_dependencies(nr_shader_pack_service_tests rexgpu-native)
    add_executable(nr_builtin_shader_tests builtin_shader_tests.cpp "${NR_ROOT}/plugin/builtin_shaders.cpp")
    target_include_directories(nr_builtin_shader_tests PRIVATE "${NR_ROOT}")
    target_compile_features(nr_builtin_shader_tests PRIVATE cxx_std_23)
    target_link_libraries(nr_builtin_shader_tests PRIVATE kkshaders rex::runtime)
    add_test(NAME nr_builtin_shader_tests COMMAND nr_builtin_shader_tests)
endif()
add_executable(nr_vulkan_requirements_tests vulkan_requirements_tests.cpp)
target_include_directories(nr_vulkan_requirements_tests PRIVATE "${NR_ROOT}")
add_test(NAME nr_vulkan_requirements_tests COMMAND nr_vulkan_requirements_tests)

add_executable(nr_fallback_resources_tests fallback_resources_tests.cpp)
target_include_directories(nr_fallback_resources_tests PRIVATE "${NR_ROOT}" "${NR_ROOT}/thirdparty/nvrhi/include")
add_test(NAME nr_fallback_resources_tests COMMAND nr_fallback_resources_tests)

if(WIN32)
    add_executable(nr_d3d12_requirements_tests d3d12_requirements_tests.cpp)
    target_include_directories(nr_d3d12_requirements_tests PRIVATE "${NR_ROOT}")
    add_test(NAME nr_d3d12_requirements_tests COMMAND nr_d3d12_requirements_tests)
endif()

if(NR_HAS_D3D12)
    add_executable(nr_bindless_sampler_tests bindless_sampler_tests.cpp)
    target_link_libraries(nr_bindless_sampler_tests PRIVATE nvrhi nvrhi_d3d12 d3d12)
    add_test(NAME nr_bindless_sampler_tests COMMAND nr_bindless_sampler_tests)
endif()

add_executable(nr_draw_tests draw_tests.cpp)
target_link_libraries(nr_draw_tests PRIVATE nr_draw nr_hooks)
target_compile_definitions(nr_draw_tests PRIVATE NR_SOURCE_DIR="${NR_ROOT}")
add_test(NAME nr_draw_tests COMMAND nr_draw_tests)

add_executable(nr_frame_log_tests frame_log_tests.cpp)
target_link_libraries(nr_frame_log_tests PRIVATE nr_draw)
add_test(NAME nr_frame_log_tests COMMAND nr_frame_log_tests)

add_executable(nr_pipeline_cache_tests pipeline_cache_tests.cpp)
target_link_libraries(nr_pipeline_cache_tests PRIVATE nr_draw)
# The tests also run the nr_pipelines tool on files they write.
target_compile_definitions(nr_pipeline_cache_tests PRIVATE NR_PIPELINES_TOOL="$<TARGET_FILE:nr_pipelines>")
add_dependencies(nr_pipeline_cache_tests nr_pipelines)
add_test(NAME nr_pipeline_cache_tests COMMAND nr_pipeline_cache_tests)

if(TARGET nr_resources)
    add_executable(nr_resource_preload_bundle_tests resource_preload_bundle_tests.cpp)
    target_include_directories(nr_resource_preload_bundle_tests PRIVATE "${NR_ROOT}")
    target_link_libraries(nr_resource_preload_bundle_tests PRIVATE kknr_resources)
    target_compile_definitions(nr_resource_preload_bundle_tests PRIVATE NR_RESOURCES_TOOL="$<TARGET_FILE:nr_resources>")
    add_dependencies(nr_resource_preload_bundle_tests nr_resources)
    add_test(NAME nr_resource_preload_bundle_tests COMMAND nr_resource_preload_bundle_tests)
endif()
add_executable(nr_motion_pipeline_tests motion_pipeline_tests.cpp)
target_link_libraries(nr_motion_pipeline_tests PRIVATE nr_draw)
add_test(NAME nr_motion_pipeline_tests COMMAND nr_motion_pipeline_tests)

add_executable(nr_ao_rules_tests ao_rules_tests.cpp)
target_link_libraries(nr_ao_rules_tests PRIVATE nr_draw)
add_test(NAME nr_ao_rules_tests COMMAND nr_ao_rules_tests)

add_executable(nr_upscale_tests upscale_tests.cpp)
target_link_libraries(nr_upscale_tests PRIVATE nr_draw)
add_test(NAME nr_upscale_tests COMMAND nr_upscale_tests)

add_executable(nr_trace_replay trace_replay_main.cpp)
target_link_libraries(nr_trace_replay PRIVATE nr_draw nr_hooks)

if(NR_HAS_VULKAN)
    add_executable(nr_upload_tests upload_tests.cpp vk_test_device.cpp readback.cpp)
    target_link_libraries(nr_upload_tests PRIVATE nr_renderer ${CMAKE_DL_LIBS})
    if(NR_HAS_D3D12)
        target_link_libraries(nr_upload_tests PRIVATE d3d12)
    endif()
    add_test(NAME nr_upload_tests
             COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_SOURCE_DIR}/check_validation.py"
                     $<TARGET_FILE:nr_upload_tests>)
    if(NR_GAME_RENDERER)
        add_executable(nr_ann_tessellation_tests ann_tessellation_tests.cpp vk_test_device.cpp readback.cpp)
        target_link_libraries(nr_ann_tessellation_tests PRIVATE nr_renderer ${CMAKE_DL_LIBS})
        if(NR_HAS_D3D12)
            target_link_libraries(nr_ann_tessellation_tests PRIVATE d3d12)
        endif()
        add_executable(nr_memory_migration_tests memory_migration_tests.cpp vk_test_device.cpp readback.cpp)
        target_link_libraries(nr_memory_migration_tests PRIVATE nr_renderer nr_hooks ${CMAKE_DL_LIBS})
        if(NR_HAS_D3D12)
            target_link_libraries(nr_memory_migration_tests PRIVATE d3d12)
            add_test(NAME nr_memory_migration_d3d12_tests COMMAND nr_memory_migration_tests --d3d12)
        endif()
        add_test(NAME nr_memory_migration_tests COMMAND nr_memory_migration_tests)
    endif()
    add_executable(nr_render_tests render_tests.cpp vk_test_device.cpp readback.cpp)
    target_link_libraries(nr_render_tests PRIVATE nr_renderer nr_hooks ${CMAKE_DL_LIBS})
    # Under the Khronos layer (the device enables it when installed and counts
    # its errors; check_validation.py also fails on any it prints).
    add_test(NAME nr_render_tests
             COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_SOURCE_DIR}/check_validation.py"
                     $<TARGET_FILE:nr_render_tests>)
    add_executable(nr_ao_tests ao_tests.cpp vk_test_device.cpp readback.cpp)
    target_link_libraries(nr_ao_tests PRIVATE nr_renderer nr_hooks ${CMAKE_DL_LIBS})
    add_test(NAME nr_ao_tests
             COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_SOURCE_DIR}/check_validation.py"
                     $<TARGET_FILE:nr_ao_tests>)
    add_executable(nr_gtao_tests gtao_tests.cpp vk_test_device.cpp readback.cpp)
    target_link_libraries(nr_gtao_tests PRIVATE nr_renderer nr_hooks ${CMAKE_DL_LIBS})
    add_test(NAME nr_gtao_tests
             COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_SOURCE_DIR}/check_validation.py"
                     $<TARGET_FILE:nr_gtao_tests>)
    # The screen-space GI's GPU time per level (docs/gi.md, "Cost"): a bench, not a test.
    add_executable(nr_ssgi_bench ssgi_bench.cpp vk_test_device.cpp readback.cpp)
    target_link_libraries(nr_ssgi_bench PRIVATE nr_renderer nr_hooks ${CMAKE_DL_LIBS})
    # Prefer lavapipe when several Vulkan drivers are installed.
    set(_lvp "/usr/share/vulkan/icd.d/lvp_icd.json")
    if(EXISTS "${_lvp}")
        set_tests_properties(nr_render_tests nr_ao_tests nr_gtao_tests PROPERTIES ENVIRONMENT "VK_ICD_FILENAMES=${_lvp}")
    endif()
endif()

# The checked-in SPIR-V must validate.
find_program(NR_SPIRV_VAL spirv-val)
if(NR_SPIRV_VAL AND Python3_FOUND)
    add_test(NAME nr_spirv_val
             COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_SOURCE_DIR}/validate_spirv.py"
                     "${NR_SPIRV_VAL}" "${NR_ROOT}/backend/shaders/generated")
endif()

# The plugin against the SDK bundle (Linux): loaded by the runtime's own
# loader, and NVRHI on the SDK's VulkanProvider. Built next to the plugin so
# LoadGpuPlugin finds it (it looks beside the executable).
if(NR_BUILD_PLUGIN AND NR_HAS_VULKAN AND UNIX)
    add_executable(nr_plugin_tests plugin_tests.cpp readback.cpp
        "${NR_ROOT}/backend/vulkan_host.cpp" "${NR_ROOT}/backend/backend.cpp"
        "${NR_ROOT}/hooks/kk_native_ui.cpp")
    target_link_libraries(nr_plugin_tests PRIVATE nr_renderer nr_hooks rex::runtime rex::renderdoc
                          ${CMAKE_DL_LIBS})
    target_compile_features(nr_plugin_tests PRIVATE cxx_std_23)
    set_target_properties(nr_plugin_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:rexgpu-native>")
    add_dependencies(nr_plugin_tests rexgpu-native)
    set(_env "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation")
    if(EXISTS "${_lvp}")
        list(APPEND _env "VK_ICD_FILENAMES=${_lvp}")
    endif()
    add_test(NAME nr_plugin_tests
             COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_SOURCE_DIR}/check_validation.py"
                     $<TARGET_FILE:nr_plugin_tests>)
    set_tests_properties(nr_plugin_tests PROPERTIES ENVIRONMENT "${_env}")
endif()

# The game renderer without the game: synthetic guest memory, shaders from the
# shader tests' assembler and container writer, through the hooks on lavapipe.
if(NR_HAS_VULKAN AND NR_GAME_RENDERER)
    add_executable(nr_smaa_tests smaa_tests.cpp vk_test_device.cpp readback.cpp)
    target_link_libraries(nr_smaa_tests PRIVATE nr_renderer ${CMAKE_DL_LIBS})
    add_test(NAME nr_smaa_tests COMMAND nr_smaa_tests)
    if(NR_HAS_D3D12)
        target_link_libraries(nr_smaa_tests PRIVATE d3d12)
        add_test(NAME nr_smaa_d3d12_tests COMMAND nr_smaa_tests --d3d12)
    endif()
    add_executable(nr_texture_pack_tests texture_pack_tests.cpp vk_test_device.cpp readback.cpp
        "${NR_ROOT}/resources/tools/image_io.cpp")
    target_link_libraries(nr_texture_pack_tests PRIVATE nr_renderer ${CMAKE_DL_LIBS})
    add_test(NAME nr_texture_pack_vulkan_tests COMMAND nr_texture_pack_tests)
    if(WIN32 AND NR_HAS_D3D12)
        target_link_libraries(nr_texture_pack_tests PRIVATE d3d12)
        add_test(NAME nr_texture_pack_d3d12_tests COMMAND nr_texture_pack_tests --d3d12)
    endif()
    add_executable(nr_game_tests game_tests.cpp vk_test_device.cpp readback.cpp
        "${NR_ROOT}/shaders/tests/xenos_asm.cpp" "${NR_ROOT}/shaders/tests/container_writer.cpp")
    target_include_directories(nr_game_tests PRIVATE "${NR_ROOT}/shaders/tests")
    target_link_libraries(nr_game_tests PRIVATE nr_renderer nr_hooks ${CMAKE_DL_LIBS})
    target_compile_definitions(nr_game_tests PRIVATE NR_TEST_DXC_DIR="${NR_DXC_DIR}"
                              NR_COMPARE_PY="${NR_ROOT}/tests/compare.py"
                              NR_PYTHON="${Python3_EXECUTABLE}")
    add_test(NAME nr_game_tests
             COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_SOURCE_DIR}/check_validation.py"
                     $<TARGET_FILE:nr_game_tests>)
    set(_env "VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation")
    if(EXISTS "${_lvp}")
        list(APPEND _env "VK_ICD_FILENAMES=${_lvp}")
    endif()
    set_tests_properties(nr_game_tests PROPERTIES ENVIRONMENT "${_env}")
    # The ambient occlusion through the game renderer (docs/ao.md).
    add_executable(nr_game_ao_tests game_ao_tests.cpp vk_test_device.cpp readback.cpp
        "${NR_ROOT}/shaders/tests/xenos_asm.cpp" "${NR_ROOT}/shaders/tests/container_writer.cpp")
    target_include_directories(nr_game_ao_tests PRIVATE "${NR_ROOT}/shaders/tests")
    target_link_libraries(nr_game_ao_tests PRIVATE nr_renderer nr_hooks ${CMAKE_DL_LIBS})
    target_compile_definitions(nr_game_ao_tests PRIVATE NR_TEST_DXC_DIR="${NR_DXC_DIR}")
    add_test(NAME nr_game_ao_tests
             COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_SOURCE_DIR}/check_validation.py"
                     $<TARGET_FILE:nr_game_ao_tests>)
    set_tests_properties(nr_game_ao_tests PROPERTIES ENVIRONMENT "${_env}")
    # The screen-space GI through the game renderer (docs/gi.md).
    add_executable(nr_game_gi_tests game_gi_tests.cpp vk_test_device.cpp readback.cpp
        "${NR_ROOT}/shaders/tests/xenos_asm.cpp" "${NR_ROOT}/shaders/tests/container_writer.cpp")
    target_include_directories(nr_game_gi_tests PRIVATE "${NR_ROOT}/shaders/tests")
    target_link_libraries(nr_game_gi_tests PRIVATE nr_renderer nr_hooks ${CMAKE_DL_LIBS})
    target_compile_definitions(nr_game_gi_tests PRIVATE NR_TEST_DXC_DIR="${NR_DXC_DIR}")
    add_test(NAME nr_game_gi_tests
             COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_SOURCE_DIR}/check_validation.py"
                     $<TARGET_FILE:nr_game_gi_tests>)
    set_tests_properties(nr_game_gi_tests PROPERTIES ENVIRONMENT "${_env}")
    foreach(_suite game game_ao game_gi)
        foreach(_tables small minimum)
            add_test(NAME nr_${_suite}_${_tables}_tests
                     COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_SOURCE_DIR}/check_validation.py"
                             $<TARGET_FILE:nr_${_suite}_tests>)
            set_tests_properties(nr_${_suite}_${_tables}_tests PROPERTIES
                                 ENVIRONMENT "${_env};NR_TEST_DESCRIPTOR_TABLES=${_tables}")
        endforeach()
    endforeach()
    # The temporal upscalers' frame plumbing through the game renderer (docs/upscalers.md).
    add_executable(nr_game_upscale_tests game_upscale_tests.cpp vk_test_device.cpp readback.cpp
        "${NR_ROOT}/shaders/tests/xenos_asm.cpp" "${NR_ROOT}/shaders/tests/container_writer.cpp")
    target_include_directories(nr_game_upscale_tests PRIVATE "${NR_ROOT}/shaders/tests")
    target_link_libraries(nr_game_upscale_tests PRIVATE nr_renderer nr_hooks ${CMAKE_DL_LIBS})
    target_compile_definitions(nr_game_upscale_tests PRIVATE NR_TEST_DXC_DIR="${NR_DXC_DIR}")
    add_test(NAME nr_game_upscale_tests
             COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_SOURCE_DIR}/check_validation.py"
                     $<TARGET_FILE:nr_game_upscale_tests>)
    set_tests_properties(nr_game_upscale_tests PROPERTIES ENVIRONMENT "${_env}")
    # The upscalers' DLLs beside the test (the renderer loads them from the executable's folder).
    get_property(_nr_up_runtime GLOBAL PROPERTY NR_UPSCALER_RUNTIME_FILES)
    foreach(_f IN LISTS _nr_up_runtime)
        get_filename_component(_name "${_f}" NAME)
        add_custom_command(TARGET nr_game_upscale_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_f}" $<TARGET_FILE_DIR:nr_game_upscale_tests>/${_name})
    endforeach()
endif()

# The upscalers' SDKs alone on Direct3D 12 and Vulkan, without the game renderer (docs/upscalers.md,
# "Tests"): devices of their own on the first hardware adapter; skipped without one.
if(WIN32 AND NR_HAS_D3D12 AND NR_GAME_RENDERER)
    set(_nr_sdk_test_sources upscaler_sdk_tests.cpp readback.cpp)
    if(NR_HAS_VULKAN)
        list(APPEND _nr_sdk_test_sources vk_test_device.cpp)
    endif()
    add_executable(nr_upscaler_sdk_tests ${_nr_sdk_test_sources})
    target_link_libraries(nr_upscaler_sdk_tests PRIVATE nr_renderer d3d12 dxgi ${CMAKE_DL_LIBS})
    add_test(NAME nr_upscaler_sdk_tests COMMAND nr_upscaler_sdk_tests)
    get_property(_nr_up_runtime GLOBAL PROPERTY NR_UPSCALER_RUNTIME_FILES)
    foreach(_f IN LISTS _nr_up_runtime)
        get_filename_component(_name "${_f}" NAME)
        add_custom_command(TARGET nr_upscaler_sdk_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_f}" $<TARGET_FILE_DIR:nr_upscaler_sdk_tests>/${_name})
    endforeach()
endif()

# The motion variant of the translated vertex shaders, on the translator's generated corpus
# (docs/upscalers.md).
if(NR_GAME_RENDERER)
    add_executable(nr_motion_shader_tests motion_shader_tests.cpp
        "${NR_ROOT}/shaders/tests/xenos_asm.cpp" "${NR_ROOT}/shaders/tests/container_writer.cpp"
        "${NR_ROOT}/shaders/tests/corpus.cpp")
    target_include_directories(nr_motion_shader_tests PRIVATE "${NR_ROOT}/shaders/tests" "${NR_ROOT}")
    target_link_libraries(nr_motion_shader_tests PRIVATE kkshaders)
    target_compile_definitions(nr_motion_shader_tests PRIVATE NR_TEST_DXC_DIR="${NR_DXC_DIR}")
    add_test(NAME nr_motion_shader_tests COMMAND nr_motion_shader_tests)
endif()

# CPU hot-path output and process-affinity regression tests, no GPU or game.
add_executable(nr_cpu_paths_tests cpu_paths_tests.cpp
    "${NR_ROOT}/backend/primitive.cpp"
    "${NR_ROOT}/resources/src/buffers.cpp"
    "${NR_ROOT}/resources/src/endian.cpp"
    "${NR_ROOT}/resources/src/xenos.cpp"
    "${NR_ROOT}/resources/src/tiling.cpp")
target_include_directories(nr_cpu_paths_tests PRIVATE "${NR_ROOT}" "${NR_ROOT}/resources/include")
add_test(NAME nr_cpu_paths_tests COMMAND nr_cpu_paths_tests)

add_executable(nr_scheduling_tests scheduling_tests.cpp)
target_include_directories(nr_scheduling_tests PRIVATE "${NR_ROOT}")
add_test(NAME nr_scheduling_tests COMMAND nr_scheduling_tests)

# Synthetic lighting scenes, no game process or game-derived assets.
if(NR_HAS_VULKAN AND NR_GAME_RENDERER)
    find_program(NR_LIGHTING_DXC dxc HINTS "${NR_DXC_DIR}" REQUIRED)
    foreach(stage vs ps)
        set(blob "${CMAKE_CURRENT_BINARY_DIR}/lighting_trace_${stage}.h")
        add_custom_command(OUTPUT "${blob}"
            COMMAND "${NR_LIGHTING_DXC}" -T ${stage}_6_0 -E ${stage}_main -HV 2021 -O3
                -spirv -fspv-target-env=vulkan1.2
                -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-b-shift 256 0
                -Fh "${blob}" -Vn lighting_trace_${stage}
                "${CMAKE_CURRENT_SOURCE_DIR}/lighting_trace.hlsl"
            DEPENDS lighting_trace.hlsl "${NR_ROOT}/shaders/hlsl/kk_lighting.hlsli")
        list(APPEND lighting_test_blobs "${blob}")
    endforeach()
    foreach(spec "vs:vs:0" "pcf:ps:0" "pcss:ps:1")
        string(REPLACE ":" ";" parts "${spec}")
        list(GET parts 0 name)
        list(GET parts 1 stage)
        list(GET parts 2 pcss)
        set(blob "${CMAKE_CURRENT_BINARY_DIR}/lighting_filter_${name}.h")
        add_custom_command(OUTPUT "${blob}"
            COMMAND "${NR_LIGHTING_DXC}" -T ${stage}_6_0 -E ${stage}_main -HV 2021 -O3 -Gis
                -D KL_SHADOW_FIXTURE=1 -D KL_PCSS=${pcss}
                -spirv -fspv-target-env=vulkan1.2
                -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-b-shift 256 0
                -Fh "${blob}" -Vn lighting_filter_${name}
                "${NR_ROOT}/backend/shaders/lighting_shadow.hlsl"
            DEPENDS "${NR_ROOT}/backend/shaders/lighting_shadow.hlsl")
        list(APPEND lighting_test_blobs "${blob}")
    endforeach()
    add_executable(nr_lighting_tests lighting_tests.cpp vk_test_device.cpp readback.cpp ${lighting_test_blobs})
    target_include_directories(nr_lighting_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(nr_lighting_tests PRIVATE nr_renderer ${CMAKE_DL_LIBS})
    add_test(NAME nr_lighting_tests COMMAND nr_lighting_tests)
endif()
