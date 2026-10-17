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
    src/startup_recovery.cpp
    src/fpe_guard.cpp
    src/frame_stats.cpp
    src/handoff.cpp
    src/benchmark.cpp
    src/benchmark_monitor.cpp
    src/sampler.cpp
    src/settings.cpp
    src/input_remap.cpp
    src/launcher.cpp
    src/launcher_ui.cpp
    src/launcher_art.cpp
    src/launcher_text.cpp
    src/launcher_music.cpp
    src/launcher_sounds.cpp
    src/launcher_benchmark.cpp
    src/game_sound.cpp
    src/steam_shortcut.cpp
    src/platform.cpp
    src/display_modes.cpp
    src/iso.cpp
    src/art.cpp
    src/capture_png.cpp
    src/overlay.cpp
    src/debug_overlay.cpp
    src/toast.cpp
    src/io_fix.cpp
    src/loading_fix.cpp
    src/camera_shake.cpp
    src/hfr.cpp
    src/hfr_particles.cpp
    src/hfr_water.cpp
    src/hfr_dt_floor.cpp
    src/hfr_step_decay.cpp
    src/hfr_follow.cpp
    src/hfr_frame_time.cpp
    src/hfr_rowing.cpp
    src/hfr_splash.cpp
    src/hfr_fish.cpp
    src/hfr_sway.cpp
    src/hfr_cam_ground.cpp
    src/online.cpp
    src/telemetry.cpp
    src/telemetry_upload.cpp
    src/discord_presence.cpp
    src/mouse_look.cpp
    src/cursor_activity.cpp
    src/language.cpp
    src/intros.cpp
    src/fov.cpp
    src/freecam.cpp
    src/motion_blur.cpp
    src/fog.cpp
    src/big_blur.cpp
    src/cheats.cpp
    src/hardcore.cpp
    src/mods.cpp
    src/changelog.cpp
    src/menu_hook.cpp
    src/glyphs.cpp
    src/crash_dump.cpp
    src/diagnostics.cpp
    src/shader_pack.cpp
    src/native_shaders.cpp
    src/http.cpp
    src/content.cpp
    src/artbook_files.cpp
    src/artbook.cpp
    src/workshop.cpp
    src/workshop_mods.cpp
    src/workshop_markdown.cpp
    src/workshop_net.cpp
    src/system_notify.cpp
    src/update.cpp
    src/play_session.cpp
    src/ps2_movies.cpp
    src/profile_options.cpp
    src/profile_save_file.cpp
    src/pause_hooks.cpp
    src/pause_menu.cpp
)
# The game side of the launcher's Play menu and the pause menu (play_session.h) is play_session.cpp;
# until it is there, a stand-in with made-up saves lets the UI side build (play_session_stub.cpp).
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/play_session.cpp")
    list(APPEND KINGKONG_SOURCES src/play_session_stub.cpp)
endif()

# The restored PS2 cutscenes' decoder (src/ps2_movie_decoder.h): Media Foundation on Windows. On Linux, small
# LGPL builds of FFmpeg's H.264/AAC decoders (tools/build_ffmpeg_linux.sh; -DKK_FFMPEG_DIR=<its install
# folder>), linked dynamically and copied beside the program. Without them a Linux build has no PS2 cutscenes.
set(KK_FFMPEG_DIR "" CACHE PATH "Linux: install folder of tools/build_ffmpeg_linux.sh (PS2 cutscene playback)")
if(WIN32)
    set(KK_PS2_DECODER src/ps2_movie_decoder_mf.cpp)
