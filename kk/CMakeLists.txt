# king_kong - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(king_kong LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(KINGKONG_SOURCES
    src/main.cpp
    src/fpe_guard.cpp
    src/frame_stats.cpp
    src/sampler.cpp
    src/settings.cpp
    src/input_remap.cpp
    src/launcher.cpp
    src/platform.cpp
    src/iso.cpp
    src/art.cpp
    src/overlay.cpp
    src/toast.cpp
    src/io_fix.cpp
    src/menu_hook.cpp
    src/glyphs.cpp
    src/crash_dump.cpp
)

# Developer-only test aids (never in release builds): -DKK_DEV_TOOLS=ON.
option(KK_DEV_TOOLS "Build developer test aids such as KK_DEV_AUTOSKIP" OFF)
if(KK_DEV_TOOLS)
    list(APPEND KINGKONG_SOURCES src/dev_tools.cpp)
endif()

if(WIN32)
    add_executable(king_kong WIN32 ${KINGKONG_SOURCES})
else()
    add_executable(king_kong ${KINGKONG_SOURCES})
endif()

rexglue_setup_target(king_kong GPU_PLUGINS xenos)

# Button prompt pictures (kk_button_prompts) live beside the program.
add_custom_command(TARGET king_kong POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/glyphs $<TARGET_FILE_DIR:king_kong>/glyphs)
