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
    src/language.cpp
    src/intros.cpp
    src/fov.cpp
    src/motion_blur.cpp
    src/cheats.cpp
    src/changelog.cpp
    src/menu_hook.cpp
    src/glyphs.cpp
    src/crash_dump.cpp
    src/shader_pack.cpp
    src/http.cpp
    src/update.cpp
)

# Developer-only test aids (never in release builds): -DKK_DEV_TOOLS=ON.
option(KK_DEV_TOOLS "Build developer test aids such as KK_DEV_AUTOSKIP" OFF)
if(KK_DEV_TOOLS)
    list(APPEND KINGKONG_SOURCES src/dev_tools.cpp src/dev_watch.cpp)
endif()

if(WIN32)
    add_executable(king_kong WIN32 ${KINGKONG_SOURCES})
else()
    add_executable(king_kong ${KINGKONG_SOURCES})
endif()

rexglue_setup_target(king_kong GPU_PLUGINS xenos)
if(KK_DEV_TOOLS)
    target_compile_definitions(king_kong PRIVATE KK_DEV_TOOLS=1)
endif()

# Developer-only call profiler (preset kk-prof, never in release builds): every
# recompiled game function counts its calls (src/dev_prof.cpp).
option(KK_DEV_PROFILE "Count calls to every recompiled game function" OFF)
if(KK_DEV_PROFILE)
    target_sources(king_kong PRIVATE src/dev_prof.cpp)
    target_compile_definitions(king_kong PRIVATE KK_DEV_PROFILE=1)
    file(GLOB KK_RECOMP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/king_kong_recomp.*.cpp")
    set_property(SOURCE ${KK_RECOMP_SOURCES} APPEND PROPERTY COMPILE_OPTIONS -finstrument-functions-after-inlining)
endif()

# CHANGELOG.md, built into the program for the launcher (About page, "What's new").
set(KK_CHANGELOG_MD "${CMAKE_CURRENT_SOURCE_DIR}/../CHANGELOG.md")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KK_CHANGELOG_MD}")
file(READ "${KK_CHANGELOG_MD}" KK_CHANGELOG_TEXT)
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/changelog_text.cpp"
     CONTENT "namespace kk {
extern const char* const kChangelogText;
const char* const kChangelogText = R\"KKCHANGELOG(@KK_CHANGELOG_TEXT@)KKCHANGELOG\";
}
"
     @ONLY)
target_sources(king_kong PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/changelog_text.cpp")

# king_kong.map beside the program: turns addresses in players' crash and hang
# reports into game functions. Kept with each release build, not shipped.
if(WIN32)
    target_link_options(king_kong PRIVATE "LINKER:/MAP")
endif()

# The port's version: shown in the launcher and compared with GitHub releases by the updater.
# (-DKK_VERSION=x.y.z overrides it, to test the updater with an "older" build.)
if(NOT DEFINED KK_VERSION)
    set(KK_VERSION "1.7.2")
endif()
target_compile_definitions(king_kong PRIVATE KK_VERSION="${KK_VERSION}" KK_VERSION_W=L"${KK_VERSION}")

# Button prompt pictures (kk_button_prompts) and launcher art live beside the program.
add_custom_command(TARGET king_kong POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/glyphs $<TARGET_FILE_DIR:king_kong>/glyphs
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/launcher_art $<TARGET_FILE_DIR:king_kong>/launcher_art)
