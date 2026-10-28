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
    src/coop_direct_udp.cpp
    src/coop_protocol.cpp
    src/coop_session.cpp
    src/coop_flow_game.cpp
    src/coop_story_game.cpp
    src/coop_objects_game.cpp
    src/coop_desync_game.cpp
    src/coop_world_snapshot.cpp
    src/coop_runtime.cpp
    src/coop_resume.cpp
    src/coop_partner_body.cpp
    src/coop_nametag.cpp
    src/coop_observer.cpp
    src/coop_enemies_game.cpp
    src/steam_runtime.cpp
    src/steam_lobby.cpp
    src/steam_transport.cpp
    src/steam_pad.cpp
    src/steam_pad_driver.cpp
    src/social_panel.cpp
    src/social_panel_logic.cpp
    src/social_fake.cpp
    src/input_remap.cpp
    src/input_diag.cpp
    src/launcher.cpp
    src/launcher_ui.cpp
    src/launcher_art.cpp
    src/launcher_text.cpp
    src/launcher_music.cpp
    src/launcher_sounds.cpp
    src/easter_egg.cpp
    src/launcher_benchmark.cpp
    src/game_sound.cpp
    src/steam_shortcut.cpp
    src/platform.cpp
    src/hdr_display.cpp
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
    src/hfr_kong_lift.cpp
    src/hfr_splash.cpp
    src/hfr_fish.cpp
    src/hfr_sway.cpp
    src/hfr_cam_ground.cpp
    src/fix_raptor_path.cpp
    src/prompt_anim.cpp
    src/online.cpp
    src/telemetry.cpp
    src/telemetry_upload.cpp
    src/discord_presence.cpp
    src/mouse_look.cpp
    src/cursor_activity.cpp
    src/language.cpp
    src/intros.cpp
    src/fov.cpp
    src/ultrawide.cpp
    src/freecam.cpp
    src/motion_blur.cpp
    src/fog.cpp
    src/big_blur.cpp
    src/cheats.cpp
    src/hardcore.cpp
    src/mods.cpp
    src/changelog.cpp
    src/menu_hook.cpp
    src/menu_scene.cpp
    src/loading_loop.cpp
    src/startup_movies.cpp
    src/glyphs.cpp
    src/crash_dump.cpp
    src/exit_evidence.cpp
    src/diagnostics.cpp
    src/shader_pack.cpp
    src/native_shaders.cpp
    src/http.cpp
    src/content.cpp
    src/artbook_files.cpp
    src/artbook.cpp
    src/pdf_pages.cpp
    src/workshop.cpp
    src/workshop_mods.cpp
    src/workshop_texpacks.cpp
    src/workshop_markdown.cpp
    src/workshop_net.cpp
    src/system_notify.cpp
    src/update.cpp
    src/countdown.cpp
    src/update_files.cpp
    src/play_session.cpp
    src/ps2_movies.cpp
    src/profile_options.cpp
    src/profile_save_file.cpp
    src/pause_hooks.cpp
    src/pause_menu.cpp
    src/photo_mode.cpp
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

option(KK_COOP_DEFAULT_ON "Enable co-op by default unless settings or command line override it" OFF)

# Developer-only test aids (never in release builds): -DKK_DEV_TOOLS=ON.
option(KK_DEV_TOOLS "Build developer test aids such as KK_DEV_AUTOSKIP" OFF)
if(KK_DEV_TOOLS)
    list(APPEND KINGKONG_SOURCES src/dev_chapter_prepare.cpp)
    list(APPEND KINGKONG_SOURCES src/dev_lod.cpp src/dev_aa.cpp)
    list(APPEND KINGKONG_SOURCES src/coop_transport_udp.cpp)
    list(APPEND KINGKONG_SOURCES src/dev_puppet_mesh.cpp)
    list(APPEND KINGKONG_SOURCES src/dev_puppet_variant.cpp)
    list(APPEND KINGKONG_SOURCES src/dev_tools.cpp src/dev_watch.cpp src/dev_ae.cpp src/dev_sound_trace.cpp src/dev_load_trace.cpp src/dev_frame_log.cpp src/dev_sample.cpp src/dev_venture.cpp src/dev_chapter_jump.cpp src/dev_pause.cpp src/dev_fish.cpp src/dev_clock_trace.cpp src/dev_camera_start.cpp src/dev_game_watch.cpp src/dev_teleport.cpp src/dev_trig_trace.cpp src/dev_bug_trace.cpp)
    list(APPEND KINGKONG_SOURCES src/dev_kong_lift.cpp)
