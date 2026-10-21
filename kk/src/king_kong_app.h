// king_kong - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>

#include <imgui.h>

#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/xdbf_utils.h>
#include <rex/system/xcontent.h>
#include <rex/system/xmemory.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "art.h"
#include "benchmark.h"
#include "crash_dump.h"
#include "exit_evidence.h"
#include "diagnostics.h"
#include "discord_presence.h"
#include "display_modes.h"
#include "frame_stats.h"
#include "glyphs.h"
#include "guest.h"
#include "handoff.h"
#include "hdr_display.h"
#include "hfr.h"

namespace kk {
void InstallFpeGuard();  // fpe_guard.cpp
}
#include "menu_hook.h"
#include "mouse_look.h"
#include "freecam.h"
#include "compare_capture.h"
#include "cursor_activity.h"
#include "debug_overlay.h"
#include "online.h"
#include "shader_pack.h"
#include "launcher.h"
#include "menu_scene.h"
#include "mods.h"
#include "overlay.h"
#include "ui_paint.h"
#include "platform.h"
#include "scheduled_audio.h"
#include "pause_menu.h"
#include "photo_mode.h"
#include "play_internal.h"
#include "ps2_movies.h"
#include "settings.h"
#include "toast.h"
#include "startup_recovery.h"
#include "telemetry.h"
#include "hooks/kk_native_ui.h"  // the native renderer's launcher prewarm
#include "backend/startup_steps.h"  // task 353

class KingKongApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    kk::startup::MonitorStartup();
    return std::unique_ptr<KingKongApp>(new KingKongApp(ctx, "king_kong",
        PPCImageConfig));
  }

  ~KingKongApp() override { kk::FinishDisplayModePrefetch(); kk::movies::Shutdown(); kk::benchmark::Shutdown(); kk::SetUiPaintRequest({}); kk::startup::CleanExit(); }

 protected:
  bool OnWindowCloseRequested() override {
    // The SDK's accepted window close hard-exits without running our destructor.
    // Task 349c: the window closed (X, Alt+F4) while the menu scene started is not a failed start.
    kk::menu_scene::NoteClosing();
    kk::benchmark::Shutdown();
    kk::play::FlushRendererFiles();
    kk::startup::CleanExit();
    // Beta 7 uploads (Online player count on): at most about 2 s in all, on their own threads.
    kk::telemetry::BeginQuitUploads();
    kk::telemetry::WaitQuitUploads();
    return true;
  }

  void OnShutdown() override {
    kk::benchmark::Shutdown();
    kk::play::FlushRendererFiles();
    kk::startup::CleanExit();
    kk::telemetry::BeginQuitUploads();
    kk::telemetry::WaitQuitUploads();
  }

  // Task 173: a restore from minimised is not a size change, and the SDK's presenter retries a surface it
  // found zero-sized (minimised) only at a size change: reconnect it here, as a resize would.
  void OnWindowRestored() override {
    REXLOG_INFO("KK: window restored");
    kk::menu_scene::SetThrottled(false, "window restored");  // task 330
    RecoverPresentation("window restored");
  }
  void OnWindowMinimized() override {
    REXLOG_INFO("KK: window minimised");
    kk::menu_scene::SetThrottled(true, "window minimised");  // task 330: nothing of the scene is seen
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    // Register the GPU plugin's cvars before the config is read so the
    // launcher can edit and save them, and set the port's own defaults.
    kk::handoff::ArmChild();  // started by a previous run: tell it when this one's first frame is up
    kk::PreloadGpuPlugin();
    kk::ApplyPortDefaults();
    // An AppImage runs from a read-only mount, so keep the game files and the
    // settings beside the .AppImage file instead of beside the program.
    std::filesystem::path base = rex::filesystem::GetExecutableFolder();
    if (const char* appimage = std::getenv("APPIMAGE"); appimage && *appimage) {
      const auto exe_dir = base;
      base = std::filesystem::path(appimage).parent_path();
      if (paths.config_path.empty() || paths.config_path.parent_path() == exe_dir)
        paths.config_path = base / (paths.config_path.empty() ? std::filesystem::path("king_kong.toml")
                                                              : paths.config_path.filename());
      // The runtime's default log folder is beside the program too.
      if (REXCVAR_GET(log_file).empty()) {
        std::error_code ec;
        std::filesystem::create_directories(base / "logs", ec);
        kk::SetCvarDefault("log_file", (base / "logs" / "king_kong.log").string());
      }
    }
    if (paths.game_data_root.empty()) paths.game_data_root = base / "game";
    // The SDK makes the dialogs (OnCreateDialogs: the pause menu and its in-game Settings, which
    // keep this path) in SetupPresentation, before OnFinalizePaths: without this they had an empty
    // path and every in-game change failed to save ("cannot write settings to "), on Windows and
    // Linux. This is the path the SDK and OnFinalizePaths use (SetupEnvironment takes it from here
    // unchanged).
    config_path_ = paths.config_path;
    // Achievement names and icons extracted from the player's default.xex by the builder.
    if (paths.metadata_root.empty()) paths.metadata_root = kk::art::AchievementDir(paths.game_data_root);
    // Before SetupPresentation creates dialogs and captures their user paths.
    if (kk::benchmark::Requested())
      paths.user_data_root = kk::benchmark::Prepare(paths.user_data_root);
  }

  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& input_paths, std::function<void(rex::PathConfig)> resume) override {
    auto defaults = input_paths;
    if (kk::benchmark::Requested() && !kk::benchmark::Active())
      defaults.user_data_root = kk::benchmark::Prepare(input_paths.user_data_root);
    user_data_root_ = defaults.user_data_root;
    config_path_ = defaults.config_path;
    nr::steps::Note("window and graphics device made (presentation set up)");
    kk::SetDebugOverlayConfigPath(defaults.config_path);
    kk::InitDiagnostics();
    {
      const std::filesystem::path log = REXCVAR_GET(log_file);
      kk::InstallCrashDumps(log.has_parent_path() ? log.parent_path()
                                                  : rex::filesystem::GetExecutableFolder() / "logs",
                            user_data_root_);
      // Beta 7 uploads: last session's unsent summary and crash/hang reports, a few seconds from now.
      const std::string cache_root = rex::cvar::GetFlagByName("cache_root");
      kk::telemetry::Configure(user_data_root_,
                               log.has_parent_path() ? log.parent_path()
                                                     : rex::filesystem::GetExecutableFolder() / "logs",
                               !defaults.cache_root.empty() ? defaults.cache_root
                               : cache_root.empty()         ? user_data_root_ / "cache"
                                                            : std::filesystem::path(cache_root));
      kk::telemetry::StartupUploads();
    }
    kk::ApplyRendererChoice();
    const auto recommendation_output = OutputSize();
    kk::InitializeHardwareRecommendation(defaults.config_path, recommendation_output.first, recommendation_output.second);
    kk::MigrateSettings(defaults.config_path);
    kk::MigrateFrameCounter();
    kk::MigrateHdrSettings();
    kk::hdr::Apply("start");  // HDR follows Windows: present_hdr and its levels, before the presenter starts
    kk::ApplyFixedSettings();
    // The launcher's Play menu: the saves live under the user data, a start request may be given
    // (--kk_start), and the game goes back to the launcher by restarting (play_session.cpp).
    kk::play::SetUserDir(defaults.user_data_root);
    if (!kk::benchmark::Active()) kk::play::LoadStartRequestFromSettings();
    kk::play::SetQuitHandler([this] {
      // As closing the window does (the runtime shuts the game down); quitting the app context directly
      // while the game runs hung.
      app_context().CallInUIThreadDeferred([this] {
        kk::startup::CleanExit();
        if (window()) window()->RequestClose();
        else app_context().QuitFromUIThread();
      });
    });
    // Decided once, in OnPreSetup (startup recovery arms only a process that runs the game).
    const bool show = show_launcher_ ? *show_launcher_ : LauncherWillShow(defaults.game_data_root);
    {
      // Task 229: last words for abort() and the like, and logs\session-<role>.txt for an end without any.
      const std::filesystem::path log = REXCVAR_GET(log_file);
      kk::exit_evidence::Install(log.has_parent_path() ? log.parent_path()
                                                       : rex::filesystem::GetExecutableFolder() / "logs",
                                 show ? "launcher" : "game");
    }
    rex::cvar::ResetToDefault("kk_skip_launcher");  // never persist it
    if (!show) {
      std::error_code recommendation_ec;
      if (!std::filesystem::exists(defaults.config_path, recommendation_ec) && !recommendation_ec &&
          !kk::benchmark::Active() && !rex::cvar::GetFlagByName("kk_hardware_tier").empty()) kk::SaveSettings(defaults.config_path);
      // Kong mods: over the player's settings for this run only (never saved), before the Original look
      // (which wins) and before the GPU starts.
      if (!kk::benchmark::Active()) kk::mods::ApplyAtGameStart(defaults.user_data_root);
      kk::NoteGameStarting();  // task 285: a startup hang report if no first frame follows
      kk::compare::ApplyAtGameStart();  // task 354 (--kk_compare_capture only): before the Original look
      kk::EnforceOriginalLook();
      kk::ApplyFullscreenMode(window());
      kk::ApplyRendererChoice();  // the renderer, and with it the default upscaler
      { const auto out = OutputSize(); kk::ApplyRenderPreset(out.first, out.second); }
      kk::ApplyVideoMode();  // before the GPU starts: it sizes its frame from the mode (settings.cpp)
      if (!kk::benchmark::Active()) {
        kk::online::SetPlaying(true);
        kk::telemetry::SetGameRunning();
        kk::online::Start();
        kk::discord::SetPlaying(true);
        kk::discord::Start();
      }
      return defaults;
    }

    kk::LauncherCallbacks cb;
    cb.play = [this, resume, defaults] {
      app_context().CallInUIThreadDeferred([this, resume, defaults] {
        kk::mods::ApplyAtGameStart(defaults.user_data_root);  // Kong mods, for this run only
        kk::compare::ApplyAtGameStart();  // task 354 (--kk_compare_capture only): before the Original look
        kk::EnforceOriginalLook();  // the Original look still wins over a settings mod
        // The Resolution (Display page) before the render quality, which follows it.
        kk::ApplyFullscreenMode(window());
        kk::ApplyRendererChoice();  // the renderer, and with it the default upscaler
        { const auto out = OutputSize(); kk::ApplyRenderPreset(out.first, out.second); }
        kk::ApplyVideoMode();  // before the GPU starts: it sizes its frame from the mode (settings.cpp)
        kk::online::SetPlaying(true);
        kk::telemetry::SetGameRunning();
        kk::discord::SetPlaying(true);
        kk::exit_evidence::SetRole("game");
        // The game starts in this process: startup recovery watches it from here.
        kk::startup::Begin(gpu_plugin_, true);
        kk::NoteGameStarting();  // task 285: a startup hang report if no first frame follows
        resume(defaults);
      });
    };
    cb.restart_and_play = [this] {
      nr::kk::StopLauncherPrewarm();  // its device goes before this process does
      // Task 233: no pipeline compile of this process beside the game's (one driver shader cache).
      kk::play::QuiescePipelines();
      // Task 330: the live menu scene's renderer has written pipelines too: on disk before the game reads them.
      if (kk::menu_scene::Active()) kk::play::FlushRendererFiles();
      kk::menu_scene::SetThrottled(true, "handing over to the game");  // its GPU time to the new process
      // The start request chosen on the Play menu goes with it (kk::play::SetStartRequest).
      const std::wstring start = kk::play::StartRequestArg();
      kk::startup::CleanExit();
      kk::play::RelaunchWith(L"--kk_skip_launcher=true" + (start.empty() ? std::wstring() : L" " + start));
      // The launcher's window stays until the game's has drawn (handoff.h): no desktop between them.
      kk::handoff::QuitWhenChildShown([this] { CloseLauncherProcess(); });
    };
    cb.quit = [this] {
      nr::kk::StopLauncherPrewarm();
      // Task 330: with the live menu scene the window's close ends the process (hard exit): save first.
      if (kk::menu_scene::Active() && !config_path_.empty()) kk::SaveSettings(config_path_);
#if defined(__linux__)
      // The hard exit below skips the launcher's own save a moment after a change.
      if (!config_path_.empty()) kk::SaveSettings(config_path_);
#endif
      CloseLauncherProcess();
    };
    cb.set_fullscreen = [this](bool on) {
      if (window()) window()->SetFullscreen(on);
    };
    cb.benchmark = [this] {
      nr::kk::StopLauncherPrewarm();
      // The launcher's choice of passes (a standard or a quick run) goes to the new process.
      const std::string passes = rex::cvar::GetFlagByName("kk_benchmark_passes");
      // Started from Settings' About tab: the results open there again (benchmark.cpp kk_benchmark_from).
      const bool from_settings = rex::cvar::GetFlagByName("kk_benchmark_from") == "settings";
      rex::cvar::ResetToDefault("kk_benchmark_from");  // never saved
      if (kk::play::RelaunchWith(L"--kk_benchmark=true --kk_benchmark_passes=" +
                                 std::wstring(passes.begin(), passes.end()) +
                                 (from_settings ? L" --kk_benchmark_from=settings" : L"") +
                                 L" --kk_skip_launcher=true")) {
        kk::play::QuiescePipelines();  // task 233, as Play (only when this process is going)
        if (kk::menu_scene::Active()) kk::play::FlushRendererFiles();  // task 330, as Play
        kk::menu_scene::SetThrottled(true, "handing over to the benchmark");
        kk::startup::CleanExit();
        kk::handoff::QuitWhenChildShown([this] { CloseLauncherProcess(); });
      }
    };
    cb.dpi_scale = [this] {
      return window() ? double(window()->GetDpi()) / window()->GetMediumDpi() : 1.0;
    };
    cb.screen_size = [] { return kk::PrimaryScreenSize(); };
    cb.output_size = [this] { return OutputSize(); };
    cb.display_modes = [this](int monitor) { return kk::ListDisplayModes(monitor, window()); };
    cb.desktop_mode = [this](int monitor) { return kk::DesktopDisplayMode(monitor, window()); };
    // Task 330: the game's start menu scene behind the launcher (menu_scene.h): the game starts in this process
    // as Play does, with the launcher staying up; Play then always starts the game as a new process.
    // The runtime's start takes about 1.5 s on the UI thread. Back from the game (a handoff: the game's black
    // cover stays until the launcher's first frame) it happens at once, before that frame; on a start from the
    // desktop just after the launcher's first frames, so its window shows at once (it then pauses briefly).
    cb.start_scene = [this, resume, defaults](bool before_first_frame) {
      auto start = [this, resume, defaults] {
        nr::kk::StopLauncherPrewarm();  // the scene's renderer makes what it draws
        { const auto out = OutputSize(); kk::ApplyRenderPreset(out.first, out.second); }
        kk::ApplyVideoMode();  // before the GPU starts: it sizes its frame from the mode (settings.cpp)
        kk::menu_scene::Begin(defaults.user_data_root);
        resume(defaults);
      };
      if (before_first_frame) start();
      else app_context().CallInUIThreadDeferred(std::move(start));
    };
    // A launcher that starts again (task 330: a shader rebuild while the scene runs, which holds the pack open).
    cb.restart_launcher = [this](const std::wstring& args) {
      nr::kk::StopLauncherPrewarm();
      kk::menu_scene::SetThrottled(true, "handing over to the new launcher");
      kk::play::FlushRendererFiles();
      if (!config_path_.empty()) kk::SaveSettings(config_path_);
      if (kk::play::RelaunchWith(args)) kk::handoff::QuitWhenChildShown([this] { CloseLauncherProcess(); });
    };
    cb.post_to_ui = [this](std::function<void()> f) { app_context().CallInUIThreadDeferred(std::move(f)); };
    cb.recover_presentation = [this] { RecoverPresentation(nullptr); };
    cb.window_state = [this] {
      return window() ? kk::NativeWindowState(window()->GetNativeWindowHandle()) : std::pair<bool, bool>{false, false};
    };
    cb.restore = [this] {
      if (window()) kk::RestoreNativeWindow(window()->GetNativeWindowHandle());
    };
    kk::hdr::SetWindowProvider([this]() -> void* { return window() ? window()->GetNativeWindowHandle() : nullptr; });
    // The Display page's Resolution list, made on a worker while the launcher opens (display_modes.h).
    kk::PrefetchDisplayModes(std::atoi(rex::cvar::GetFlagByName("monitor").c_str()), window());
    kk::ShowLauncher(imgui_drawer(), immediate_drawer(),
                      {defaults.game_data_root, defaults.user_data_root, defaults.config_path},
                      std::move(cb));
    nr::steps::Note("launcher made; its first frame next");
    // The native renderer makes the game's pipelines while the player is in the launcher, when
    // --native_launcher_prewarm is on (the plugin decides; nothing happens otherwise). After the
    // launcher is made: it applies the Original look, which the settings the prewarm reads follow.
    if (!kk::menu_scene::Active()) nr::kk::StartLauncherPrewarm(defaults.cache_root);  // task 330: not beside the scene
    return std::nullopt;
  }

  // The launcher's process ends (Quit, or Play restarting the program for restart-only settings).
  void CloseLauncherProcess() {
    REXLOG_INFO("KK: the launcher closes");
    kk::exit_evidence::NoteCleanExit("launcher closed");
    kk::menu_scene::NoteClosing();
    // Task 330: a launcher with the live menu scene has the game running: closed as the game's window is
    // (the SDK's QuitFromUIThread is for a process without a running game).
    if (kk::menu_scene::Active()) {
      app_context().CallInUIThreadDeferred([this] {
        if (window()) window()->RequestClose();
        else app_context().QuitFromUIThread();
      });
      return;
    }
#if defined(__linux__)
    // On Linux the SDK's normal quit (QuitFromUIThread, then the subsystems' teardown) never returned
    // after the launcher: the process stayed asleep in a futex wait with its relaunched game as a
    // <defunct> child, and Steam saw the game still running. Close the window instead: the SDK's
    // OnClosing flushes the log and hard-exits, as the game's own quit does, and the relaunched
    // process is then reaped by Steam's reaper (or init).
    app_context().CallInUIThreadDeferred([this] {
      if (window()) window()->RequestClose();
      else app_context().QuitFromUIThread();
    });
#else
    app_context().CallInUIThreadDeferred([this] { app_context().QuitFromUIThread(); });
#endif
  }

  void OnConfigureFonts(ImFontAtlas* atlas) override { kk::LoadUiFont(atlas); }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // SetupPresentation precedes OnFinalizePaths in the SDK. Arm before either provider or game GPU setup.
    user_data_root_ = user_data_root();
    {
      // Task 353: crash reports and the start-up step log before the upscaler check, the graphics device, the
      // swapchain and the window (SetupPresentation), not only from OnFinalizePaths on.
      const std::filesystem::path log = REXCVAR_GET(log_file);
      kk::InstallEarlyCrashHandlers(log.has_parent_path() ? log.parent_path()
                                                          : rex::filesystem::GetExecutableFolder() / "logs",
                                    user_data_root_);
      kk::exit_evidence::InstallHandlers();
      nr::steps::Note("settings read; renderer and graphics API next");
    }
    // Task 350a: DirectX 12 on Windows for a settings file from before Beta 14, once: before the renderer reads
    // native_backend, and before startup recovery puts a fallback (Vulkan when DirectX 12 cannot start) over it.
    kk::MigrateGraphicsApi();
    kk::startup::Configure(user_data_root_);
    kk::ApplyVSyncSettings();
    {  // before the runtime reads the shader cache: drop records a finished-early write damaged
      const std::string root = rex::cvar::GetFlagByName("cache_root");
      kk::RepairShaderStorage(root.empty() ? user_data_root_ / "cache" : std::filesystem::path(root));
    }
    // The renderer: kk_renderer (Graphics > Renderer) unless gpu_plugin was given (launcher.h).
    if (!config.graphics) config.gpu_plugin = kk::ApplyRendererChoice();
    if (!config.graphics && config.gpu_plugin.empty()) config.gpu_plugin = "native";
    if (!config.graphics) {
      config.gpu_plugin = kk::startup::ApplyChoice(config.gpu_plugin);
      gpu_plugin_ = config.gpu_plugin;
      show_launcher_ = LauncherWillShow(game_data_root());
      kk::startup::Begin(config.gpu_plugin, !*show_launcher_);
    }
    REXLOG_INFO("KK: renderer {} (kk_renderer {})", config.gpu_plugin, rex::cvar::GetFlagByName("kk_renderer"));
    // Task 335: the launcher's upscaler question (a Direct3D 12 device) before SetupPresentation makes this
    // process's device, swapchain and window; NVIDIA's driver crashed when both were made at once.
    if (!config.graphics && show_launcher_ && *show_launcher_ && config.gpu_plugin == "native")
      kk::AskUpscalersBeforeWindow();
    nr::steps::Note("renderer %s, graphics API %s, %s; the window and graphics device next", config.gpu_plugin.c_str(),
                    rex::cvar::GetFlagByName("native_backend").c_str(),
                    show_launcher_ && *show_launcher_ ? "launcher" : "game");
    // Task 286: always ours. The SDK's SetupPresentation sets its plain SDLAudioSystem before OnPreSetup, so
    // the old "if (!config.audio_factory)" never chose this one (no audio thread scheduling since e360f6f,
    // and none of task 285's slow audio-open lines).
    config.audio_factory = REX_AUDIO_BACKEND(kk::ScheduledAudioSystem);
  }

  void OnPostSetup() override {
    nr::steps::Note("game runtime set up (guest graphics ready)");
    kk::g_guest_base = runtime()->virtual_membase();
    kk::InstallFpeGuard();  // after the runtime's own signal handlers
    kk::TuneProcessScheduling();
    {
      const std::filesystem::path log = REXCVAR_GET(log_file);
      kk::InstallCrashDumps(log.has_parent_path() ? log.parent_path()
                                                  : rex::filesystem::GetExecutableFolder() / "logs", user_data_root_);
    }
    kk::ApplyRuntimeOverrides();
    kk::benchmark::InstallPresentObserver();
    UseOneControllerSlot();
    if (!kk::menu_scene::Active()) kk::mouse_look::Install(window());  // task 330: the launcher keeps the mouse
    // HDR follows Windows for the screen the game's window is on (hdr_display.h).
    kk::hdr::SetWindowProvider([this]() -> void* { return window() ? window()->GetNativeWindowHandle() : nullptr; });
    kk::hdr::Apply("game window");

    ExportAchievementArt();
    // Give the launcher the achievement names (read from the game by the runtime).
    if (!user_data_root_.empty())
      kk::art::WriteAchievementCache(achievements().ListAchievements(),
                                      kk::art::AchievementCachePath(user_data_root_));
    if (!kk::menu_scene::Active()) ScheduleTitleCapture();
    kk::play::SetFrameGrabber([this](int& width, int& height, std::vector<uint8_t>& rgba) {
      rex::ui::RawImage image;
      auto* gfx = runtime() ? runtime()->graphics_system() : nullptr;
      auto* presenter = gfx ? gfx->presenter() : nullptr;  // CaptureGuestOutput: callable from any thread
      if (!presenter || !presenter->CaptureGuestOutput(image) || !image.width || !image.height) return false;
      width = int(image.width), height = int(image.height);
      rgba.resize(size_t(width) * size_t(height) * 4);
      for (int y = 0; y < height; ++y) {
        const uint8_t* src = image.data.data() + size_t(y) * image.stride;
        uint8_t* dst = rgba.data() + size_t(y) * size_t(width) * 4;
        for (int x = 0; x < width; ++x, src += 4, dst += 4) dst[0] = src[0], dst[1] = src[1], dst[2] = src[2], dst[3] = 255;
      }
      return true;
    });
    kk::compare::SetGrabber([this](rex::ui::RawImage& image) {  // task 354 (any thread)
      auto* gfx = runtime() ? runtime()->graphics_system() : nullptr;
      auto* presenter = gfx ? gfx->presenter() : nullptr;
      return presenter && presenter->CaptureGuestOutput(image);
    });
    kk::art::SetFrameCapturer([this](const std::filesystem::path& path) {
      app_context().CallInUIThread([this, path] {
        rex::ui::RawImage image;
        auto* gfx = runtime() ? runtime()->graphics_system() : nullptr;
        auto* presenter = gfx ? gfx->presenter() : nullptr;
        if (presenter && presenter->CaptureGuestOutput(image)) {
          const auto width = image.width, height = image.height;
          if (kk::art::QueueDevCapture(std::move(image), path))
            REXLOG_INFO("KK: captured frame {} ({}x{})", path.filename().string(), width, height);
          else
            REXLOG_ERROR("KK: capture queue full or capture failed: {}", path.string());
        }
      });
    });
    kk::StartButtonPrompts(runtime()->memory());
    kk::hfr::Setup(runtime()->memory());
    if (!kk::menu_scene::Active()) ScheduleWelcomeAchievement();  // in the game's own process

    // Debug aid: set KK_DUMP_IMAGE=<file> to write the decrypted guest image
    // (0x82000000-0x823A0000) for offline analysis.
    const char* dump_path = std::getenv("KK_DUMP_IMAGE");
    if (!dump_path || !*dump_path) return;
    const uint8_t* base = runtime()->virtual_membase();
    std::ofstream out(dump_path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(base + PPCImageConfig.image_base),
              PPCImageConfig.image_size);
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    kk::SetUiPaintRequest([this] {
      app_context().CallInUIThreadDeferred([this] {
        if (window()) window()->RequestPresenterUIPaintFromUIThread();
      });
    });
    SetGuestFrameStats(kk::GetGuestFrameStats);  // F3 overlay "Guest: N FPS"
    // The port's pause menu: the last frame of play behind it (pause_hooks.cpp), under every other dialog.
    kk::play::CreatePauseBackdrop(drawer, immediate_drawer());
    // The port's pause menu (pause_menu.cpp; the launcher's Play makes it too, later calls do nothing).
    kk::CreatePauseMenu(drawer, immediate_drawer(), config_path_, user_data_root_, game_data_root());
    kk::photo::CreatePanel(drawer, immediate_drawer(), user_data_root_);  // photo mode (photo_mode.cpp)
    kk::CreateFpsOverlay(drawer);
    kk::CreateDebugOverlay(drawer, window(), config_path_, user_data_root_);
    kk::compare::Create(drawer, window(), user_data_root_);  // task 354: only with --kk_compare_capture=true
    kk::CreateCursorActivity(drawer, window());
    kk::freecam::Install(drawer, window());
    kk::benchmark::CreateBenchmarkDialog(drawer);
    kk::movies::CreateMovieDialog(drawer, immediate_drawer(), rex::filesystem::GetExecutableFolder() / "glyphs");
  }

  // Xbox 360-style toast with a chime for the game's achievements.
  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog() override {
    auto toast = std::make_unique<kk::AchievementToast>(imgui_drawer(), immediate_drawer(), game_data_root(),
                                                         user_data_root_);
    toast_ = toast.get();
    return toast;
  }

 private:
  // Window::presenter() is protected; a class derived from Window may name it (never made).
  struct WindowPresenter : rex::ui::Window {
    static rex::ui::Presenter* Of(const rex::ui::Window& w) { return (w.*&WindowPresenter::presenter)(); }
  };
  // Reconnects the presenter to the window's surface (what a resize does), when the window can be drawn
  // (not minimised or hidden, some pixels). UI thread. `why`: logged (null: the launcher's heartbeat, which
  // counts its own).
  void RecoverPresentation(const char* why) {
#if defined(KK_DEV_TOOLS)
    static const bool off = std::getenv("KK_DEV_NO_HEARTBEAT") != nullptr;  // the behaviour before task 173
    if (off) return;
#endif
    rex::ui::Window* w = window();
    rex::ui::Presenter* presenter = w ? WindowPresenter::Of(*w) : nullptr;
    if (!presenter) return;
    // SDL_GetWindows here sees none of the SDK's windows (another SDL): ask the platform instead.
    const bool drawable = kk::NativeWindowDrawable(w->GetNativeWindowHandle());
    static int asked = 0;
    if (why || ++asked <= 3 || asked % 30 == 0)
      REXLOG_INFO("KK: presenter reconnect ({}): {}", why ? why : "launcher heartbeat",
                  drawable ? "reconnecting" : "window minimised or hidden, later");
    if (!drawable) return;
    presenter->OnSurfaceResizeFromUIThread();
    w->RequestPaint();
  }

  // Size the game is shown at: in fullscreen the Resolution picked on the Display page, or the
  // monitor's; else the window.
  std::pair<int, int> OutputSize() const {
    if (REXCVAR_QUERY(bool, fullscreen)) {
      if (const auto mode = kk::FullscreenModeSize(); mode && mode->first > 0 && mode->second > 0) return *mode;
      if (!kk::ParseDisplayMode(REXCVAR_GET(kk_fullscreen_mode))) {
        const int monitor = std::atoi(rex::cvar::GetFlagByName("monitor").c_str());
        const kk::DisplayMode desktop = kk::DesktopDisplayMode(monitor, window());
        if (desktop.width > 0 && desktop.height > 0) return {desktop.width, desktop.height};
      }
      return kk::PrimaryScreenSize();
    }
    if (!window()) {
      return kk::PrimaryScreenSize();
    }
    return {int(window()->GetActualPhysicalWidth()), int(window()->GetActualPhysicalHeight())};
  }

  // The game plays as whichever player pressed Start at the title screen, but
  // only player 1 has a signed-in profile, so from player 2-4 it refuses to
  // save ("sign in to save"). By default each controller gets its own player
  // in connection order, and Steam Input, DS4Windows or a handheld's built-in
  // pad often add a second device ahead of the one in your hands. The game is
  // single player, so every controller (and keyboard & mouse) is player 1.
  void UseOneControllerSlot() {
    auto* input = dynamic_cast<rex::input::InputSystem*>(runtime()->input_system());
    if (!input) return;
    input->SetDeviceAssignment(std::make_unique<rex::input::SharedAssignment>());
    REXLOG_INFO("KK: all controllers are player 1");
  }

  // The launcher shows the achievements' names and pictures from
  // game/achievements. On the first run after installing, write them out of
  // the game's own executable (its XDBF resource).
  void ExportAchievementArt() {
    const auto dir = kk::art::AchievementDir(game_data_root());
    std::error_code ec;
    if (std::filesystem::exists(dir / "achievements.toml", ec)) return;
    auto* ks = runtime() ? runtime()->kernel_state() : nullptr;
    if (!ks) return;
    const auto db = ks->title_xdbf();
    if (!db.is_valid()) return;
    std::filesystem::create_directories(dir / "icons", ec);
    const auto lang = db.GetExistingLanguage(rex::system::XLanguage::kEnglish);
    auto quote = [](const std::string& s) {
      std::string out = "\"";
      for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
      }
      return out + "\"";
    };
    std::ofstream toml(dir / "achievements.toml", std::ios::binary);
    for (const auto& a : db.GetAchievements()) {
      const uint32_t image = a.image_id;
      toml << "[[achievements]]\n"
           << "id = " << uint32_t(a.id) << "\n"
           << "label = " << quote(db.GetStringTableEntry(lang, a.label_id)) << "\n"
           << "description = " << quote(db.GetStringTableEntry(lang, a.description_id)) << "\n"
           << "unachieved_description = " << quote(db.GetStringTableEntry(lang, a.unachieved_id)) << "\n"
           << "gamerscore = " << uint32_t(a.gamerscore) << "\n"
           << "image_id = " << image << "\n"
           << "icon_path = \"icons/" << image << ".png\"\n\n";
      const auto block = db.GetEntry(rex::system::util::XdbfSection::kImage, image);
      if (block.buffer && block.size) {
        std::ofstream png(dir / "icons" / (std::to_string(image) + ".png"), std::ios::binary);
        png.write(reinterpret_cast<const char*>(block.buffer), std::streamsize(block.size));
      }
    }
    REXLOG_INFO("KK: wrote achievement list and icons to {}", dir.string());
  }

  // On first play, keep a frame of the game's menu (moon over Skull Island) as
  // launcher art. The intro videos run for over a minute and can be skipped, so
  // wait for the save menu to open, then let its backdrop fade in.
  void ScheduleTitleCapture() {
    if (user_data_root_.empty()) return;
    const auto path = kk::art::TitleCapturePath(user_data_root_);
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) return;
    kk::OnSaveMenuShown([this, path] {
      kk::RunAfterDelay(3.0, [this, path] {
        app_context().CallInUIThread([this, path] {
          rex::ui::RawImage image;
          auto* gfx = runtime() ? runtime()->graphics_system() : nullptr;
          auto* presenter = gfx ? gfx->presenter() : nullptr;
          if (presenter && presenter->CaptureGuestOutput(image) && kk::art::SaveTitleCapture(image, path))
            REXLOG_INFO("KK: saved launcher art {}x{}", image.width, image.height);
        });
      });
    });
  }

  // The port's own "Welcome" achievement: unlocks a few seconds into the first
  // play so players learn the game has achievements.
  void ScheduleWelcomeAchievement() {
    const auto& welcome = kk::PortAchievements().front();
    if (user_data_root_.empty() || kk::IsPortAchievementUnlocked(user_data_root_, welcome.id)) return;
    kk::RunAfterFirstFrame(6.0, [this, &welcome] {
      if (kk::UnlockPortAchievement(user_data_root_, welcome.id) && toast_) toast_->Show(welcome.title, 0, 0);
    });
  }

  std::filesystem::path user_data_root_, config_path_;
  std::string gpu_plugin_;
  std::optional<bool> show_launcher_;

  // Whether this start shows the launcher before the game. --kk_launcher_open (back from the game,
  // launcher.cpp): the launcher shows even when it is switched off.
  static bool LauncherWillShow(const std::filesystem::path& game_data_root) {
    const bool skip_once = REXCVAR_GET(kk_skip_launcher);
    return kk::startup::NeedsNotice() || !kk::GameFilesPresent(game_data_root) ||
           (!kk::benchmark::Requested() && !kk::benchmark::Active() && (kk::IsShiftHeld() ||
           (REXCVAR_GET(kk_launcher) && !skip_once) ||
           (!skip_once && !rex::cvar::GetFlagByName("kk_launcher_open").empty())));
  }
  kk::AchievementToast* toast_ = nullptr;  // owned by ReXApp
};
