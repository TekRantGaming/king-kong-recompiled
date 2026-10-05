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
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "art.h"
#include "frame_stats.h"
#include "menu_hook.h"
#include "launcher.h"
#include "overlay.h"
#include "platform.h"
#include "settings.h"
#include "toast.h"

class KingKongApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<KingKongApp>(new KingKongApp(ctx, "king_kong",
        PPCImageConfig));
  }

 protected:
  void OnConfigurePaths(rex::PathConfig& paths) override {
    // Register the GPU plugin's cvars before the config is read so the
    // launcher can edit and save them, and set the port's own defaults.
    kk::PreloadGpuPlugin();
    kk::ApplyPortDefaults();
    if (paths.game_data_root.empty())
      paths.game_data_root = rex::filesystem::GetExecutableFolder() / "game";
    // Achievement names and icons extracted from the player's default.xex by the builder.
    if (paths.metadata_root.empty()) paths.metadata_root = kk::art::AchievementDir(paths.game_data_root);
  }

  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume) override {
    user_data_root_ = defaults.user_data_root;
    const bool skip_once = REXCVAR_GET(kk_skip_launcher);
    rex::cvar::ResetToDefault("kk_skip_launcher");  // never persist it
    const bool files_ok = kk::GameFilesPresent(defaults.game_data_root);
    const bool show = !files_ok || kk::IsShiftHeld() || (REXCVAR_GET(kk_launcher) && !skip_once);
    if (!show) {
      kk::ApplyRenderPreset(OutputSize().second);
      return defaults;
    }

    kk::LauncherCallbacks cb;
    cb.play = [this, resume, defaults] {
      app_context().CallInUIThreadDeferred([this, resume, defaults] {
        kk::ApplyRenderPreset(OutputSize().second);
        resume(defaults);
      });
    };
    cb.restart_and_play = [this] {
      kk::RelaunchSelf(L"--kk_skip_launcher=true");
      app_context().CallInUIThreadDeferred([this] { app_context().QuitFromUIThread(); });
    };
    cb.quit = [this] {
      app_context().CallInUIThreadDeferred([this] { app_context().QuitFromUIThread(); });
    };
    cb.set_fullscreen = [this](bool on) {
      if (window()) window()->SetFullscreen(on);
    };
    cb.dpi_scale = [this] {
      return window() ? double(window()->GetDpi()) / window()->GetMediumDpi() : 1.0;
    };
    cb.screen_size = [] { return kk::PrimaryScreenSize(); };
    cb.output_size = [this] { return OutputSize(); };
    kk::ShowLauncher(imgui_drawer(), immediate_drawer(),
                      {defaults.game_data_root, defaults.user_data_root, defaults.config_path},
                      std::move(cb));
    return std::nullopt;
  }

  void OnConfigureFonts(ImFontAtlas* atlas) override { kk::LoadUiFont(atlas); }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (!config.graphics && config.gpu_plugin.empty()) config.gpu_plugin = "xenos";
    if (!config.audio_factory)
      config.audio_factory = REX_AUDIO_BACKEND(rex::audio::sdl::SDLAudioSystem);
  }

  void OnPostSetup() override {
    kk::ApplyRuntimeOverrides();

    // Give the launcher the achievement names (read from the game by the runtime).
    if (!user_data_root_.empty())
      kk::art::WriteAchievementCache(achievements().ListAchievements(),
                                      kk::art::AchievementCachePath(user_data_root_));
    ScheduleTitleCapture();
    ScheduleWelcomeAchievement();

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
    SetGuestFrameStats(kk::GetGuestFrameStats);  // F3 overlay "Guest: N FPS"
    kk::CreateFpsOverlay(drawer);
  }

  // Xbox 360-style toast with a chime for the game's achievements.
  std::unique_ptr<rex::ui::AchievementNotificationDialog> CreateAchievementNotificationDialog() override {
    auto toast = std::make_unique<kk::AchievementToast>(imgui_drawer(), immediate_drawer(), game_data_root(),
                                                         user_data_root_);
    toast_ = toast.get();
    return toast;
  }

 private:
  // Size the game is shown at: the monitor in fullscreen, else the window.
  std::pair<int, int> OutputSize() const {
    if (REXCVAR_QUERY(bool, fullscreen) || !window()) return kk::PrimaryScreenSize();
    return {int(window()->GetActualPhysicalWidth()), int(window()->GetActualPhysicalHeight())};
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

  std::filesystem::path user_data_root_;
  kk::AchievementToast* toast_ = nullptr;  // owned by ReXApp
};