endif()

if(WIN32)
    add_executable(king_kong WIN32 ${KINGKONG_SOURCES})
else()
    add_executable(king_kong ${KINGKONG_SOURCES})
endif()

rexglue_setup_target(king_kong)

# Optional Steamworks SDK, kept outside the checkout. No import library is linked.
set(KK_STEAMWORKS_DEFAULT "")
if(EXISTS "F:/kk-native-coop/sdk/steamworks_sdk_165/sdk/public/steam/steam_api_flat.h")
    set(KK_STEAMWORKS_DEFAULT "F:/kk-native-coop/sdk/steamworks_sdk_165/sdk")
endif()
set(KK_STEAMWORKS_SDK "${KK_STEAMWORKS_DEFAULT}" CACHE PATH "External Steamworks SDK sdk folder (empty disables Steam)")
if(WIN32)
    set(KK_STEAM_REDIST "${KK_STEAMWORKS_SDK}/redistributable_bin/win64/steam_api64.dll")
elseif(APPLE)
    set(KK_STEAM_REDIST "${KK_STEAMWORKS_SDK}/redistributable_bin/osx/libsteam_api.dylib")
else()
    set(KK_STEAM_REDIST "${KK_STEAMWORKS_SDK}/redistributable_bin/linux64/libsteam_api.so")
endif()
set(KK_HAS_STEAM 0)
if(KK_STEAMWORKS_SDK AND EXISTS "${KK_STEAMWORKS_SDK}/public/steam/steam_api_flat.h"
        AND EXISTS "${KK_STEAMWORKS_SDK}/public/steam/steam_api.h" AND EXISTS "${KK_STEAM_REDIST}")
    set(KK_HAS_STEAM 1)
    target_include_directories(king_kong SYSTEM PRIVATE "${KK_STEAMWORKS_SDK}/public")
    add_custom_command(TARGET king_kong POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KK_STEAM_REDIST}" "$<TARGET_FILE_DIR:king_kong>"
        VERBATIM)
endif()
target_compile_definitions(king_kong PRIVATE KK_HAS_STEAM=${KK_HAS_STEAM})
find_package(Threads REQUIRED)
target_link_libraries(king_kong PRIVATE Threads::Threads ${CMAKE_DL_LIBS})

# All game builds use the native renderer.
option(NR_BUILD_TESTS "Build native renderer tests" OFF)
set(NR_BUILD_PLUGIN ON CACHE BOOL "" FORCE)
# plugin/CMakeLists.txt copies rexgpu-native beside this target after each plugin build.
set(NR_PLUGIN_COPY_BESIDE king_kong)
add_subdirectory(../native-renderer native-renderer)
if(KK_DEV_TOOLS)
    target_compile_definitions(rexgpu-native PRIVATE KK_DEV_TOOLS=1)
    target_compile_definitions(nr_draw PRIVATE KK_DEV_TOOLS=1)
endif()
target_sources(king_kong PRIVATE ../native-renderer/hooks/kk_native_hooks.cpp
                                 ../native-renderer/hooks/kk_native_ui.cpp
                                 ../native-renderer/hooks/kk_native_photo.cpp)
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
# Task 333: the user manual's pages are drawn by PDFium (src/pdf_pages.h; BSD 3-Clause / Apache 2.0, the
# bblanchon/pdfium-binaries build without JavaScript or XFA), loaded at run time from beside the program. Its
# release folder for this platform (bin/pdfium.dll, lib/libpdfium.so): -DKK_PDFIUM_DIR=<folder>, the KK_PDFIUM_DIR
# environment variable, or tools/pdfium/<win-x64|linux-x64> (a link to F:\KK-native-renderer\downloads\
# pdfium-binaries-chromium-8076 on the build PC; never in git). Copied beside the program; packages must ship it.
# Without it the program runs and Extras > User manual says the manual cannot be shown.
if(WIN32)
    set(_kk_pdfium_platform win-x64)
    set(_kk_pdfium_lib bin/pdfium.dll)
else()
    set(_kk_pdfium_platform linux-x64)
    set(_kk_pdfium_lib lib/libpdfium.so)
