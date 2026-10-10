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

add_executable(nr_trace_replay trace_replay_main.cpp)
target_link_libraries(nr_trace_replay PRIVATE nr_draw nr_hooks)

if(NR_HAS_VULKAN)
    add_executable(nr_render_tests render_tests.cpp vk_test_device.cpp readback.cpp)
    target_link_libraries(nr_render_tests PRIVATE nr_renderer nr_hooks ${CMAKE_DL_LIBS})
    add_test(NAME nr_render_tests COMMAND nr_render_tests)
    # Prefer lavapipe when several Vulkan drivers are installed.
    set(_lvp "/usr/share/vulkan/icd.d/lvp_icd.json")
    if(EXISTS "${_lvp}")
        set_tests_properties(nr_render_tests PROPERTIES ENVIRONMENT "VK_ICD_FILENAMES=${_lvp}")
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
        "${NR_ROOT}/backend/vulkan_host.cpp" "${NR_ROOT}/backend/backend.cpp")
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
