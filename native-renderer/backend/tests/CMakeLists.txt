# Tests and test tools (standalone build only).
#   nr_draw_tests    milestone 3 without a GPU: recorded call sequences through
#                    the hook table, the NrApi binding and the tracker
#   nr_render_tests  the standalone test host: NVRHI on its own Vulkan device
#                    (lavapipe), milestones 1-3 rendered offscreen and read back
#   nr_trace_replay  replays a KK_DEV_D3D_TRACE log (a tool for real traces)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

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

add_executable(nr_ao_rules_tests ao_rules_tests.cpp)
target_link_libraries(nr_ao_rules_tests PRIVATE nr_draw)
add_test(NAME nr_ao_rules_tests COMMAND nr_ao_rules_tests)

add_executable(nr_upscale_tests upscale_tests.cpp)
target_link_libraries(nr_upscale_tests PRIVATE nr_draw)
add_test(NAME nr_upscale_tests COMMAND nr_upscale_tests)

add_executable(nr_trace_replay trace_replay_main.cpp)
target_link_libraries(nr_trace_replay PRIVATE nr_draw nr_hooks)

if(NR_HAS_VULKAN)
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