elseif(KK_FFMPEG_DIR)
    set(KK_PS2_DECODER src/ps2_movie_decoder_ffmpeg.cpp)
    add_library(kk_ffmpeg INTERFACE)
    target_include_directories(kk_ffmpeg INTERFACE "${KK_FFMPEG_DIR}/include")
    foreach(lib avformat-kk avcodec-kk swscale-kk swresample-kk avutil-kk)
        find_library(KK_FFMPEG_LIB_${lib} NAMES ${lib} PATHS "${KK_FFMPEG_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
        target_link_libraries(kk_ffmpeg INTERFACE "${KK_FFMPEG_LIB_${lib}}")
    endforeach()
    # The files the program loads (by their sonames, e.g. libavcodec-kk.so.58), copied beside it.
    file(GLOB KK_FFMPEG_ALL "${KK_FFMPEG_DIR}/lib/lib*-kk.so.*")
    set(KK_FFMPEG_RUNTIME "")
    foreach(path ${KK_FFMPEG_ALL})
        if(path MATCHES "-kk\\.so\\.[0-9]+$")
            list(APPEND KK_FFMPEG_RUNTIME "${path}")
        endif()
    endforeach()
else()
    set(KK_PS2_DECODER src/ps2_movie_decoder_none.cpp)
    message(WARNING "KK_FFMPEG_DIR is not set: this Linux build cannot play the PS2 cutscenes "
                    "(tools/build_ffmpeg_linux.sh builds the libraries).")
endif()
list(APPEND KINGKONG_SOURCES ${KK_PS2_DECODER})

# Developer-only test aids (never in release builds): -DKK_DEV_TOOLS=ON.
option(KK_DEV_TOOLS "Build developer test aids such as KK_DEV_AUTOSKIP" OFF)
if(KK_DEV_TOOLS)
    list(APPEND KINGKONG_SOURCES src/dev_lod.cpp src/dev_aa.cpp)
    list(APPEND KINGKONG_SOURCES src/dev_tools.cpp src/dev_watch.cpp src/dev_ae.cpp src/dev_sound_trace.cpp src/dev_load_trace.cpp src/dev_frame_log.cpp src/dev_sample.cpp src/dev_venture.cpp src/dev_chapter_jump.cpp src/dev_pause.cpp src/dev_fish.cpp src/dev_clock_trace.cpp src/dev_camera_start.cpp)
endif()

if(WIN32)
    add_executable(king_kong WIN32 ${KINGKONG_SOURCES})
else()
    add_executable(king_kong ${KINGKONG_SOURCES})
endif()

rexglue_setup_target(king_kong)

# All game builds use the native renderer.
option(NR_BUILD_TESTS "Build native renderer tests" OFF)
set(NR_BUILD_PLUGIN ON CACHE BOOL "" FORCE)
# plugin/CMakeLists.txt copies rexgpu-native beside this target after each plugin build.
set(NR_PLUGIN_COPY_BESIDE king_kong)
add_subdirectory(../native-renderer native-renderer)
if(KK_DEV_TOOLS)
    target_compile_definitions(rexgpu-native PRIVATE KK_DEV_TOOLS=1)
endif()
target_sources(king_kong PRIVATE ../native-renderer/hooks/kk_native_hooks.cpp
                                 ../native-renderer/hooks/kk_native_ui.cpp)
target_link_libraries(king_kong PRIVATE nr_hooks)
# The runtime loads the plugin from beside the program.
add_dependencies(king_kong rexgpu-native)

if(TARGET kk_ffmpeg)
    target_link_libraries(king_kong PRIVATE kk_ffmpeg)
    foreach(path ${KK_FFMPEG_RUNTIME})
        add_custom_command(TARGET king_kong POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${path}" $<TARGET_FILE_DIR:king_kong>/)
    endforeach()
endif()
if(WIN32)
    target_link_libraries(king_kong PRIVATE windowscodecs ole32 pdh psapi runtimeobject shell32)
    # UTF-8 as the program's code page (src/king_kong.manifest, issue #37).
    target_link_options(king_kong PRIVATE "LINKER:/MANIFEST:EMBED" "LINKER:/MANIFESTUAC:NO"
                        "LINKER:/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/king_kong.manifest")
endif()
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
    set(KK_VERSION "2.0.0-beta.9")
endif()
target_compile_definitions(king_kong PRIVATE KK_VERSION="${KK_VERSION}" KK_VERSION_W=L"${KK_VERSION}")
execute_process(COMMAND git describe --always --dirty --abbrev=12 WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE KK_BENCH_BUILD OUTPUT_STRIP_TRAILING_WHITESPACE)
target_compile_definitions(king_kong PRIVATE KK_BENCH_BUILD="${KK_BENCH_BUILD}-task115")

if(KK_DEV_TOOLS)
    add_executable(kk-freecam-tests tests/freecam_tests.cpp)
    target_include_directories(kk-freecam-tests PRIVATE src)
    enable_testing()
    add_test(NAME free-camera COMMAND kk-freecam-tests)
    add_executable(kk-hfr-fish-tests tests/hfr_fish_tests.cpp)
    target_include_directories(kk-hfr-fish-tests PRIVATE src)
    add_test(NAME hfr-fish-wiggle COMMAND kk-hfr-fish-tests)
    add_executable(kk-hfr-blur-tests tests/hfr_blur_tests.cpp)
    target_include_directories(kk-hfr-blur-tests PRIVATE src)
    add_test(NAME hfr-motion-blur COMMAND kk-hfr-blur-tests)
    add_executable(kk-ps2-movie-tests tests/ps2_movies_tests.cpp)
    target_include_directories(kk-ps2-movie-tests PRIVATE src)
    enable_testing()
    add_test(NAME ps2-movie-logic COMMAND kk-ps2-movie-tests)
    # Decodes one private clip (KK_PS2_TEST_CLIP, _FRAMES, _SECONDS in the environment) with this platform's
    # decoder and checks frames, timestamps and audio; skipped without a clip (they are never in git).
    add_executable(kk-ps2-movie-decode-tests tests/ps2_movie_decode_tests.cpp ${KK_PS2_DECODER})
    target_include_directories(kk-ps2-movie-decode-tests PRIVATE src)
    if(TARGET kk_ffmpeg)
        target_link_libraries(kk-ps2-movie-decode-tests PRIVATE kk_ffmpeg)
        set_target_properties(kk-ps2-movie-decode-tests PROPERTIES BUILD_RPATH "${KK_FFMPEG_DIR}/lib")
    endif()
    add_test(NAME ps2-movie-decode COMMAND kk-ps2-movie-decode-tests)
    set_tests_properties(ps2-movie-decode PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(kk-hardware-tests tests/hardware_recommendation_tests.cpp src/settings.cpp)
    target_include_directories(kk-hardware-tests PRIVATE src)
    target_compile_definitions(kk-hardware-tests PRIVATE KK_VERSION="${KK_VERSION}")
    target_link_libraries(kk-hardware-tests PRIVATE rex::runtime)
    rexglue_apply_target_settings(kk-hardware-tests)
    enable_testing()
    add_test(NAME hardware-recommendation COMMAND kk-hardware-tests)
    # Launcher logic without a game or window: the shader job's progress bar, the options in every save.
    add_executable(kk-launcher-tests tests/launcher_logic_tests.cpp src/profile_save_file.cpp)
    target_include_directories(kk-launcher-tests PRIVATE src)
    rexglue_apply_target_settings(kk-launcher-tests)
    add_test(NAME launcher-logic COMMAND kk-launcher-tests)
    # Content downloads (texture pack, PS2 cutscenes): service, ZIP checks, resume, with a fake server.
    add_executable(kk-content-tests tests/content_tests.cpp src/content.cpp src/artbook_files.cpp)
    target_include_directories(kk-content-tests PRIVATE src)
    rexglue_apply_target_settings(kk-content-tests)
    add_test(NAME content-downloads COMMAND kk-content-tests "${CMAKE_CURRENT_BINARY_DIR}/content-test")
    add_executable(kk-menu-sound-tests tests/launcher_sounds_tests.cpp src/launcher_sounds.cpp)
    target_include_directories(kk-menu-sound-tests PRIVATE src)
    target_link_libraries(kk-menu-sound-tests PRIVATE rex::runtime)
    rexglue_apply_target_settings(kk-menu-sound-tests)
    add_test(NAME menu-sound-volume COMMAND kk-menu-sound-tests)
    # Job J3: when a download end dings and sends a system notification; the toast's XML.
    add_executable(kk-notify-tests tests/notify_tests.cpp)
    target_include_directories(kk-notify-tests PRIVATE src)
    rexglue_apply_target_settings(kk-notify-tests)
    add_test(NAME download-notifications COMMAND kk-notify-tests)
    # Job J8: the Workshop page: its Markdown subset, cache (ETag / once a day), GitHub addresses, README
    # pictures and picture decoding (no network: a fake server).
    add_executable(kk-workshop-tests tests/workshop_tests.cpp src/workshop_markdown.cpp src/workshop_net.cpp)
    target_include_directories(kk-workshop-tests PRIVATE src)
    rexglue_apply_target_settings(kk-workshop-tests)
    add_test(NAME workshop COMMAND kk-workshop-tests "${CMAKE_CURRENT_BINARY_DIR}/workshop-test")
    # Beta 7 Extras > Artbook: raw sectors to ISO, our ISO 9660 / Joliet reader (a synthetic disc), the readme's
    # Word text, titles, the Pictures folder and Save to PC, and the content service installing a synthetic disc.
    # Task 200: Kong mods: mod.toml for each type, broken files, version ranges, load order, conflicts, the plan.
    add_executable(kk-mods-tests tests/mods_tests.cpp)
    target_include_directories(kk-mods-tests PRIVATE src)
    target_link_libraries(kk-mods-tests PRIVATE rex::runtime)
    rexglue_apply_target_settings(kk-mods-tests)
    add_test(NAME kong-mods COMMAND kk-mods-tests)
    add_executable(kk-artbook-tests tests/artbook_tests.cpp src/artbook_files.cpp src/content.cpp src/workshop_net.cpp)
    target_include_directories(kk-artbook-tests PRIVATE src)
    target_compile_definitions(kk-artbook-tests PRIVATE KK_DEV_TOOLS=1)
    rexglue_apply_target_settings(kk-artbook-tests)
    add_test(NAME artbook COMMAND kk-artbook-tests "${CMAKE_CURRENT_BINARY_DIR}/artbook-test")
    add_executable(kk-slider-ui-tests tests/slider_ui_tests.cpp src/launcher_ui.cpp src/launcher_text.cpp)
    target_include_directories(kk-slider-ui-tests PRIVATE src)
    target_link_libraries(kk-slider-ui-tests PRIVATE rex::runtime)
    rexglue_apply_target_settings(kk-slider-ui-tests)
    add_test(NAME slider-controller-ui COMMAND kk-slider-ui-tests)
    # The benchmark's launcher pages with controller, keyboard and mouse; results files and pass rules.
    add_executable(kk-benchmark-ui-tests tests/benchmark_ui_tests.cpp src/launcher_ui.cpp src/launcher_text.cpp
        src/launcher_benchmark.cpp)
    target_include_directories(kk-benchmark-ui-tests PRIVATE src)
    target_link_libraries(kk-benchmark-ui-tests PRIVATE rex::runtime)
    rexglue_apply_target_settings(kk-benchmark-ui-tests)
    add_test(NAME benchmark-launcher-ui COMMAND kk-benchmark-ui-tests "${CMAKE_CURRENT_BINARY_DIR}/benchmark-ui-test")
    add_executable(kk-panel-tests tests/debug_overlay_tests.cpp src/launcher_text.cpp)
    target_include_directories(kk-panel-tests PRIVATE src)
    target_link_libraries(kk-panel-tests PRIVATE rex::runtime)
    rexglue_apply_target_settings(kk-panel-tests)
    add_test(NAME graphics-panel COMMAND kk-panel-tests)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME graphics-panel-text COMMAND "${Python3_EXECUTABLE}" -I
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/debug_overlay_text_tests.py" "${CMAKE_CURRENT_SOURCE_DIR}")
    # Task 144: settings categories' texts in five languages, no online reminder, one shader bar.
    add_test(NAME launcher-text COMMAND "${Python3_EXECUTABLE}" -I
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/launcher_text_tests.py" "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME benchmark-text COMMAND "${Python3_EXECUTABLE}" -I
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmark_text_tests.py" "${CMAKE_CURRENT_SOURCE_DIR}")
    # Task 194: the Beta 7 uploads (redaction, gzip, size limits, switch off = nothing sent), then Python's
    # gzip reads back what the game's gzip wrote.
    add_executable(kk-telemetry-tests tests/telemetry_tests.cpp src/telemetry_upload.cpp)
    target_include_directories(kk-telemetry-tests PRIVATE src)
    rexglue_apply_target_settings(kk-telemetry-tests)
    add_test(NAME telemetry-uploads COMMAND kk-telemetry-tests "${CMAKE_CURRENT_BINARY_DIR}/telemetry-test")
    set_tests_properties(telemetry-uploads PROPERTIES FIXTURES_SETUP telemetry_files)
    # Task 197: the crash/hang report's log tail (fixed ring of 200 lines, redaction as for uploads).
    add_executable(kk-log-tail-tests tests/log_tail_tests.cpp)
    target_include_directories(kk-log-tail-tests PRIVATE src)
    rexglue_apply_target_settings(kk-log-tail-tests)
    add_test(NAME log-tail COMMAND kk-log-tail-tests)
    add_test(NAME telemetry-gzip COMMAND "${Python3_EXECUTABLE}" -I
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/telemetry_gzip_tests.py" "${CMAKE_CURRENT_BINARY_DIR}/telemetry-test")
    set_tests_properties(telemetry-gzip PROPERTIES FIXTURES_REQUIRED telemetry_files)
    add_executable(kk-startup-tests ../native-renderer/tests/startup-recovery-039.cpp)
    add_test(NAME startup-recovery COMMAND kk-startup-tests "${CMAKE_CURRENT_BINARY_DIR}/startup-test-state")
endif()
if(KK_DEV_TOOLS OR NR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/pacing-bench)
endif()

# Button prompt pictures (kk_button_prompts) live beside the program.
# Task 200: the built-in Kong mods and the modding guide ship beside the program (packages copy them too).
add_custom_command(TARGET king_kong POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/mods-builtin $<TARGET_FILE_DIR:king_kong>/mods-builtin
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/../MODDING.md $<TARGET_FILE_DIR:king_kong>/MODDING.md)
add_custom_command(TARGET king_kong POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/glyphs $<TARGET_FILE_DIR:king_kong>/glyphs
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/THIRD-PARTY-NOTICES.txt
            $<TARGET_FILE_DIR:king_kong>/THIRD-PARTY-NOTICES.txt)
