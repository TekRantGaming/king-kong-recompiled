# Tests and test tools (standalone build only).
#   nr_draw_tests    milestone 3 without a GPU: recorded call sequences through
#                    the hook table, the NrApi binding and the tracker
#   nr_render_tests  the standalone test host: NVRHI on its own Vulkan device
#                    (lavapipe), milestones 1-3 rendered offscreen and read back
#   nr_trace_replay  replays a KK_DEV_D3D_TRACE log (a tool for real traces)

add_executable(nr_draw_tests draw_tests.cpp)
target_link_libraries(nr_draw_tests PRIVATE nr_draw nr_hooks)
target_compile_definitions(nr_draw_tests PRIVATE NR_SOURCE_DIR="${NR_ROOT}")
add_test(NAME nr_draw_tests COMMAND nr_draw_tests)

add_executable(nr_trace_replay trace_replay_main.cpp)
target_link_libraries(nr_trace_replay PRIVATE nr_draw nr_hooks)

if(NR_HAS_VULKAN)
    add_executable(nr_render_tests render_tests.cpp vk_test_device.cpp)
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
find_package(Python3 COMPONENTS Interpreter)
if(NR_SPIRV_VAL AND Python3_FOUND)
    add_test(NAME nr_spirv_val
             COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_SOURCE_DIR}/validate_spirv.py"
                     "${NR_SPIRV_VAL}" "${NR_ROOT}/backend/shaders/generated")
endif()
