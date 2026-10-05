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
)

if(WIN32)
    add_executable(king_kong WIN32 ${KINGKONG_SOURCES})
else()
    add_executable(king_kong ${KINGKONG_SOURCES})
endif()

rexglue_setup_target(king_kong GPU_PLUGINS xenos)