endif()
set(_kk_pdfium_default "$ENV{KK_PDFIUM_DIR}")
if(NOT _kk_pdfium_default AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../tools/pdfium/${_kk_pdfium_platform}/${_kk_pdfium_lib}")
    set(_kk_pdfium_default "${CMAKE_CURRENT_SOURCE_DIR}/../tools/pdfium/${_kk_pdfium_platform}")
endif()
set(KK_PDFIUM_DIR "${_kk_pdfium_default}" CACHE PATH "PDFium release folder (bin/pdfium.dll or lib/libpdfium.so) for the user manual")
if(KK_PDFIUM_DIR AND EXISTS "${KK_PDFIUM_DIR}/${_kk_pdfium_lib}")
    message(STATUS "King Kong: ${KK_PDFIUM_DIR}/${_kk_pdfium_lib} goes beside the game (the user manual)")
    add_custom_command(TARGET king_kong POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KK_PDFIUM_DIR}/${_kk_pdfium_lib}" $<TARGET_FILE_DIR:king_kong>/)
else()
    message(WARNING "King Kong: no PDFium (set KK_PDFIUM_DIR): Extras > User manual cannot show the manual")
endif()
if(WIN32)
    target_link_libraries(king_kong PRIVATE windowscodecs ole32 pdh psapi runtimeobject shell32)
    target_link_libraries(king_kong PRIVATE ws2_32)
    # UTF-8 as the program's code page (src/king_kong.manifest, issue #37).
    target_link_options(king_kong PRIVATE "LINKER:/MANIFEST:EMBED" "LINKER:/MANIFESTUAC:NO"
                        "LINKER:/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/src/king_kong.manifest")
endif()
if(KK_DEV_TOOLS)
    target_compile_definitions(king_kong PRIVATE KK_DEV_TOOLS=1)
    execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                    OUTPUT_VARIABLE KK_WORLD_SOURCE_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE)
    configure_file(src/coop_world_build.h.in "${CMAKE_CURRENT_BINARY_DIR}/coop_world_build.h" @ONLY)
    target_include_directories(king_kong PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
endif()
# Only settings.cpp needs the default; other sources use the resolved cvar.
set_property(SOURCE src/settings.cpp APPEND PROPERTY COMPILE_DEFINITIONS
    KK_COOP_DEFAULT_ON=$<BOOL:${KK_COOP_DEFAULT_ON}>)

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
    set(KK_VERSION "2.0.0-beta.17")
endif()
# The update channel the build follows (update_channel.h): "stable" (only the stable repo, never the
# beta channel) or "beta". Empty: from KK_VERSION ("-beta" in it: beta). The kk-stable preset sets
# stable together with the stable version number; a stable build with a beta version is refused.
set(KK_RELEASE_CHANNEL "" CACHE STRING "Update channel of this build: stable, beta, or empty (from KK_VERSION)")
if(KK_RELEASE_CHANNEL STREQUAL "")
    if(KK_VERSION MATCHES "-beta")
        set(KK_BUILD_CHANNEL "beta")
    else()
        set(KK_BUILD_CHANNEL "stable")
    endif()
elseif(KK_RELEASE_CHANNEL STREQUAL "stable" OR KK_RELEASE_CHANNEL STREQUAL "beta")
    set(KK_BUILD_CHANNEL "${KK_RELEASE_CHANNEL}")
else()
    message(FATAL_ERROR "KK_RELEASE_CHANNEL must be stable, beta or empty (got '${KK_RELEASE_CHANNEL}')")
endif()
if(KK_BUILD_CHANNEL STREQUAL "stable" AND KK_VERSION MATCHES "-beta")
    message(FATAL_ERROR "A stable build needs a stable version (KK_VERSION is ${KK_VERSION})")
endif()
message(STATUS "King Kong ${KK_VERSION}, update channel ${KK_BUILD_CHANNEL}")
target_compile_definitions(king_kong PRIVATE KK_VERSION="${KK_VERSION}" KK_VERSION_W=L"${KK_VERSION}"
    KK_BUILD_CHANNEL="${KK_BUILD_CHANNEL}")
execute_process(COMMAND git describe --always --dirty --abbrev=12 WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE KK_BENCH_BUILD OUTPUT_STRIP_TRAILING_WHITESPACE)
target_compile_definitions(king_kong PRIVATE KK_BENCH_BUILD="${KK_BENCH_BUILD}-task115")

# Include dirty source contents, not just the commit id, in co-op compatibility.
set(KK_COOP_SOURCE_ID "${KK_BENCH_BUILD};${KK_VERSION};${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION};${CMAKE_BUILD_TYPE};${CMAKE_CXX_FLAGS};${KK_DEV_TOOLS};${KK_HAS_STEAM}")
foreach(source IN LISTS KINGKONG_SOURCES)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
    file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${source}" source_hash)
    string(APPEND KK_COOP_SOURCE_ID ";${source_hash}")
endforeach()
file(GLOB KK_COOP_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")
foreach(header IN LISTS KK_COOP_HEADERS)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${header}")
    file(SHA256 "${header}" source_hash)
    string(APPEND KK_COOP_SOURCE_ID ";${source_hash}")
endforeach()
string(SHA256 KK_COOP_BUILD "${KK_COOP_SOURCE_ID}")
target_compile_definitions(king_kong PRIVATE KK_COOP_BUILD="${KK_COOP_BUILD}")

if(KK_DEV_TOOLS)
    add_executable(kk-chapter-texture-prepare-tests tests/chapter_texture_prepare_tests.cpp)
    target_include_directories(kk-chapter-texture-prepare-tests PRIVATE ../native-renderer)
    target_link_libraries(kk-chapter-texture-prepare-tests PRIVATE kknr_resources)
    add_test(NAME chapter-texture-prepare COMMAND kk-chapter-texture-prepare-tests)
    add_executable(kk-freecam-tests tests/freecam_tests.cpp)
    enable_testing()
    add_executable(kk-coop-tests tests/coop_tests.cpp src/coop_protocol.cpp src/coop_session.cpp)
    target_include_directories(kk-coop-tests PRIVATE src)
    add_test(NAME coop-net-core COMMAND kk-coop-tests)
    add_executable(kk-direct-tests tests/direct_udp_tests.cpp src/coop_direct_udp.cpp src/coop_protocol.cpp src/coop_session.cpp src/steam_lobby.cpp)
    target_include_directories(kk-direct-tests PRIVATE src)
    target_compile_definitions(kk-direct-tests PRIVATE KK_DEV_TOOLS=1)
    target_link_libraries(kk-direct-tests PRIVATE Threads::Threads)
    if(WIN32)
        target_link_libraries(kk-direct-tests PRIVATE ws2_32)
    endif()
    add_test(NAME coop-direct-udp COMMAND kk-direct-tests)
    add_executable(kk-rejoin-tests tests/coop_rejoin_tests.cpp src/coop_direct_udp.cpp src/coop_protocol.cpp
        src/coop_session.cpp src/steam_lobby.cpp src/steam_transport.cpp)
    target_include_directories(kk-rejoin-tests PRIVATE src)
    target_compile_definitions(kk-rejoin-tests PRIVATE KK_DEV_TOOLS=1)
    target_link_libraries(kk-rejoin-tests PRIVATE Threads::Threads)
    if(WIN32)
        target_link_libraries(kk-rejoin-tests PRIVATE ws2_32)
    endif()
    add_test(NAME coop-rejoin COMMAND kk-rejoin-tests)
    # Mutation code exists only in this separately compiled actual-core test binary.
    add_executable(kk-chaos-tests tests/coop_chaos_tests.cpp src/coop_direct_udp.cpp
        src/coop_protocol.cpp src/coop_session.cpp src/steam_lobby.cpp)
    target_include_directories(kk-chaos-tests PRIVATE src)
    target_compile_definitions(kk-chaos-tests PRIVATE KK_DEV_TOOLS=1 COOP_RUNTIME_MUT=1 KK_COOP_MUTATION_TEST=1)
    target_link_libraries(kk-chaos-tests PRIVATE Threads::Threads)
    if(WIN32)
        target_link_libraries(kk-chaos-tests PRIVATE ws2_32)
    endif()
    foreach(case model profiles outages ownership_generation story_duplicate story_world
            digest_compare false_quiet shot_validator session_epoch udp_duplicate udp_order
            projectile_source impact_selector grab_feedback attack_sound grab_start native_script_target
            bat_contact_sound raptor_camera raptor_bite_sound centipede_contact_sound body_hit_class after_contact_sound
            story_native_stop
            event_pause event_dispatch event_owner_cap event_vision_guard event_network_defer
            unarmed_gait placement_floor placement_sight placement_wait void_guard)
        add_test(NAME coop-chaos-${case} COMMAND kk-chaos-tests ${case})
        set_tests_properties(coop-chaos-${case} PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE ENVIRONMENT "COOP_MUT=")
    endforeach()
    add_executable(kk-camera-tests tests/coop_camera_tests.cpp src/coop_protocol.cpp src/coop_session.cpp)
    target_include_directories(kk-camera-tests PRIVATE src)
    add_test(NAME coop-observer-camera COMMAND kk-camera-tests)
    add_executable(kk-session-tests tests/coop_flow_tests.cpp src/coop_protocol.cpp src/coop_session.cpp)
    target_include_directories(kk-session-tests PRIVATE src)
    target_compile_definitions(kk-session-tests PRIVATE KK_DEV_TOOLS=1)
    add_test(NAME coop-session-flow COMMAND kk-session-tests)
    add_executable(kk-objects-tests tests/coop_objects_tests.cpp src/coop_protocol.cpp src/coop_session.cpp)
    target_include_directories(kk-objects-tests PRIVATE src)
    add_test(NAME coop-world-objects COMMAND kk-objects-tests)
    add_executable(kk-handover-tests tests/coop_handover_tests.cpp src/coop_protocol.cpp src/coop_session.cpp)
    target_include_directories(kk-handover-tests PRIVATE src)
    add_test(NAME coop-handover COMMAND kk-handover-tests)
    add_executable(kk-story-tests tests/coop_story_tests.cpp src/coop_protocol.cpp src/coop_session.cpp)
    target_include_directories(kk-story-tests PRIVATE src)
    add_test(NAME coop-story-sync COMMAND kk-story-tests)
    add_executable(kk-desync-tests tests/coop_desync_tests.cpp src/coop_protocol.cpp src/coop_session.cpp)
    target_include_directories(kk-desync-tests PRIVATE src)
    add_test(NAME coop-desync-guard COMMAND kk-desync-tests)
    add_executable(kk-enemies-tests tests/coop_enemies_tests.cpp src/coop_protocol.cpp src/coop_session.cpp)
    target_include_directories(kk-enemies-tests PRIVATE src)
    add_test(NAME coop-enemies COMMAND kk-enemies-tests)
    add_executable(kk-combat-tests tests/coop_combat_tests.cpp src/coop_protocol.cpp src/coop_session.cpp)
    target_include_directories(kk-combat-tests PRIVATE src)
    add_test(NAME coop-combat COMMAND kk-combat-tests)
    add_executable(kk-puppet-mesh-tests tests/puppet_mesh_probe_tests.cpp)
    target_include_directories(kk-puppet-mesh-tests PRIVATE ../native-renderer)
    target_compile_features(kk-puppet-mesh-tests PRIVATE cxx_std_20)
    target_link_libraries(kk-puppet-mesh-tests PRIVATE nr_draw Threads::Threads)
    add_test(NAME coop-puppet-mesh COMMAND kk-puppet-mesh-tests)
    # Always SDK-free, including the real runtime's compile-time unavailable branch.
    add_executable(kk-steam-tests tests/steam_tests.cpp src/steam_lobby.cpp src/steam_transport.cpp src/steam_pad.cpp
        src/steam_runtime.cpp src/coop_protocol.cpp src/coop_session.cpp)
    target_include_directories(kk-steam-tests PRIVATE src)
    target_compile_definitions(kk-steam-tests PRIVATE KK_HAS_STEAM=0 KK_DEV_TOOLS=1)
    target_link_libraries(kk-steam-tests PRIVATE Threads::Threads)
    if(WIN32)
        target_link_libraries(kk-steam-tests PRIVATE shell32)
    endif()
    add_test(NAME coop-steam-services COMMAND kk-steam-tests)
    add_executable(kk-social-tests tests/social_tests.cpp src/social_panel_logic.cpp src/social_fake.cpp
        src/steam_lobby.cpp src/coop_protocol.cpp)
    target_include_directories(kk-social-tests PRIVATE src)
    target_compile_definitions(kk-social-tests PRIVATE KK_DEV_TOOLS=1)
    add_test(NAME coop-social-panel COMMAND kk-social-tests)
    add_executable(kk-social-ui-tests tests/social_ui_tests.cpp src/social_panel.cpp src/social_panel_logic.cpp
        src/launcher_ui.cpp src/launcher_text.cpp src/coop_direct_udp.cpp src/coop_protocol.cpp src/steam_lobby.cpp src/steam_pad.cpp)
    target_include_directories(kk-social-ui-tests PRIVATE src)
    target_compile_definitions(kk-social-ui-tests PRIVATE KK_DEV_TOOLS=1)
    target_link_libraries(kk-social-ui-tests PRIVATE rex::runtime)
    if(WIN32)
      target_link_libraries(kk-social-ui-tests PRIVATE ws2_32)
    endif()
    rexglue_apply_target_settings(kk-social-ui-tests)
    add_test(NAME coop-social-controller COMMAND kk-social-ui-tests)
    target_include_directories(kk-freecam-tests PRIVATE src)
    add_test(NAME free-camera COMMAND kk-freecam-tests)
    # Task 208: photo mode's settings, camera limits, controller, hidden draws and grade colours; its text.
    add_executable(kk-photo-mode-tests tests/photo_mode_tests.cpp ../native-renderer/backend/photo_config.cpp)
    target_include_directories(kk-photo-mode-tests PRIVATE src ../native-renderer)
    target_compile_features(kk-photo-mode-tests PRIVATE cxx_std_20)
    add_test(NAME photo-mode COMMAND kk-photo-mode-tests)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME photo-mode-text COMMAND "${Python3_EXECUTABLE}" -I
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/photo_text_tests.py" "${CMAKE_CURRENT_SOURCE_DIR}")
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
    target_compile_definitions(kk-hardware-tests PRIVATE KK_VERSION="${KK_VERSION}" KK_BUILD_CHANNEL="${KK_BUILD_CHANNEL}")
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
    # Task 370: the updater's file work (kk-files.txt, staged install with undo, retired files, the first start
    # check) on real files in a temp folder; also the packaging simulation's helper (codex\simulate-update.ps1).
    add_executable(kk-update-tests tests/update_files_tests.cpp src/update_files.cpp)
    target_include_directories(kk-update-tests PRIVATE src)
    rexglue_apply_target_settings(kk-update-tests)
    add_test(NAME update-files COMMAND kk-update-tests "${CMAKE_CURRENT_BINARY_DIR}/update-test")
    add_executable(kk-menu-sound-tests tests/launcher_sounds_tests.cpp src/launcher_sounds.cpp)
    target_include_directories(kk-menu-sound-tests PRIVATE src)
    target_link_libraries(kk-menu-sound-tests PRIVATE rex::runtime)
    target_compile_definitions(kk-menu-sound-tests PRIVATE KK_DEV_TOOLS=1)
    rexglue_apply_target_settings(kk-menu-sound-tests)
    add_test(NAME menu-sound-volume COMMAND kk-menu-sound-tests)
    set_tests_properties(menu-sound-volume PROPERTIES TIMEOUT 10)
    # Job J3: when a download end dings and sends a system notification; the toast's XML.
    add_executable(kk-notify-tests tests/notify_tests.cpp)
    target_include_directories(kk-notify-tests PRIVATE src)
    rexglue_apply_target_settings(kk-notify-tests)
    add_test(NAME download-notifications COMMAND kk-notify-tests)
    # Task 229: evidence when the program ends without a word (session marker, Windows' crash / hang events,
    # rate-limited focus-loss lines).
    add_executable(kk-exit-evidence-tests tests/exit_evidence_tests.cpp)
    target_include_directories(kk-exit-evidence-tests PRIVATE src)
    rexglue_apply_target_settings(kk-exit-evidence-tests)
    add_test(NAME exit-evidence COMMAND kk-exit-evidence-tests)
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
    # Task 209: one texture pack on at a time (the HD pack, the player's folder, texture pack mods).
    # Task 212: plus the Workshop downloads' table and their texts in five languages (the launcher's lookup).
    add_executable(kk-texture-packs-tests tests/texture_packs_tests.cpp src/launcher_text.cpp)
    target_include_directories(kk-texture-packs-tests PRIVATE src)
    target_compile_features(kk-texture-packs-tests PRIVATE cxx_std_20)
    target_link_libraries(kk-texture-packs-tests PRIVATE rex::runtime)
    rexglue_apply_target_settings(kk-texture-packs-tests)
    add_test(NAME texture-pack-rules COMMAND kk-texture-packs-tests)
    add_executable(kk-artbook-tests tests/artbook_tests.cpp src/artbook_files.cpp src/content.cpp src/workshop_net.cpp)
    target_include_directories(kk-artbook-tests PRIVATE src)
    target_compile_definitions(kk-artbook-tests PRIVATE KK_DEV_TOOLS=1)
    rexglue_apply_target_settings(kk-artbook-tests)
    add_test(NAME artbook COMMAND kk-artbook-tests "${CMAKE_CURRENT_BINARY_DIR}/artbook-test")
    add_executable(kk-slider-ui-tests tests/slider_ui_tests.cpp src/launcher_ui.cpp src/steam_pad.cpp src/launcher_text.cpp)
    target_include_directories(kk-slider-ui-tests PRIVATE src)
    target_link_libraries(kk-slider-ui-tests PRIVATE rex::runtime)
    rexglue_apply_target_settings(kk-slider-ui-tests)
    add_test(NAME slider-controller-ui COMMAND kk-slider-ui-tests)
    # The benchmark's launcher pages with controller, keyboard and mouse; results files and pass rules.
    add_executable(kk-benchmark-ui-tests tests/benchmark_ui_tests.cpp src/launcher_ui.cpp src/steam_pad.cpp src/launcher_text.cpp
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
    # Task 220: every settings row's description panel help (settings_help.h) in five languages, and its fit.
    add_test(NAME settings-help-text COMMAND "${Python3_EXECUTABLE}" -I
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/settings_help_tests.py" "${CMAKE_CURRENT_SOURCE_DIR}")
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
    # Task 368: the overlays a crash report names (overlay_modules.h).
    add_executable(kk-overlay-modules-tests tests/overlay_modules_tests.cpp)
    target_include_directories(kk-overlay-modules-tests PRIVATE src)
    rexglue_apply_target_settings(kk-overlay-modules-tests)
    add_test(NAME overlay-modules COMMAND kk-overlay-modules-tests)
    add_executable(kk-background-log-tests tests/background_log_tests.cpp)
    target_include_directories(kk-background-log-tests PRIVATE src)
    target_link_libraries(kk-background-log-tests PRIVATE rex::runtime)
    rexglue_apply_target_settings(kk-background-log-tests)
    add_test(NAME background-log-output COMMAND kk-background-log-tests)
    set_tests_properties(background-log-output PROPERTIES TIMEOUT 20)
    # Task 297: the startup limits leave out shader compile time; the renderer's compile clock.
    add_executable(kk-startup-clock-tests tests/startup_clock_tests.cpp)
    target_include_directories(kk-startup-clock-tests PRIVATE src ../native-renderer)
    rexglue_apply_target_settings(kk-startup-clock-tests)
    add_test(NAME startup-clock COMMAND kk-startup-clock-tests)
    # Task 342: device open cannot hold the game thread; cancelled opens never
    # retain the SDK client semaphore, and current PCM takes over without replay.
    add_executable(kk-deferred-audio-tests tests/deferred_audio_tests.cpp)
    target_include_directories(kk-deferred-audio-tests PRIVATE src)
    target_link_libraries(kk-deferred-audio-tests PRIVATE rex::runtime)
    rexglue_apply_target_settings(kk-deferred-audio-tests)
    add_test(NAME deferred-audio COMMAND kk-deferred-audio-tests)
    set_tests_properties(deferred-audio PROPERTIES TIMEOUT 20)
    # Task 206: brightness and gamma (the renderer's picture curve), the Performance metrics overlay's logic.
    add_executable(kk-perf-overlay-tests tests/perf_overlay_tests.cpp)
    target_include_directories(kk-perf-overlay-tests PRIVATE src ../native-renderer)
    rexglue_apply_target_settings(kk-perf-overlay-tests)
    add_test(NAME picture-and-metrics COMMAND kk-perf-overlay-tests)
    # Task 222: the pause menu's layout (option A): entries, order, names, the quiet group, LB / RB.
    add_executable(kk-pause-menu-tests tests/pause_menu_tests.cpp)
    target_include_directories(kk-pause-menu-tests PRIVATE src)
    target_compile_features(kk-pause-menu-tests PRIVATE cxx_std_20)
    add_test(NAME pause-menu-layout COMMAND kk-pause-menu-tests)
    # Task 251: cheats that are off in some chapters (the weapon cheats in Hayes, level 303) and the greyed note.
    add_executable(kk-cheat-rules-tests tests/cheat_rules_tests.cpp)
    target_include_directories(kk-cheat-rules-tests PRIVATE src)
    target_compile_features(kk-cheat-rules-tests PRIVATE cxx_std_20)
    add_test(NAME cheat-rules COMMAND kk-cheat-rules-tests)
    # Task 330: the live main menu scene (when it runs, the failure marker, Start on the title, readiness); task 331:
    # the loading loops' rules (loading_loop_logic.h).
    add_executable(kk-menu-scene-tests tests/menu_scene_tests.cpp)
    target_include_directories(kk-menu-scene-tests PRIVATE src)
    target_compile_features(kk-menu-scene-tests PRIVATE cxx_std_20)
    add_test(NAME menu-scene COMMAND kk-menu-scene-tests)
    # Task 219: HDR follows Windows (the default decision, the calibration profile's brightness, automatic
    # levels; prints what this PC's screen reports) and the setup's comparison pictures made from the game files.
    add_executable(kk-hdr-tests tests/hdr_tests.cpp src/hdr_display.cpp)
    target_include_directories(kk-hdr-tests PRIVATE src)
    target_link_libraries(kk-hdr-tests PRIVATE rex::runtime)
    rexglue_apply_target_settings(kk-hdr-tests)
    add_test(NAME hdr-follows-windows COMMAND kk-hdr-tests)
    add_test(NAME telemetry-gzip COMMAND "${Python3_EXECUTABLE}" -I
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/telemetry_gzip_tests.py" "${CMAKE_CURRENT_BINARY_DIR}/telemetry-test")
    set_tests_properties(telemetry-gzip PROPERTIES FIXTURES_REQUIRED telemetry_files)
    add_executable(kk-startup-tests ../native-renderer/tests/startup-recovery-039.cpp)
    add_test(NAME startup-recovery COMMAND kk-startup-tests "${CMAKE_CURRENT_BINARY_DIR}/startup-test-state")
endif()
if(KK_DEV_TOOLS OR NR_BUILD_TESTS)
    enable_testing()
    add_executable(nr_ao_hotspot_tests ../native-renderer/backend/tests/ao_hotspot_tests.cpp)
    target_link_libraries(nr_ao_hotspot_tests PRIVATE nr_draw)
    add_test(NAME render-hotspot-ao COMMAND nr_ao_hotspot_tests)
    add_executable(nr_motion_hotspot_tests ../native-renderer/backend/tests/motion_hotspot_tests.cpp)
    target_link_libraries(nr_motion_hotspot_tests PRIVATE nr_draw)
    add_test(NAME render-hotspot-motion COMMAND nr_motion_hotspot_tests)
    add_executable(nr_capture_hotspot_tests ../native-renderer/backend/tests/capture_hotspot_tests.cpp)
    target_link_libraries(nr_capture_hotspot_tests PRIVATE nr_draw)
    add_test(NAME render-hotspot-capture COMMAND nr_capture_hotspot_tests)
    add_executable(nr_constants_hotspot_tests ../native-renderer/backend/tests/constants_hotspot_tests.cpp)
    target_link_libraries(nr_constants_hotspot_tests PRIVATE nr_draw)
    add_test(NAME render-hotspot-constants COMMAND nr_constants_hotspot_tests)
    add_executable(nr_copy_hotspot_tests ../native-renderer/backend/tests/copy_hotspot_tests.cpp)
    target_link_libraries(nr_copy_hotspot_tests PRIVATE nr_draw)
    add_test(NAME render-hotspot-copy COMMAND nr_copy_hotspot_tests)
    add_executable(nr_vertex_view_tests ../native-renderer/backend/tests/vertex_view_tests.cpp)
    target_link_libraries(nr_vertex_view_tests PRIVATE nr_renderer)
    add_test(NAME render-vertex-view COMMAND nr_vertex_view_tests)
    # Existing CPU regressions also exercise the production thread queue and motion/AO callers.
    foreach(_regression IN ITEMS draw upscale ao_rules)
        if(NOT TARGET nr_${_regression}_tests)
            add_executable(nr_${_regression}_tests ../native-renderer/backend/tests/${_regression}_tests.cpp)
            target_link_libraries(nr_${_regression}_tests PRIVATE nr_draw nr_hooks)
            target_compile_definitions(nr_${_regression}_tests PRIVATE NR_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../native-renderer")
            add_test(NAME render-regression-${_regression} COMMAND nr_${_regression}_tests)
        endif()
    endforeach()
    add_subdirectory(tests/pacing-bench)
endif()

# Button prompt pictures (kk_button_prompts) live beside the program.
# Task 200: the built-in Kong mods and the modding guide ship beside the program (packages copy them too).
add_custom_command(TARGET king_kong POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/mods-builtin $<TARGET_FILE_DIR:king_kong>/mods-builtin
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/../MODDING.md $<TARGET_FILE_DIR:king_kong>/MODDING.md)
# Task 331: the owner's two loading loops (the launcher plays them from launcher/video beside the program).
add_custom_command(TARGET king_kong POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/launcher-video $<TARGET_FILE_DIR:king_kong>/launcher/video)
add_custom_command(TARGET king_kong POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/glyphs $<TARGET_FILE_DIR:king_kong>/glyphs
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/THIRD-PARTY-NOTICES.txt
            $<TARGET_FILE_DIR:king_kong>/THIRD-PARTY-NOTICES.txt)
