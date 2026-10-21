// Console checks of launcher logic that needs no game, window or device: the shader job's single
// progress bar (shader_progress.h), and the audio and subtitle options written to every save
// (profile_save_file.h), on made-up saves in a temporary folder.
#include "profile_save_file.h"
#include "shader_progress.h"
#include "shader_prepared.h"
#include "settings_layout.h"
#include "launcher_policy.h"
#include "graphics_presets.h"
#include "cursor_activity.h"
#include "slider_input.h"
#include "extras.h"
#include "dev_launcher_chain.h"
#include "key_glyphs.h"
#include "dev_movie_input.h"
#include "video_modes.h"
#include "cheats.h"
#include "startup_recovery_state.h"
#include "amd_shader_cache.h"
#include "setup_again.h"

#include <bit>
#include <cstdio>
#if defined(_WIN32)
#include <share.h>
#endif
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace {
int checks = 0;
void Check(bool condition, const char* message) {
  ++checks;
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}
bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

void ExtrasChecks() {
  using namespace kk::play;
  ExtraProgress fresh;
  Check(kExtras.size() == 15, "the retail Extras page has fifteen entries");
  // Independently transcribed from sub_824CBFE8, loc_824CCD08's id switch.
  // Modes return from that switch; their fields are read by sub_825EE130.
  constexpr ExtraInfo original[] = {
      {19, ExtraType::kMovie, 0x820AE520, -1, -1},
      {18, ExtraType::kLevel, 7, -1, -1},
      {12, ExtraType::kLevel, 54, -1, 5},
      {1, ExtraType::kMode, 0, -1, 15, 0, 0, 0x4DA4},
      {15, ExtraType::kLevel, 55, -1, 1000},
      {9, ExtraType::kLevel, 57, -1, 50},
      {5, ExtraType::kLevel, 50, -1, 75},
      {8, ExtraType::kMode, 0, -1, 100, 0, 0, 0x4DA0},
      {6, ExtraType::kLevel, 51, 20000, -1},
      {7, ExtraType::kLevel, 56, 50000, -1},
      {0, ExtraType::kMovie, 0x820AE4F0, 75000, -1},
      {10, ExtraType::kLevel, 52, 100000, -1},
      {11, ExtraType::kLevel, 53, 150000, -1},
      {3, ExtraType::kMovie, 0x820AE508, -1, 2000},
      {2, ExtraType::kLevel, 2003, 250000, 100, 1, 1},
  };
  constexpr uint32_t original_selection[] = {0, 2, 3, 0, 5, 6, 7, 0, 9, 10, 0, 12, 13, 0, 15};
  for (size_t i = 0; i < kExtras.size(); ++i) {
    const auto& want = original[i];
    const auto* got = FindExtra(want.id);
    Check(kExtras[i].id == want.id && got, "original Extras order and id lookup");
    Check(got->type == want.type && got->level == want.level,
          "every original id selects its exact level or movie address");
    Check(got->start_point == want.start_point && got->session_flag == want.session_flag &&
              got->mode_offset == want.mode_offset,
          "every original id preserves its start point and session or mode flags");
    Check(got->score == want.score && got->progress == want.progress,
          "every original id retains its own unlock requirements");
    Check(ExtraSelection(want.id) == original_selection[i],
          "only level bonuses write their original one-based selection to G+0x5680");
  }
  Check(!ExtraSelection(-1) && !ExtraSelection(4) && !ExtraSelection(13) && !ExtraSelection(20),
        "invalid bonuses do not write the selection field");
  Check(!FindExtra(-1) && !FindExtra(4) && !FindExtra(13) && !FindExtra(20), "unused and invalid bonus ids are not launchable");
  std::map<int, bool> ids;
  for (const auto& e : kExtras) {
    Check(ids.emplace(e.id, true).second, "each retail bonus appears once");
    Check(ExtraUnlocked(e, fresh) == (e.id == 19 || e.id == 18), "new profile availability");
    // Owner 18 Oct: every bonus is unlocked for every player, with no save at all (task 204).
    Check(BonusUnlocked(e), "every bonus is unlocked without a save");
    ExtraProgress p;
    p.museum = true;
    Check(ExtraUnlocked(e, p), "the original museum cheat unlocks each bonus");
    p = {};
    if (e.progress > 100) {
      p.percent = 100; p.score = 1000000;
      Check(!ExtraUnlocked(e, p), "progress and score cannot unlock an online-code bonus");
      p.online_codes = 1u << e.id;
      Check(ExtraUnlocked(e, p), "the matching saved online-code flag unlocks its bonus");
      p.online_codes = 1u << (e.id + 1);
      Check(!ExtraUnlocked(e, p), "an unrelated code flag does not unlock the bonus");
    } else {
      p.percent = std::max(0, e.progress); p.score = std::max(0, e.score);
      Check(ExtraUnlocked(e, p) == (float(p.percent) * 0.01f >= float(e.progress) / 100.0f),
            "unlock comparison matches the game's single-precision progress calculation");
      if (e.progress > 0) { --p.percent; Check(!ExtraUnlocked(e, p), "progress below threshold remains locked"); p.percent = 100; }
      if (e.score > 0) { --p.score; Check(!ExtraUnlocked(e, p), "score below threshold remains locked"); }
    }
  }
  Check(kAllBonusesUnlocked, "all bonus content is unlocked by default");
  for (const auto& c : kk::kCheats)
    Check(std::string(c.id) != "bonus" && std::string(c.id) != "chapters",
          "All bonus content and All chapters are not offered as cheats (both always on, never cheats)");
  const auto* ending = FindExtra(2);
  Check(ending && ending->level == 2003 && ending->score == 250000 && ending->progress == 100,
        "alternate ending uses its original level, score and completion requirements");
}

void GraphicsPresetChecks() {
  const char* expected[] = {"low", "medium", "high", "high", "low"};
  const char* distances[] = {"near", "medium", "far", "far", "near"};  // Ultra: Far since Beta 8 (Full is F8 only)
  for (int i = 0; i < kk::kGraphicsPresetCount; ++i) {
    const auto& p = kk::kGraphicsPresets[i];
    Check(std::string_view(p.gi) == expected[i], "preset GI mapping");
    Check(std::string_view(p.gi_distance) == distances[i], "preset GI distance mapping");
    std::map<std::string, std::string> settings;
    auto get = [&](const char* name) { return settings[name]; };
    kk::ApplyGraphicsPresetSettings(p, [&](const char* name, const char* value) { settings[name] = value; });
    Check(kk::MatchesGraphicsPresetSettings(p, get), "applied preset matches");
    settings["native_gi"] = "off";
    Check(!kk::MatchesGraphicsPresetSettings(p, get), "different GI makes the preset Custom");
    Check(kk::MatchesGraphicsPresetSettings(p, get, false), "old preset is recognised without GI");
    settings["anisotropic_override"] = "-1";
    Check(!kk::MatchesGraphicsPresetSettings(p, get, false), "other custom settings do not match");
  }
  std::map<std::string, std::string> settings;
  auto get = [&](const char* name) { return settings[name]; };
  auto set = [&](const char* name, const char* value) { settings[name] = value; };
  auto match = [&] { return kk::FindGraphicsPreset(get, [](const auto&) { return true; }); };
  kk::ApplyGraphicsPresetSettings(kk::kGraphicsPresets[2], set);
  Check(settings["anisotropic_override"] == "5", "High uses 16x");
  Check(match() == 2, "High selection survives identical Ultra values");
  kk::ApplyGraphicsPresetSettings(kk::kGraphicsPresets[3], set);
  Check(settings["anisotropic_override"] == "5", "Ultra uses 16x");
  Check(match() == 3, "Ultra selection is kept");
  settings.erase("kk_graphics_preset");
  Check(match() == 3, "unmarked old Ultra is still recognised");
  settings["kk_graphics_preset"] = "High";
  settings["anisotropic_override"] = "4";
  Check(match() == -1 && settings["anisotropic_override"] == "4", "existing 8x choice remains Custom and is not overwritten");
}

// The ONE definition of "the shaders are prepared" (shader_prepared.h; owner, 18 Oct): exactly one preparation
// per version, which setup, Home, What's new and Play all read the same way.
void ShaderPreparedChecks() {
  using namespace kk;
  const std::string v = "2.0.0-beta.9";
  // The marker round trip.
  Check(FormatPreparedMarker(v, kShaderPackAll) == "2.0.0-beta.9;spirv,dxil", "marker names both packs");
  Check(FormatPreparedMarker(v, kShaderPackSpirv) == "2.0.0-beta.9;spirv", "marker for Vulkan only (Linux)");
  Check(FormatPreparedMarker(v, kShaderPackDxil) == "2.0.0-beta.9;dxil", "marker for DirectX 12 only");
  for (int packs = 1; packs <= kShaderPackAll; ++packs) {
    const PreparedMarker m = ParsePreparedMarker(FormatPreparedMarker(v, packs));
    Check(m.version == v && m.packs == packs, "marker parses back to its version and packs");
  }
  Check(ParsePreparedMarker("2.0.0-beta.9;dxil,spirv").packs == kShaderPackAll, "pack order does not matter");
  Check(ParsePreparedMarker("2.0.0-beta.9;spirv,bogus").packs == kShaderPackSpirv, "unknown pack names are ignored");

  // The definition.
  const std::string both = FormatPreparedMarker(v, kShaderPackAll);
  Check(ShadersPrepared(both, v, kShaderPackAll), "this version, both packs there: prepared");
  Check(!ShadersPrepared(both, v, kShaderPackSpirv), "a pack the preparation made is missing: not prepared");
  Check(!ShadersPrepared(both, v, kShaderPackDxil), "the Vulkan pack missing: not prepared");
  Check(!ShadersPrepared(both, v, 0), "no packs: not prepared");
  Check(!ShadersPrepared(both, "2.0.0-beta.10", kShaderPackAll), "another version (an update): not prepared");
  Check(!ShadersPrepared("", v, kShaderPackAll), "never prepared (a fresh install): not prepared");
  // Beta 8 / Beta 9 test build: only the version, written even for a cancelled job. Not trusted.
  Check(!ShadersPrepared(v, v, kShaderPackAll), "the old marker without packs is not trusted");
  Check(!ShadersPrepared(v + ";", v, kShaderPackAll), "a marker naming no pack is not prepared");
  Check(ShadersPrepared(FormatPreparedMarker(v, kShaderPackSpirv), v, kShaderPackSpirv),
        "Linux: the Vulkan pack alone is the whole preparation");
  Check(ShadersPrepared(FormatPreparedMarker(v, kShaderPackSpirv), v, kShaderPackAll),
        "an extra pack in the cache does not matter");

  // Which job counts as the preparation.
  ShaderJobEnd e;
  e.finished = true;
  e.packs_needed = kShaderPackAll;
  e.packs_ready = kShaderPackAll;
  Check(JobIsThePreparation(e), "finished with both packs: the preparation");
  e.cancelled = true;
  Check(!JobIsThePreparation(e), "a cancelled job (launcher closed during setup) is not the preparation");
  e.cancelled = false;
  e.packs_ready = kShaderPackSpirv;
  Check(!JobIsThePreparation(e), "a job that left a pack unbuilt is not the preparation");
  e.packs_ready = kShaderPackAll;
  e.finished = false;
  Check(!JobIsThePreparation(e), "a job still running is not the preparation yet");
  e.finished = true;
  e.packs_needed = e.packs_ready = 0;
  Check(!JobIsThePreparation(e), "a job that covered no pack is not the preparation");
  e.packs_needed = e.packs_ready = kShaderPackSpirv;
  Check(JobIsThePreparation(e), "Linux: the Vulkan pack is all the job needs");
  // What the job records is exactly what the definition then accepts.
  e.packs_needed = e.packs_ready = kShaderPackAll;
  Check(ShadersPrepared(FormatPreparedMarker(v, e.packs_needed), v, e.packs_ready),
        "the finished setup's marker reads as prepared on Home and at Play");

  // The launcher opening: exactly once per version.
  Check(JobAtLauncherStart(false, false) == ShaderJobAtStart::kPrepare, "first start: prepare");
  Check(JobAtLauncherStart(false, true) == ShaderJobAtStart::kNone, "prepared: never again at a launcher start");
  Check(JobAtLauncherStart(true, false) == ShaderJobAtStart::kRebuild, "after an update: rebuilt in full once");
  // The update's rebuild records the new version: the next start does nothing.
  Check(JobAtLauncherStart(false, ShadersPrepared(FormatPreparedMarker(v, kShaderPackAll), v, kShaderPackAll)) ==
            ShaderJobAtStart::kNone,
        "after the update's preparation, the next start prepares nothing");

  // Play.
  Check(AtPlay(true, false, false) == ShaderPlayStep::kWaitForJob, "a job running finishes first");
  Check(AtPlay(true, true, false) == ShaderPlayStep::kWaitForJob, "a Rebuild running finishes first");
  Check(AtPlay(false, true, false) == ShaderPlayStep::kPlay, "prepared (a later launcher start): straight to the game");
  Check(AtPlay(false, true, true) == ShaderPlayStep::kPlay, "prepared at setup in this launcher: straight to the game");
  Check(AtPlay(false, false, false) == ShaderPlayStep::kPrepareFirst, "never prepared (setup closed half way): once");
  Check(AtPlay(false, false, true) == ShaderPlayStep::kPlay, "a job that failed here is not repeated at Play");

  // Home's status reads the same definition, not the launcher's own job.
  Check(HomeStatus(false, false, true, false, false) == ShaderHomeStatus::kReady,
        "a launcher start with prepared shaders and no job shows Ready (the Beta 9 bug showed Not prepared)");
  Check(HomeStatus(false, false, true, true, false) == ShaderHomeStatus::kReady, "right after the setup's job: Ready");
  Check(HomeStatus(true, true, true, false, false) == ShaderHomeStatus::kPreparing, "a Rebuild shows its progress");
  Check(HomeStatus(true, false, false, false, false) == ShaderHomeStatus::kChecking, "a job only checking");
  Check(HomeStatus(false, false, false, true, false) == ShaderHomeStatus::kFailed, "a job that failed: see the log");
  Check(HomeStatus(false, false, false, true, true) == ShaderHomeStatus::kNotPrepared, "a cancelled job: not prepared");
  Check(HomeStatus(false, false, false, false, false) == ShaderHomeStatus::kNotPrepared, "nothing yet: not prepared");
  Check(HomeStatus(false, false, true, true, true) == ShaderHomeStatus::kReady,
        "a cancelled Rebuild leaves the earlier preparation Ready");
}

void ShaderProgressChecks() {
  using kk::ShaderJobFraction;
  using kk::ShaderJobState;
  ShaderJobState s;
  Check(ShaderJobFraction(s) == 0.0f, "no steps yet: 0");
  // Two APIs: four steps, shaders 45% each and pipelines 5% each.
  s.steps = 4;
  s.step = 1;
  s.phase = 1;  // reading the database
  Check(ShaderJobFraction(s) == 0.0f, "step 1 reading: 0");
  s.phase = 2;
  s.total = 1000;
  s.done = 500;
  Check(Near(ShaderJobFraction(s), 0.45f * 0.5f * kk::kShaderCompileShare), "step 1 half compiled");
  s.phase = 3;
  Check(Near(ShaderJobFraction(s), 0.45f * kk::kShaderCompileShare), "step 1 writing");
  s.phase = 4;
  Check(Near(ShaderJobFraction(s), 0.45f), "step 1 finished");
  s.step = 2;
  s.step_kind = 1;
  s.done = s.total = 0;
  s.pass = 0;
  Check(Near(ShaderJobFraction(s), 0.45f), "step 2 starts where step 1 ended");
  s.total = 100;
  s.done = 50;
  Check(Near(ShaderJobFraction(s), 0.45f + 0.05f * 0.25f), "step 2 first pass half");
  s.pass = 1;
  s.done = 100;
  Check(Near(ShaderJobFraction(s), 0.50f), "step 2 both passes done");
  s.step = 3;
  s.step_kind = 0;
  s.phase = 0;
  s.done = s.total = 0;
  Check(Near(ShaderJobFraction(s), 0.50f), "step 3 starts at half");
  s.step = 4;
  s.step_kind = 1;
  s.pass = 1;
  s.total = s.done = 10;
  Check(Near(ShaderJobFraction(s), 1.0f), "the last step done: 100%");
  // One API (the other failed to start on this PC): two steps over the whole bar.
  ShaderJobState one;
  one.steps = 2;
  one.step = 2;
  one.step_kind = 1;
  Check(Near(ShaderJobFraction(one), 0.45f / 0.5f), "one API: its shaders are 90%");
  // Out-of-range reports stay on the bar.
  ShaderJobState odd;
  odd.steps = 2;
  odd.step = 7;
  odd.step_kind = 1;
  odd.pass = 5;
  odd.done = 50;
  odd.total = 10;
  Check(ShaderJobFraction(odd) <= 1.0f && ShaderJobFraction(odd) >= 0.0f, "out-of-range counts are clamped");

  // A whole job, sampled as the launcher polls it: the bar never goes back and ends at 100%.
  kk::ShaderProgressBar bar;
  float last = 0;
  bool monotonic = true;
  auto poll = [&](const ShaderJobState& st) {
    const float f = bar.Update(st);
    monotonic = monotonic && f >= last;
    last = f;
  };
  for (int api = 0; api < 2; ++api) {
    ShaderJobState st;
    st.steps = 4;
    st.step = api * 2 + 1;
    for (int phase = 0; phase <= 1; ++phase) { st.phase = phase; poll(st); }
    st.phase = 2;
    st.total = 17790;
    for (uint32_t d = 0; d <= st.total; d += 997) { st.done = d; poll(st); }
    st.phase = 3; poll(st);
    st.phase = 4; poll(st);
    st.step = api * 2 + 2;
    st.step_kind = 1;
    st.phase = 4;
    for (int pass = 0; pass < 2; ++pass) {
      st.pass = pass;
      st.done = st.total = 0;
      poll(st);
      // The pass queues more pipelines as it goes: the count goes back, the bar must not.
      st.total = 3000;
      st.done = 2000;
      poll(st);
      st.total = 3700;
      st.done = 2100;
      poll(st);
      st.done = 3700;
      poll(st);
    }
  }
  Check(monotonic, "the bar never goes back over a whole job");
  Check(Near(last, 1.0f), "a whole job ends at 100%");
  bar.Reset();
  Check(bar.Shown() == 0.0f, "a new job starts at 0");

  // Task 144: ONE bar, no steps shown. A step with nothing to do (its API's shaders already current) takes
  // no room, so the bar does not jump by its share; the drawn bar eases and never goes back.
  kk::ShaderProgressBar only_pipelines;
  ShaderJobState c;
  c.steps = 4;
  c.step = 1;
  c.working = false;  // Vulkan's shaders: only checked
  c.phase = 4;
  only_pipelines.Update(c);
  c.step = 2;
  c.step_kind = 1;
  c.working = true;  // Vulkan's pipelines: prepared
  c.total = 100;
  c.done = 0;
  Check(only_pipelines.Update(c) < 0.01f, "a skipped step does not move the bar");
  c.done = 100;
  c.pass = 1;
  const float after_vulkan = only_pipelines.Update(c);
  Check(after_vulkan > 0.04f && after_vulkan < 0.2f, "the prepared step fills its own share of what is left");
  Check(only_pipelines.skipped() == 1u, "step 1 is known as skipped");
  // Smoothness: the drawn bar follows the job's value over time, never past it, never back.
  kk::ShaderProgressBar eased;
  ShaderJobState e;
  e.steps = 2;
  e.step = 1;
  e.phase = 2;
  e.total = 100;
  e.done = 100;
  const float target = eased.Update(e);
  float drawn = eased.Drawn(0.0), previous = drawn;
  bool smooth = true;
  for (int frame = 1; frame <= 120; ++frame) {
    drawn = eased.Drawn(frame / 60.0);
    smooth = smooth && drawn >= previous && drawn <= target && drawn - previous < 0.2f;
    previous = drawn;
  }
  Check(smooth, "the drawn bar moves in small steps, never back, never past the job");
  Check(target - drawn < 0.01f, "the drawn bar reaches the job's value within two seconds");
  Check(eased.Drawn(2.0) == drawn, "drawing twice at the same time changes nothing");
}

void SettingsLayoutChecks() {
  namespace L = kk::settings_layout;
  // The owner's order (16 Oct), page by page: each label must come after the one before.
  auto in_order = [](L::Page page, std::initializer_list<const char*> labels) {
    int last = -1;
    for (const char* label : labels) {
      const int at = L::IndexOf(page, label);
      if (at < 0 || at <= last) return false;
      last = at;
    }
    return true;
  };
  Check(std::string(L::kPageNames[0]) == "Display" && std::string(L::kPageNames[1]) == "Graphics" &&
            std::string(L::kPageNames[2]) == "Audio" && std::string(L::kPageNames[3]) == "Controls" &&
            std::string(L::kPageNames[4]) == "Gameplay" && std::string(L::kPageNames[5]) == "Cheats" &&
            std::string(L::kPageNames[6]) == "Advanced" && L::kPageCount == 7,
        "categories: Display, Graphics, Audio, Controls, Gameplay, Cheats, Advanced (owner, 16 Oct evening)");
  Check(in_order(L::kDisplay, {"Window mode", "Monitor", "Resolution", "Refresh rate", "VSync", "Frame rate", "HDR"}),
        "Display: window mode, monitor, resolution, refresh rate, VSync, frame-rate cap, HDR");
  Check(in_order(L::kGraphics, {"Preset", "Graphics API", "Upscaler", "Upscaler mode", "Anti-aliasing",
                                "Ambient occlusion", "Global illumination", "Screen blur", "Texture pack"}),
        "Graphics: preset, API, upscaler and quality, anti-aliasing, effects, advanced last");
  Check(L::IndexOf(L::kGraphics, "Look") < L::IndexOf(L::kGraphics, "Graphics API"), "the look is with the preset");
  Check(std::string(L::RowsOf(L::kGraphics).begin()[L::IndexOf(L::kGraphics, "Rebuild shaders")].section) == "Advanced",
        "Rebuild shaders is in Graphics' advanced section");
  // Task 209: the texture pack row is a choice of the installed packs (one on at a time); downloading, importing
  // and removing moved to the Workshop, which the next row opens.
  Check(in_order(L::kGraphics, {"Texture pack", "Texture packs in the Workshop", "Texture pack folder", "Effect resolution"}) &&
            std::string(L::RowsOf(L::kGraphics).begin()[L::IndexOf(L::kGraphics, "Texture packs in the Workshop")].section) ==
                "Texture pack" &&
            L::IndexOf(L::kGraphics, "Import ZIP") < 0 && L::IndexOf(L::kGraphics, "Download HD texture pack") < 0,
        "Graphics: the texture pack's own section before Advanced (job J2), its row opening the Workshop (task 209)");
  Check(in_order(L::kAdvanced, {"PS2 cutscenes", "Download PS2 cutscenes", "Remove PS2 cutscenes", "PS2 movie folder"}),
        "Advanced: the PS2 cutscenes switch, Download and Remove (job J2)");
  // Owner, 18 Oct: brightness and gamma on Display (Picture), and the frame counter became Performance metrics
  // (the pause menu shows it too).
  Check(in_order(L::kDisplay, {"Aspect ratio", "Brightness", "Gamma", "VSync", "HDR"}) &&
            std::string(L::RowsOf(L::kDisplay).begin()[L::IndexOf(L::kDisplay, "Gamma")].section) == "Picture",
        "Display: Picture (brightness, gamma) after the screen rows");
  // Owner, 18 Oct (after task 222): Field of view, Motion blur and Camera shake (with its strength) moved to Display
  // (Camera, the last section); Performance metrics moved to Gameplay; the frame rate cap stays in Display.
  {
    const auto section = [](L::Page p, const char* label) {
      const int at = L::IndexOf(p, label);
      return at < 0 ? std::string() : std::string(L::RowsOf(p).begin()[at].section);
    };
    Check(in_order(L::kDisplay, {"VSync", "Frame rate", "HDR", "Field of view", "Motion blur", "Camera shake",
                                 "Camera shake strength"}) &&
              section(L::kDisplay, "Field of view") == "Camera" && section(L::kDisplay, "Motion blur") == "Camera" &&
              section(L::kDisplay, "Camera shake strength") == "Camera" &&
              L::IndexOf(L::kDisplay, "Camera shake strength") + 1 == int(L::RowsOf(L::kDisplay).size),
          "Display ends with Camera: field of view, motion blur, camera shake and its strength");
    Check(section(L::kDisplay, "Frame rate") == "Frame rate", "the frame rate cap stays in Display");
    Check(in_order(L::kGameplay, {"Pop-up style", "Performance metrics", "Startup logos"}) &&
              section(L::kGameplay, "Performance metrics") == "Overlay",
          "Gameplay: Performance metrics (Overlay) after the HUD");
    for (const char* moved : {"Field of view", "Motion blur", "Camera shake", "Camera shake strength",
                              "Performance metrics"}) {
      int pages = 0;
      for (int p = 0; p < L::kPageCount; ++p)
        if (L::IndexOf(L::Page(p), moved) >= 0 && L::IndexOf(L::Page(p), moved) != L::IndexOf(L::Page(p), "*"))
          ++pages;
      Check(pages == 1, (std::string(moved) + " is on exactly one page").c_str());
    }
    Check(L::IndexOf(L::kGraphics, "Motion blur") < 0 && L::IndexOf(L::kDisplay, "Performance metrics") < 0 &&
              L::IndexOf(L::kGameplay, "Field of view") < 0,
          "the moved rows left their old pages");
  }
  for (int p = 0; p < L::kPageCount; ++p)
    Check(L::IndexOf(L::Page(p), "Frame counter") < 0 || L::IndexOf(L::Page(p), "Frame counter") ==
              L::IndexOf(L::Page(p), "*"), "the old Frame counter row is gone (now Performance metrics)");
  Check(in_order(L::kAudio, {"Master volume", "Music volume", "Effects volume", "Voices volume", "Menu sounds"}),
        "Audio: master, music, effects, speech, menu sounds");
  Check(in_order(L::kControls, {"Camera sensitivity", "Camera horizontal", "Camera vertical", "Vibration",
                                "Button remapping", "Keyboard bindings"}),
        "Controls: sensitivity, inversion, vibration, bindings");
  Check(in_order(L::kGameplay, {"Subtitles", "Language", "Aiming visor", "Inventory", "Startup logos"}),
        "Gameplay: subtitles, language, HUD, then the rest");
  Check(in_order(L::kCheats, {"Cheats", "*"}), "Cheats: the switch, then the cheat list");
  Check(in_order(L::kAdvanced, {"PS2 movie folder", "Copy diagnostics"}), "Advanced: diagnostics");
  for (int p = 0; p < L::kPageCount; ++p) {
    // Every label once per page (rows are found by label), headings in one run each.
    std::map<std::string, int> seen;
    std::vector<std::string> headings;
    for (const L::Row& r : L::RowsOf(L::Page(p))) {
      Check(++seen[r.label] == 1, "a label appears once on its page");
      if (headings.empty() || headings.back() != r.section) headings.push_back(r.section);
    }
    std::map<std::string, int> runs;
    for (const auto& h : headings) Check(++runs[h] == 1, "each heading is one block on its page");
  }
  // Moved rows are on their new pages only.
  Check(L::IndexOf(L::kGraphics, "Graphics API") >= 0 && L::IndexOf(L::kAdvanced, "Graphics API") < 0,
        "Graphics API moved from Advanced to Graphics");
  Check(L::IndexOf(L::kGameplay, "Aiming visor") >= 0 && L::IndexOf(L::kControls, "Aiming visor") < 0,
        "the HUD rows moved from Controls to Gameplay");
  Check(L::IndexOf(L::kCheats, "Cheats") >= 0 && L::IndexOf(L::kGameplay, "Cheats") < 0 &&
            L::IndexOf(L::kGameplay, "Infinite ammo") < 0 && L::IndexOf(L::kCheats, "Infinite ammo") >= 0,
        "the cheats moved from Gameplay to their own tab");
  // The check the builders' pages go through (developer builds log it for the launcher and pause menu).
  std::string why;
  Check(L::InLayoutOrder(L::kDisplay, {{true, "Screen"}, {false, "Window mode"}, {false, "Monitor"},
                                       {false, "Window size"}, {true, "Frame rate"}, {false, "VSync"},
                                       {false, "Frame rate"}}, &why),
        "a Display page in order passes");
  Check(!L::InLayoutOrder(L::kDisplay, {{true, "Screen"}, {false, "Monitor"}, {false, "Window mode"}}, &why),
        "rows out of order fail");
  Check(!L::InLayoutOrder(L::kDisplay, {{true, "Window"}, {false, "Window mode"}}, &why),
        "a row under another heading fails");
  Check(!L::InLayoutOrder(L::kAudio, {{true, "Game audio"}, {false, "Surround"}}, &why), "an unknown row fails");
  Check(L::InLayoutOrder(L::kCheats, {{true, "Cheats"}, {false, "Cheats"}, {true, "Cheat list"},
                                      {false, "Infinite ammo"}, {false, "Revolver"}}, &why),
        "the cheat list's rows are one block");
  Check(!L::InLayoutOrder(L::kGameplay, {{true, "Cheats"}, {false, "Cheats"}}, &why), "no cheats on Gameplay");
}

void WhatsNewChecks() {
  // Task 144 / job J6: What's new never opens during first-time setup or over the New in this update screen;
  // only with the game files there and the shaders prepared. Every combination of the gate's inputs.
  int shown = 0;
  for (int bits = 0; bits < 512; ++bits) {
    kk::WhatsNewGate g;
    g.pending = bits & 1;
    g.modal_open = bits & 2;
    g.files_ok = bits & 4;
    g.in_setup = bits & 8;
    g.shader_screen = bits & 16;
    g.shaders_busy = bits & 32;
    g.shaders_wanted = bits & 64;
    g.shaders_finished = bits & 128;
    g.new_screen = bits & 256;
    const bool show = kk::ShowWhatsNewNow(g);
    if (!show) continue;
    ++shown;
    Check(g.pending && !g.modal_open, "only a waiting one, never over another pop-up");
    Check(g.files_ok, "never before the game files are given");
    Check(!g.in_setup, "never during any setup page (disc, install, artwork, shaders, downloads, graphics, prompts)");
    Check(!g.new_screen, "never over the New in this update screen");
    Check(!g.shader_screen && !g.shaders_busy, "never while shaders are prepared");
    Check(!g.shaders_wanted || g.shaders_finished, "only once the shaders were checked or prepared");
  }
  Check(shown > 0, "it does open once everything is ready");
  kk::WhatsNewGate ready;
  ready.pending = ready.files_ok = ready.shaders_wanted = ready.shaders_finished = true;
  Check(kk::ShowWhatsNewNow(ready), "after an update, with the shaders done, it opens");
  ready.new_screen = true;
  Check(!kk::ShowWhatsNewNow(ready), "the New in this update screen holds it back until Continue");
  ready.new_screen = false;
  ready.in_setup = true;
  Check(!kk::ShowWhatsNewNow(ready), "the setup screen asking for the game files holds it back");
}

namespace fs = std::filesystem;
namespace sf = kk::play::save_file;

std::vector<uint8_t> MadeUpSave(uint32_t seed, float volume, uint32_t subtitles) {
  std::vector<uint8_t> b(sf::kSaveSize);
  for (size_t i = 0; i < b.size(); ++i) b[i] = uint8_t((i * 131 + seed) & 0xFF);
  sf::WriteWord(b.data() + 0x28, 0x1234);  // a continue level: a used slot
  sf::WriteWord(b.data() + 0xA68, 4);
  for (int i = 0; i < 4; ++i) sf::WriteWord(b.data() + sf::kFileFields[i], std::bit_cast<uint32_t>(volume));
  for (int i = 4; i < sf::kOptionCount; ++i) sf::WriteWord(b.data() + sf::kFileFields[i], i == 4 ? subtitles : 0);
  sf::WriteWord(b.data() + sf::kInventoryCompanion, 0);
  return b;
}
void WriteFile(const fs::path& path, const std::vector<uint8_t>& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}
std::vector<uint8_t> ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
size_t CountFiles(const fs::path& dir, const std::string& prefix) {
  size_t n = 0;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    if (it->path().filename().string().starts_with(prefix)) ++n;
  return n;
}
float Volume(const std::vector<uint8_t>& b, int option) {
  return std::bit_cast<float>(sf::ReadWord(b.data() + sf::kFileFields[option]));
}
// Only the option words differ from `before`.
bool OnlyOptionsChanged(const std::vector<uint8_t>& before, const std::vector<uint8_t>& after) {
  if (before.size() != after.size()) return false;
  for (size_t i = 0; i < before.size(); ++i) {
    bool option = false;
    for (uint32_t f : sf::kFileFields) option |= i >= f && i < f + 4;
    option |= i >= sf::kInventoryCompanion && i < sf::kInventoryCompanion + 4;
    if (!option && before[i] != after[i]) return false;
  }
  return true;
}

void AllSavesChecks() {
  const fs::path root = fs::temp_directory_path() /
      ("kk-launcher-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  auto slot_file = [&root](int slot) { return root / ("slot" + std::to_string(slot + 1)) / "KKProfile.dat"; };
  const fs::path backups = root / "audio-option-backups";
  // Slots 1 and 2: saves. 3: none. 4: the game's empty header. 5: a save with an option out of range.
  const auto save1 = MadeUpSave(1, 1.0f, 0), save2 = MadeUpSave(2, 0.8f, 1);
  auto empty = MadeUpSave(4, 1.0f, 0);
  sf::WriteWord(empty.data() + 0x28, 0);
  auto broken = MadeUpSave(5, 1.0f, 0);
  sf::WriteWord(broken.data() + sf::kFileFields[4], 7);
  WriteFile(slot_file(0), save1);
  WriteFile(slot_file(1), save2);
  WriteFile(slot_file(3), empty);
  WriteFile(slot_file(4), broken);

  sf::Values values{};
  sf::Changed changed{};
  values[2] = 50;  // music
  changed[2] = true;
  values[4] = 1;  // subtitles on
  changed[4] = true;
  auto r = sf::WriteOptionsToSaves(slot_file, 5, backups, values, changed, -1);
  Check(r.written == 2, "both saves take the change");
  Check(r.failed == 1, "the save with an option out of range is refused");
  const auto after1 = ReadFile(slot_file(0)), after2 = ReadFile(slot_file(1));
  Check(Near(Volume(after1, 2), 0.5f) && Near(Volume(after2, 2), 0.5f), "music is 50% in both saves");
  Check(sf::ReadWord(after1.data() + sf::kFileFields[4]) == 1 && sf::ReadWord(after2.data() + sf::kFileFields[4]) == 1,
        "subtitles are on in both saves");
  Check(Near(Volume(after1, 0), 1.0f) && Near(Volume(after2, 0), 0.8f), "an option not changed keeps each save's value");
  Check(OnlyOptionsChanged(save1, after1) && OnlyOptionsChanged(save2, after2), "no other byte of a save changes");
  Check(!fs::exists(slot_file(2)), "an empty slot gets no save");
  Check(ReadFile(slot_file(3)) == empty, "the game's empty header is left alone");
  Check(ReadFile(slot_file(4)) == broken, "a refused save is left unchanged");
  Check(CountFiles(backups, "slot1-") == 1 && CountFiles(backups, "slot2-") == 1, "each changed save is backed up");
  Check(CountFiles(backups, "slot4-") == 0 && CountFiles(backups, "slot5-") == 0, "unchanged saves get no backup");
  {
    std::error_code ec;
    for (fs::directory_iterator it(backups, ec), end; !ec && it != end; it.increment(ec)) {
      const auto name = it->path().filename().string();
      if (name.starts_with("slot1-")) Check(ReadFile(it->path()) == save1, "slot 1's backup is the save before");
      if (name.starts_with("slot2-")) Check(ReadFile(it->path()) == save2, "slot 2's backup is the save before");
    }
  }
  Check(CountFiles(slot_file(0).parent_path(), "KKProfile.audio-pending-") == 0, "no temporary file is left behind");

  // The game being played writes its own save (the game thread): skipped here.
  values[2] = 25;
  r = sf::WriteOptionsToSaves(slot_file, 5, backups, values, changed, 1);
  Check(r.written == 1 && Near(Volume(ReadFile(slot_file(0)), 2), 0.25f), "the other saves take the change");
  Check(ReadFile(slot_file(1)) == after2, "the game's own save is skipped");

  // The same value again makes no new backup; many changes keep only the newest three per save.
  r = sf::WriteOptionsToSaves(slot_file, 5, backups, values, changed, 1);
  Check(r.written == 1 && CountFiles(backups, "slot1-") == 2, "an unchanged value makes no new backup");
  for (int v = 10; v <= 40; v += 10) {
    values[2] = float(v);
    sf::WriteOptionsToSaves(slot_file, 5, backups, values, changed, -1);
  }
  Check(CountFiles(backups, "slot1-") == 3 && CountFiles(backups, "slot2-") == 3, "the newest three backups are kept");
  Check(Near(Volume(ReadFile(slot_file(0)), 2), 0.4f) && Near(Volume(ReadFile(slot_file(1)), 2), 0.4f),
        "the last value is in every save");
  Check(sf::OptionsValid(ReadFile(slot_file(0)).data(), sf::kFileFields), "the saves stay valid");

  // Original modes / control indicators use the same atomic save writer, with unrelated bytes intact.
  auto before_modes = ReadFile(slot_file(0));
  values.fill(0); changed.fill(false);
  for (int i = 5; i < sf::kOptionCount; ++i) { values[i] = 1; changed[i] = true; }
  Check(sf::WriteOptions(slot_file(0), backups, 0, values, changed), "original video and control options save");
  auto after_modes = ReadFile(slot_file(0));
  for (int i = 5; i < sf::kOptionCount; ++i)
    Check(sf::ReadWord(after_modes.data() + sf::kFileFields[i]) == 1, "original option has its selected value");
  Check(sf::ReadWord(after_modes.data() + sf::kInventoryCompanion) == 1, "both original inventory words agree");
  Check(OnlyOptionsChanged(before_modes, after_modes), "changing bonus modes leaves progress, scores and other bytes untouched");
  auto progress_save = MadeUpSave(9, 1, 0);
  sf::WriteWord(progress_save.data() + 0x34, 41);
  for (int i = 0; i < 40; ++i) sf::WriteWord(progress_save.data() + 0x38 + i * 4, i == 0 ? 250000 : 0);
  for (int i = 0; i < 20; ++i) sf::WriteWord(progress_save.data() + 0x168 + 4 * i, i == 15 ? 1 : 0);
  const auto ep = kk::play::DecodeExtraProgress(progress_save);
  Check(ep.percent == 100 && ep.score == 250000, "private save progress and scores decode exactly");
  Check(kk::play::ExtraUnlocked(*kk::play::FindExtra(2), ep), "completed private save unlocks the alternate ending");
  Check(kk::play::ExtraUnlocked(*kk::play::FindExtra(15), ep) && !kk::play::ExtraUnlocked(*kk::play::FindExtra(3), ep),
        "each original saved online-code word unlocks only its own bonus");
  Check(kk::play::DecodeExtraProgress({}).score == 0, "missing save is safe");
  progress_save.resize(100);
  Check(kk::play::DecodeExtraProgress(progress_save).online_codes == 0, "truncated save has no unlocks");
  std::error_code ec;
  fs::remove_all(root, ec);
}
}  // namespace

// Owner, 18 Oct: the Workshop's menu of three and how Back walks out of it; Load earlier / later on a page that
// shows only some of the mods (Mods without the texture pack mods, Texture packs with only them).
void WorkshopChecks() {
  using kk::WorkshopBack;
  Check(kk::WorkshopBackStep(true, false) == WorkshopBack::kCloseInner, "Back closes a README or mod page first");
  Check(kk::WorkshopBackStep(true, true) == WorkshopBack::kCloseInner, "an inner page closes before anything else");
  Check(kk::WorkshopBackStep(false, false) == WorkshopBack::kToMenu, "Back from Mods / Texture packs / projects goes to the menu of three");
  Check(kk::WorkshopBackStep(false, true) == WorkshopBack::kToHome, "Back from the menu of three goes Home");

  // Load order: tex A, settings B, tex C, cutscenes D, tex E (true = shown on the Texture packs page).
  const std::vector<bool> tex = {true, false, true, false, true};
  Check(kk::WorkshopMoveTarget(tex, 2, -1) == 0, "Load earlier skips the mods another page shows");
  Check(kk::WorkshopMoveTarget(tex, 2, +1) == 4, "Load later skips the mods another page shows");
  Check(kk::WorkshopMoveTarget(tex, 0, -1) == -1, "the first shown mod cannot load earlier");
  Check(kk::WorkshopMoveTarget(tex, 4, +1) == -1, "the last shown mod cannot load later");
  std::vector<bool> others(tex.size());
  for (size_t i = 0; i < tex.size(); ++i) others[i] = !tex[i];
  Check(kk::WorkshopMoveTarget(others, 1, +1) == 3, "Mods page: settings mod moves past the cutscene mod");
  Check(kk::WorkshopMoveTarget(others, 1, -1) == -1, "Mods page: nothing shown before the first mod it shows");
  Check(kk::WorkshopMoveTarget({true, true}, 0, +1) == 1, "with every mod shown it is the neighbour (as before)");
  Check(kk::WorkshopMoveTarget(tex, -1, +1) == -1 && kk::WorkshopMoveTarget(tex, 5, -1) == -1,
        "a mod that is not switched on does not move");
  Check(kk::WorkshopMoveTarget(tex, 2, 0) == -1 && kk::WorkshopMoveTarget(tex, 2, 2) == -1, "only one step at a time");
}

// Task 228: the launcher's start marker (startup_recovery_state.h) and emptying the AMD driver
// shader caches (amd_shader_cache.h), in a made-up local app data folder.
// Task 350a: DirectX 12 first on Windows, Vulkan when it cannot start; everyone through the setup once more.
void SetupAgainChecks() {
  namespace sa = kk::setup_again;
  Check(sa::kRound >= 1, "Beta 14's round is 1 or later");
  Check(sa::Due(true, 0, false), "an existing install that has not had the round goes through the setup again");
  Check(!sa::Due(true, sa::kRound, false), "once had, never again (the player's later choices stay)");
  Check(!sa::Due(false, 0, false), "a fresh install has its own first-time setup instead");
  Check(!sa::Due(true, 0, true), "automated test runs keep their settings");
  // What the setup's pages show goes back to the defaults ...
  for (const char* name : {"native_backend", "kk_graphics_preset", "kk_original_look", "native_gi", "native_upscaler",
                           "kk_render_quality", "native_shadow_quality", "native_water_reflections",
                           "native_volumetrics", "ao_mode", "kk_frame_rate", "fullscreen", "kk_fullscreen_mode",
                           "kk_hdr", "kk_fov", "kk_motion_blur", "kk_camera_shake", "kk_button_prompts",
                           "kk_pad_prompts", "mnk_mode", "kk_camera_sensitivity", "kk_map_a", "kk_deadzone"})
    Check(sa::Resets(name, "KK"), name);
  Check(sa::Resets("keybind_a", "Input/Keybinds/Controller"), "the keyboard bindings go back too");
  // ... and the rest stays: installed downloads, the game files, audio, gameplay, cheats, setup records.
  for (const char* name : {"native_texture_pack", "native_texture_pack_folder", "kk_texture_pack_own_folder",
                           "kk_ps2_cutscenes", "kk_ps2_movie_folder", "game_data_root", "kk_upscalers_offered",
                           "kk_launcher_music", "kk_launcher_music_volume", "kk_skip_intros", "user_language",
                           "kk_cheats", "kk_cheat_infinite_ammo", "kk_achievement_toasts", "kk_performance_metrics",
                           "kk_setup_version", "kk_setup_again", "kk_settings_version", "kk_shaders_prepared",
                           "kk_last_version", "kk_update_channel", "kk_mods", "gpu_plugin", "kk_renderer"})
    Check(!sa::Resets(name, "KK"), name);

  // Startup recovery with DirectX 12 preferred: a DirectX 12 that cannot start switches to Vulkan (Windows), and
  // Vulkan failing too ends the chain; on Linux DirectX 12 is never a renderer.
  const fs::path dir = fs::temp_directory_path() /
      ("kk-recovery-d3d12-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  kk::startup::RecoveryState state(dir);
  state.preferred = state.selected = "d3d12";
#if defined(__linux__)
  Check(!state.Advance("could not start"), "Linux: DirectX 12 is not a native API");
#else
  Check(state.Advance("could not start"), "a DirectX 12 failure is recorded");
  Check(state.selected == "vulkan", "DirectX 12 could not start: Vulkan");
  Check(state.reason.find("DirectX 12") == 0 && state.reason.find("Using Vulkan") != std::string::npos,
        "the notice names both APIs");
  kk::startup::RecoveryState loaded(dir);
  Check(loaded.Load() && loaded.preferred == "d3d12" && loaded.selected == "vulkan", "the fallback is kept");
  Check(state.Advance("could not start"), "Vulkan failing as well is recorded");
  Check(state.selected == "failed", "both failed: the chain ends");
#endif
}

void LauncherGuardChecks() {
  using kk::startup::LauncherGuard;
  const fs::path dir = fs::temp_directory_path() /
      ("kk-launcher-guard-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const LauncherGuard guard(dir);
  Check(guard.Read().count == 0, "no marker: the last launcher start finished");
  Check(LauncherGuard::Next(guard.Read(), "vulkan") == 1, "a first unfinished start counts 1");
  Check(guard.Write("vulkan", "window", 1), "the launcher's marker is written");
  auto left = guard.Read();
  Check(left.api == "vulkan" && left.phase == "window" && left.count == 1, "a marker left over is read back");
  Check(!LauncherGuard::SwitchApi(left, "vulkan"), "one crash before the launcher showed does not switch API");
  Check(LauncherGuard::Next(left, "vulkan") == 2, "a second start on the same API counts on");
  Check(LauncherGuard::Next(left, "d3d12") == 1, "a start on the other API counts from 1");
  guard.Write("vulkan", "window", 2);
  left = guard.Read();
  Check(LauncherGuard::SwitchApi(left, "vulkan"), "two crashes in a row before the launcher showed switch API");
  Check(!LauncherGuard::SwitchApi(left, "d3d12"), "only the API that crashed is switched away from");
  guard.Write("vulkan", "shaders", 5);
  Check(!LauncherGuard::SwitchApi(guard.Read(), "vulkan"), "a crash while preparing shaders (both APIs) never switches");
  Check(guard.Read().phase == "shaders", "the shader phase is kept");
  guard.Clear();
  Check(guard.Read().count == 0, "a clean exit clears the marker");
  { std::ofstream(dir / "launcher.marker") << "classic window 1\n"; }
  Check(guard.Read().count == 0, "a marker naming no native API is ignored");
  { std::ofstream(dir / "launcher.marker") << "vulkan elsewhere 3\n"; }
  Check(guard.Read().count == 0, "a marker with an unknown phase is ignored");
  { std::ofstream(dir / "launcher.marker") << "vulkan"; }
  Check(guard.Read().count == 0, "a cut-off marker is ignored");
  std::error_code ec;
  fs::remove_all(dir, ec);
}

void AmdShaderCacheChecks() {
  namespace amd = kk::amd_cache;
  const fs::path local = fs::temp_directory_path() /
      ("kk-amd-cache-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Check(!amd::Present(local), "no AMD folder: nothing to offer");
  Check(amd::Clear(local).removed == 0, "no AMD folder: nothing is removed");
  fs::create_directories(local / "AMD" / "Other");
  Check(!amd::Present(local), "an AMD folder without the shader caches: nothing to offer");
  auto put = [](const fs::path& p, size_t n) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << std::string(n, 'x');
  };
  put(local / "AMD" / "VkCache" / "a.parc", 100);
  put(local / "AMD" / "VkCache" / "sub" / "b.parc", 50);
  put(local / "AMD" / "DxCache" / "c.bin", 10);
  put(local / "AMD" / "DxcCache" / "d.bin", 1);
  put(local / "AMD" / "Other" / "keep.txt", 7);  // not a shader cache: never touched
  put(local / "AMD" / "keep-root.txt", 3);
  Check(amd::Present(local), "the AMD shader cache folders are found");
#if defined(_WIN32)
  // A file another program has open (no delete sharing) is kept and counted.
  FILE* held = _wfsopen((local / "AMD" / "DxCache" / "held.bin").c_str(), L"wb", _SH_DENYRW);
  Check(held != nullptr, "a held file is made");
#endif
  const auto r = amd::Clear(local);
#if defined(_WIN32)
  Check(r.kept == 1, "a file in use by another program is kept");
  std::fclose(held);
#endif
  Check(r.removed == 4 && r.bytes == 161, "the three caches' files are removed");
  Check(fs::is_directory(local / "AMD" / "VkCache") && fs::is_directory(local / "AMD" / "DxCache") &&
            fs::is_directory(local / "AMD" / "DxcCache"),
        "the cache folders themselves stay");
  Check(!fs::exists(local / "AMD" / "VkCache" / "sub"), "emptied subfolders go");
  Check(fs::exists(local / "AMD" / "Other" / "keep.txt") && fs::exists(local / "AMD" / "keep-root.txt"),
        "nothing outside the three caches is touched");
  std::error_code ec;
  fs::remove_all(local, ec);
}

// Beta 12: the keyboard set's pictures for bound keys (key_glyphs.h): the mouse buttons are the mouse's pictures,
// modifiers and spaces dropped, every key of a binding in order, and the mouse movement picture.
void KeyGlyphChecks() {
  using namespace kk::key_glyphs;
  Check(PictureName("LMB") == "Mouse_Left" && PictureName("RMB") == "Mouse_Right" && PictureName("MMB") == "Mouse_Middle",
        "mouse buttons use the mouse pictures");
  Check(PictureName(" Ctrl+E ") == "E" && PictureName("Space") == "Space" && PictureName("").empty(),
        "keys keep their names without modifiers");
  Check(PictureNames("LMB,E") == std::vector<std::string>{"Mouse_Left", "E"}, "the default fire binding");
  Check(PictureNames("MMB, Z") == std::vector<std::string>{"Mouse_Middle", "Z"} && PictureNames("").empty(),
        "zoom binding, empty binding");
  Check(std::string(kMouseMove) == "Mouse_Simple" && PictureName("RMB") == kMouseRight, "mouse movement picture");
}

int main() {
  // Extras playback does not set either story-movie flag. SKIP must still
  // send Start, and must stop sending it once playback ends.
  std::map<uint32_t, uint32_t> movie;
  auto read_movie = [&](uint32_t address) { return movie.at(address); };
  movie[0x82CCAE80] = 0;
  Check(!kk::DevMoviePlaying(read_movie), "no movie player is idle without dereferencing it");
  movie[0x82CCAE80] = 0x1000;
  for (uint32_t flags = 0; flags < 8; ++flags) {
    movie[0x110C] = flags & 1;
    movie[0x1110] = flags & 2;
    movie[0x1130] = flags & 4;
    Check(kk::DevMoviePlaying(read_movie) == (flags != 0), "story and Extras playback enable scripted skip");
  }
  movie[0x110C] = movie[0x1110] = movie[0x1130] = 0;
  Check(!kk::DevMoviePlaying(read_movie), "completed Extras playback cannot pause the next screen");
  // A loaded save's video modes wait for the game's profile pointer and flip target (a launcher start loads
  // the save on the title, where sub_825EE130 read guest 0x4DA4 and crashed), then apply exactly once.
  // Task 213 (owner, 18 Oct): the game's Old movie filter never applies in normal play, whatever a save holds:
  // a nonzero value (1 sepia, 2 the other filter) is always forced off, with or without a load.
  {
    using kk::play::NextVideoModeStep;
    using kk::play::VideoModeStep;
    Check(NextVideoModeStep(false, 0x1000, 0x2000, 1, 0) == VideoModeStep::kNothing, "no load, nothing to apply");
    for (uint32_t profile : {0u, 0x1000u})
      for (uint32_t target : {0u, 0x2000u})
        for (uint32_t modes = 1; modes < 4; ++modes) {
          const auto step = NextVideoModeStep(true, profile, target, modes & 1, modes >> 1);
          Check(step == (profile && target ? VideoModeStep::kApply : VideoModeStep::kWait),
                "video modes apply only once both game pointers are set");
        }
    Check(NextVideoModeStep(true, 0x1000, 0x2000, 0, 0) == VideoModeStep::kNothing,
          "a save with both modes off calls nothing");
    Check(NextVideoModeStep(true, 0, 0, 0, 0) == VideoModeStep::kWait, "pending modes keep waiting before the profile");
    for (uint32_t old_movie : {1u, 2u})
      for (uint32_t flip : {0u, 1u}) {
        Check(NextVideoModeStep(false, 0x1000, 0x2000, flip, old_movie) == VideoModeStep::kApply,
              "an Old movie value in the profile is always forced off");
        Check(NextVideoModeStep(false, 0x1000, 0, flip, old_movie) == VideoModeStep::kWait &&
                  NextVideoModeStep(false, 0, 0x2000, flip, old_movie) == VideoModeStep::kWait,
              "the Old movie filter is forced off only once both game pointers are set");
      }
  }
  // Chained scripted Extras sessions: the launcher runs only the first, a started bonus hands on the rest.
  Check(kk::DevLauncherSession("a,b") == "a,b", "a single launcher session runs whole");
  Check(kk::DevLauncherSession("a,b|c|d") == "a,b", "only the first chained session runs");
  Check(kk::DevLauncherSession("") == "", "no launcher input stays empty");
  Check(kk::DevLauncherAfterStart("a,b") == "shot:returned-extras", "an unchained run only marks the return");
  Check(kk::DevLauncherAfterStart("a|c,d|e") == "shot:returned-extras,c,d|e", "the next session follows the return");
  Check(kk::DevLauncherAfterStart("a|") == "shot:returned-extras", "an empty trailing session is dropped");
  Check(kk::DevLauncherSession(kk::DevLauncherAfterStart("a|c|e")) == "shot:returned-extras,c",
        "each return runs exactly one further session");
  // Script buttons: existing names keep their bits; the left stick drives both axes and half pushes.
  Check(kk::DevScriptButton("A") == 0x1000 && kk::DevScriptButton("RIGHT") == 0x0008 &&
            kk::DevScriptButton("SKIP") == kk::kDevSkip && kk::DevScriptButton("RSUP") == kk::kDevRsUp,
        "existing script buttons are unchanged");
  Check(kk::DevScriptButton("NOPE") == 0 && kk::DevScriptButton("") == 0, "unknown script buttons press nothing");
  {
    uint32_t all = 0;
    for (const char* n : {"LSUP", "LSDOWN", "LSLEFT", "LSRIGHT", "LSHALF"}) {
      const uint32_t bit = kk::DevScriptButton(n);
      Check(bit && !(all & bit) && bit > kk::kDevSkip, "left stick names have their own bits");
      all |= bit;
    }
    const auto none = kk::DevScriptSticks(0x1000 | kk::kDevSkip);
    Check(!none.lx && !none.ly && !none.rx && !none.ry, "buttons alone leave both sticks centred");
    const auto up = kk::DevScriptSticks(kk::kDevLsUp);
    Check(up.ly == 32767 && !up.lx && !up.rx && !up.ry, "LSUP pushes the left stick fully forward only");
    const auto back = kk::DevScriptSticks(kk::kDevLsDown | kk::kDevLsLeft | kk::kDevLsHalf);
    Check(back.ly == -16384 && back.lx == -16384, "LSHALF halves both left axes");
    const auto look = kk::DevScriptSticks(kk::kDevRsRight | kk::kDevLsRight);
    Check(look.rx == 32767 && look.lx == 32767 && !look.ry && !look.ly, "both sticks can be held together");
    const auto right_half = kk::DevScriptSticks(kk::kDevRsUp | kk::kDevRsHalf);
    Check(right_half.ry == 16384 && !right_half.ly, "RSHALF still halves only the right stick");
  }
  // All launcher/pause slider ranges and F8's float/integer/opacity controls.
  for (auto range : std::vector<std::array<float, 3>>{{0,100,1}, {0,100,5}, {69,110,1}, {0,100,10},
       {0.05f,10,0.05f}, {0,50,1}, {0,1,0.05f}, {0,4,0.05f}, {0.25f,4,0.05f},
       {1,16,1}, {-2,1,0.05f}, {0.3f,1,0.05f}, {10,200,5}, {25,300,5}, {0,40,1}, {0.1f,5,0.05f}}) {
    const auto [lo, hi, step] = range;
    float v = lo;
    for (int i = 0; i < 2500 && v < hi; ++i) {
      const float next = kk::ui::StepSlider(v, lo, hi, step, 1);
      Check(next > v && next <= hi, "each right press advances and clamps"); v = next;
    }
    Check(v == hi, "slider reaches exact maximum");
    for (int i = 0; i < 2500 && v > lo; ++i) {
      const float next = kk::ui::StepSlider(v, lo, hi, step, -1);
      Check(next < v && next >= lo, "each left press advances and clamps"); v = next;
    }
    Check(v == lo, "slider reaches exact minimum");
    Check(kk::ui::StepSlider(lo,lo,hi,step,-1) == lo && kk::ui::StepSlider(hi,lo,hi,step,1) == hi,
          "slider endpoints stay clamped");
  }
  Check(kk::ui::StepSlider(1.000006f,0,100,1,-1) == 0, "save-file percent rounding reaches mute");
  Check(kk::ui::StepSlider(49.999996f,0,100,1,1) == 51, "rounded percentage advances one displayed step");
  kk::ui::DirectionRepeat repeat;
  Check(repeat.Update(true,0) && !repeat.Update(true,0.1) && repeat.Update(true,0.34), "press and initial repeat delay");
  Check(!repeat.Update(true,0.4) && repeat.Update(true,0.42), "held input repeats");
  Check(!repeat.Update(false,0.43) && repeat.Update(true,0.44), "quick second press is not lost");
  for (int fps : {30,60,120,144,240}) {
    kk::ui::DirectionRepeat clock;
    int presses = 0;
    for (int frame=0;frame<=fps*5;++frame) presses += clock.Update(true,double(frame)/fps);
    Check(presses>=63 && presses<=64, "held repeat rate stays the same across UI frame rates");
  }
  kk::CursorActivity cursor;
  kk::CursorActivity::Pad idle, stick, button;
  stick.axes[2] = 9; button.buttons = 1;
  Check(cursor.mouse, "pointer starts visible");
  cursor.Controller(idle, stick);
  Check(!cursor.mouse, "right stick hides pointer");
  cursor.Mouse();
  cursor.Controller(stick, stick);
  Check(cursor.mouse, "held controller does not override active mouse");
  cursor.Controller(stick, idle);
  Check(cursor.mouse, "controller release does not hide pointer");
  cursor.Controller(idle, button);
  Check(!cursor.mouse, "fresh button hides pointer");
  cursor.Mouse();
  Check(cursor.mouse, "mouse restores pointer");
  GraphicsPresetChecks();
  LauncherGuardChecks();
  AmdShaderCacheChecks();
  ExtrasChecks();
  ShaderProgressChecks();
  ShaderPreparedChecks();
  SetupAgainChecks();
  SettingsLayoutChecks();
  WhatsNewChecks();
  AllSavesChecks();
  WorkshopChecks();
  KeyGlyphChecks();
  std::cout << checks << " launcher checks passed\n";
  return 0;
}
