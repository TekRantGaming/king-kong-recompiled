// The description panel's structured help for each settings row (owner, 18 Oct, task 220): how the setting
// shows in this game, a short note for each option, and the impact labels (performance, video memory, when a
// change takes effect). The row's own description (launcher.cpp) is the panel's first paragraph: what the
// setting is. All text is English here and translated as drawn (settings_help_text_table.inc).
//
// Rows are found by their label (settings_layout.h) or by an Item's help_key when one label means different
// things (Upscaler mode with FSR 1 or NIS, the cheats' rows). The launcher's Settings and the pause menu attach
// these (launcher.cpp, AttachHelp); the setup's graphics pages keep their own short sentences.
//
// Costs: measured ones come from the renderer's documents (native-renderer/docs, codex/REPORT-101, task 206) on
// an RTX 4070 Ti; the rest are estimates (task 220's report lists which). Performance labels describe the cost
// of the setting's most demanding option against Off or Original.
#pragma once

#include <cstddef>
#include <string_view>

namespace kk::help {

enum class Cost : unsigned char { kUnset, kNone, kLow, kMedium, kHigh, kVaries };

// When a change takes effect.
enum class Applies : unsigned char {
  kAuto,       // "Applies when the game starts" for kRestartOnlyRows, else "Changes at once"
  kNone,       // no label (actions, information)
  kAtOnce,     // "Changes at once"
  kGameStart,  // "Applies when the game starts" (in game the row is greyed: "Can't change while playing")
  kNextStart,  // the launcher's own settings: "Applies the next time you start the game"
};

struct OptionNote {
  const char* option;  // the option's label (English); a row's option that starts with it matches
  const char* note;
};

inline constexpr int kMaxOptions = 7;

struct Help {
  const char* key;  // the row's label, or an Item's help_key
  Cost perf;        // kUnset: no performance label
  Cost vram;        // kUnset: no video memory label
  Applies applies;
  const char* how;  // what you notice in this game (nullptr: nothing more to say)
  // For a choice, only the notes of the options the row offers are shown (an upscaler this PC lacks is left
  // out); for a slider or an action, every note.
  OptionNote options[kMaxOptions];
};

// The rows that cannot change while the game runs (the pause menu greys them, LiveOnly in launcher.cpp): the
// window, resolution, refresh rate and monitor, VSync (Vulkan picks its present mode when the swapchain is
// made), render quality and its internal resolution, the look and preset, Recommended, the graphics API and
// renderer, the language and the startup logos.
inline constexpr const char* kRestartOnlyRows[] = {
    "Window mode", "Window size", "Resolution", "Refresh rate", "Monitor", "VSync", "Render quality",
    "Internal resolution", "Look", "Preset", "Recommended", "Renderer", "Graphics API", "Language",
    "Startup logos"};

inline bool RestartOnly(std::string_view label) {
  for (const char* r : kRestartOnlyRows)
    if (label == r) return true;
  return false;
}

using C = Cost;
using A = Applies;

// The rows of settings_layout.h in its order, then About's switches.
inline constexpr Help kHelp[] = {
    // ---------------------------------------------------------------- Display ---
    {"Window mode", C::kNone, C::kUnset, A::kAuto, nullptr,
     {{"Windowed", "Plays in a window on your desktop."},
      {"Borderless", "A window that fills the screen at your desktop's resolution. Alt+Tab is instant."},
      {"Fullscreen", "Switches your screen to the Resolution and Refresh rate below while the game runs."}}},
    {"Monitor", C::kNone, C::kUnset, A::kAuto, nullptr, {}},
    {"Window size", C::kMedium, C::kLow, A::kAuto,
     "The game draws its picture for this size, so a bigger window is sharper and asks more of your graphics card.",
     {}},
    {"Resolution", C::kHigh, C::kMedium, A::kAuto,
     "Render quality and the upscaler work from this size. A higher resolution is sharper, but the game draws more "
     "pixels.",
     {}},
    {"Refresh rate", C::kNone, C::kUnset, A::kAuto,
     "This is your screen's setting. How many pictures the game draws a second is the Frame rate below.", {}},
    {"Aspect ratio", C::kNone, C::kUnset, A::kAuto, nullptr,
     {{"Letterbox 16:9", "Keeps the game's shape, with black bars on wider or taller screens."},
      {"Stretch", "Fills the screen and stretches the picture to fit."}}},
    {"Brightness", C::kNone, C::kUnset, A::kAuto, nullptr, {}},
    {"Gamma", C::kNone, C::kUnset, A::kAuto, nullptr, {}},
    {"VSync", C::kNone, C::kUnset, A::kAuto, nullptr,
     {{"Off", "The lowest input delay. G-Sync and FreeSync screens run freely; other screens can show tearing."},
      {"On", "No tearing, with a little more input delay."}}},
    {"Frame rate", C::kHigh, C::kUnset, A::kAuto,
     "Above 30 is smoother, but some character animations are not right yet. A rate that divides your screen's "
     "evenly, such as 60 on a 120 Hz screen, moves most smoothly.",
     {{"30 FPS", "The Xbox 360's rate. Every animation is right, and it is the lightest."},
      {"60 FPS", "Twice as smooth. Needs about twice the work from your PC."},
      {"Unlimited", "As fast as your PC can go. Best with VSync or a G-Sync or FreeSync screen."}}},
    // HDR output (task 219: HDR follows Windows; the calibration button is the launcher's only).
    {"HDR", C::kNone, C::kUnset, A::kAuto, "Brighter highlights and deeper colour on your HDR screen.",
     {{"Follow Windows", "HDR when Windows HDR is on for this screen, SDR when it is off. It follows a change in Windows."},
      {"On", "Always asks for HDR. Windows HDR must be on too."},
      {"Off", "Always SDR, as on the Xbox 360."}}},
    {"HDR brightness", C::kNone, C::kUnset, A::kAuto, nullptr,
     {{"Automatic", "Both values come from Windows and follow it when it changes."},
      {"Manual", "Set Peak brightness and Paper white yourself with the sliders below."}}},
    {"Peak brightness (nits)", C::kNone, C::kUnset, A::kAuto,
     "Set it to your screen's peak brightness, so bright skies, fire and sparks keep their detail.", {}},
    {"Paper white (nits)", C::kNone, C::kUnset, A::kAuto,
     "Raise it if the picture looks dim next to Windows, lower it if it looks washed out.", {}},
    {"Open Windows HDR Calibration", C::kUnset, C::kUnset, A::kNone,
     "Measures your screen with a few test patterns. HDR brightness on Automatic then uses the results.", {}},
    {"Performance metrics", C::kLow, C::kUnset, A::kAuto, nullptr,
     {{"Off", "Nothing over the game."},
      {"FPS only", "The frame rate and the frame time."},
      {"Full", "Adds a frame time graph and the graphics card's and processor's load, temperature and memory."}}},

    // --------------------------------------------------------------- Graphics ---
    {"Look", C::kVaries, C::kUnset, A::kAuto, nullptr,
     {{"Original Xbox 360", "The game as it looked on the console, at 30 FPS, with no added effects. The lightest."},
      {"Remastered", "Presets, upscalers, ambient occlusion, global illumination and the other options below."}}},
    {"Preset", C::kVaries, C::kVaries, A::kAuto,
     "Shadows, water reflections, light shafts and the frame rate are not changed by a preset.",
     {{"Low", "Performance upscaling and short-range global illumination. For older PCs."},
      {"Medium", "Quality upscaling and medium global illumination."},
      {"High", "Adds strong ambient occlusion, high global illumination and 16x texture filtering."},
      {"Ultra", "The same as High for now."},
      {"Steam Deck", "Tuned for handheld PCs: light global illumination at short range."},
      {"Custom", "Your own mix of the settings below."}}},
    {"Recommended", C::kUnset, C::kUnset, A::kNone, nullptr, {}},
    {"Graphics API", C::kVaries, C::kUnset, A::kAuto, nullptr,
     {{"Auto", "Uses Vulkan."},  // Linux only; task 350a: Windows offers DirectX 12 (recommended) and Vulkan
      {"DirectX 12", "Recommended on Windows, and needed for AMD FSR 4. If it cannot start, the game uses Vulkan."},
      {"Vulkan", "Works on every graphics card the game supports."}}},
    {"Upscaler", C::kLow, C::kLow, A::kAuto,
     "DLSS, FSR 4, FSR 3.1 and XeSS rebuild detail from several frames and smooth edges too. Only those your "
     "graphics card runs are listed.",
     {{"Off", "Plain smooth scaling. No extra cost."},
      {"AMD FSR 1", "Scales and sharpens. Light, on any graphics card."},
      {"NVIDIA NIS", "Like FSR 1, from NVIDIA. Light, on any graphics card."},
      {"NVIDIA DLSS", "The steadiest, sharpest picture on NVIDIA RTX cards."},
      {"AMD FSR 4", "AMD's best, for Radeon RX 9000 cards with DirectX 12."},
      {"AMD FSR 3.1", "Runs on any card and costs the least of the four."},
      {"Intel XeSS", "Intel's upscaler. Best on Intel Arc, also runs on other cards."}}},
    {"Upscaler mode", C::kHigh, C::kMedium, A::kAuto, nullptr,
     {{"Native AA", "Draws every pixel and only smooths edges (DLAA with DLSS). The best picture, the slowest."},
      {"Quality", "Draws two thirds of the width and height. Close to Native, much faster."},
      {"Balanced", "Draws about 60% of the width and height."},
      {"Performance", "Draws half the width and height. A big gain, a little softer."},
      {"Ultra Performance", "Draws a third of the width and height. For slow PCs or 4K screens."}}},
    {"Upscaler mode (spatial)", C::kHigh, C::kMedium, A::kGameStart,
     "The game draws in steps of its original 720p, so two modes can be the same on your screen.",
     {{"Native", "Draws at your screen's size and only sharpens."},
      {"Quality", "Fewer pixels, a little softer, faster."},
      {"Balanced", "Fewer pixels again."},
      {"Performance", "The fewest pixels and the fastest."}}},
    {"Render quality", C::kHigh, C::kMedium, A::kAuto,
     "The game draws in steps of its original 720p and is scaled to your screen.",
     {{"Native", "Matches your screen. The sharpest."},
      {"Quality", "Fewer pixels, a little softer, faster."},
      {"Balanced", "Fewer pixels again."},
      {"Performance", "About half the width and height."},
      {"Ultra Performance", "The fewest pixels, for slow PCs."},
      {"Custom", "Pick an exact Internal resolution below."}}},
    {"Internal resolution", C::kHigh, C::kHigh, A::kAuto,
     "Without an upscaler it is never above your screen's size. Each step up costs a lot more.", {}},
    {"Anti-aliasing", C::kLow, C::kNone, A::kAuto,
     "Seen on branches, ropes and the edges of rocks and buildings. Not used while DLSS, FSR 3.1, FSR 4 or XeSS "
     "runs, which smooth edges themselves.",
     {{"Off", "The sharpest, with jagged edges."},
      {"FXAA", "Very light. Softens edges and a little fine detail."},
      {"SMAA", "Cleaner edges with less blur, at a slightly higher cost."}}},
    {"Sharpness", C::kLow, C::kNone, A::kAuto,
     "Uses AMD FidelityFX CAS. FSR 1, NIS, FSR 3.1 and FSR 4 use their own sharpening instead.",
     {{"Off", "No sharpening pass."},
      {"Low", "A light touch, good with DLSS or XeSS."},
      {"Medium", "Crisper textures."},
      {"High", "The strongest. Edges can look harsh."}}},
    {"Texture filtering", C::kLow, C::kNone, A::kAuto,
     "Most visible on paths, decks and rocky ground stretching away from you.",
     {{"Game", "What the game asks for, as on the Xbox 360."},
      {"Off", "Blurry at an angle."},
      {"2×", "A little sharper."},
      {"16×", "The sharpest. Costs very little on today's graphics cards."}}},
    {"Ambient occlusion", C::kMedium, C::kLow, A::kAuto,
     "Gives the jungle, the ruins and the caves more depth. Water, smoke, fire, fur and the sky are drawn after it, "
     "so they are never darkened.",
     {{"Off", "The game's own shading."},
      {"On", "Soft contact shadows everywhere. About 1 ms a frame at 1440p on a fast card."}}},
    {"Ambient occlusion (legacy)", C::kMedium, C::kLow, A::kAuto, nullptr,
     {{"Off", "The game's own shading."}, {"On", "Soft contact shadows everywhere."}}},
    {"AO strength", C::kNone, C::kUnset, A::kAuto, nullptr,
     {{"Light", "Subtle, close to the corners."},
      {"Medium", "Between the two."},
      {"Strong", "Deeper and wider shading."}}},
    {"AO quality", C::kLow, C::kUnset, A::kAuto, "More samples reduce grain on hands and flat surfaces.",
     {{"Low", "The fewest samples. A little grainy."},
      {"Medium", "The usual sample count."},
      {"High", "The smoothest. Costs more GPU time."}}},
    {"Global illumination", C::kMedium, C::kLow, A::kAuto,
     "Sunlit rock tints the shadow beside it and the jungle's green reaches the people in it. Only what is on screen "
     "bounces light.",
     {{"Off", "The game's own flat light in the shade."},
      {"Low", "Light reaches about 3 m. The lightest."},
      {"Medium", "Reaches about 5 m. About 1 ms a frame at 1440p on a fast card."},
      {"High", "Reaches about 8 m with a finer blur. The heaviest."}}},
    {"GI distance", C::kLow, C::kUnset, A::kAuto,
     "Light fades over the last quarter of the distance, so far scenery keeps the game's own light.",
     {{"Near", "16 m. Saves the most GPU time."},
      {"Medium", "32 m."},
      {"Far", "64 m. Bounced light across most of a scene."}}},
    {"Shadows", C::kLow, C::kLow, A::kAuto,
     "Sharper shadows from trees, rocks, people and Kong. Only the game's own shadows change; none are added.",
     {{"Original", "The game's blocky shadows."},
      {"High", "Twice the detail, with soft edges."},
      {"Ultra", "Twice the detail. Edges soften the farther a shadow falls from what casts it."}}},
    {"Water reflections", C::kLow, C::kLow, A::kAuto,
     "Seen on the sea, rivers and swamps. Water that is out of view or has nothing above it keeps the game's look.",
     {{"Original", "The game's own reflection."},
      {"Screen space", "Rocks, trees and the ship on screen mirror in the water."}}},
    {"Light shafts", C::kLow, C::kNone, A::kAuto,
     "Only where the game has fog and a sun shadow, so clear scenes and many chapters look the same.",
     {{"Original", "The game's own light beams only."},
      {"Volumetric low", "Soft shafts of sunlight. The lightest."},
      {"Volumetric high", "Finer shafts, with twice the samples."}}},
    {"Motion blur", C::kNone, C::kUnset, A::kAuto, nullptr,
     {{"Off", "Every frame stays sharp."}, {"On", "The game's own blur, as on the Xbox 360."}}},
    {"Screen blur", C::kNone, C::kUnset, A::kAuto,
     "The blur is sized for the Xbox 360's 720p, so on a sharper screen those levels look low resolution.",
     {{"Off", "Keeps those levels sharp."}, {"On", "The original look."}}},
    {"Distance fog", C::kNone, C::kUnset, A::kAuto,
     "The fog is part of Skull Island's look and also hides the edges of each area.",
     {{"Off", "Distant scenery is clear, but empty or unfinished backdrops can show."},
      {"On", "The game's own haze."}}},
    {"Horizontal flip", C::kNone, C::kUnset, A::kAuto, "Mirrors the whole picture from left to right.", {}},
    {"Texture pack", C::kLow, C::kHigh, A::kAuto,
     "Sharper textures on Kong, the characters and Skull Island. HD packs need more video memory.", {}},
    {"Texture packs in the Workshop", C::kUnset, C::kUnset, A::kNone, nullptr, {}},
    {"Texture pack folder", C::kUnset, C::kUnset, A::kNone, nullptr, {}},
    {"Effect resolution", C::kLow, C::kLow, A::kAuto,
     "The Xbox 360 drew them for 720p. Output draws them for your picture's size, so glows and blurs stay smooth on "
     "sharp screens.",
     {{"Original", "The game's own sizes. Blockier glows on big screens."},
      {"Output", "Matched to your resolution."}}},
    {"Rebuild shaders", C::kUnset, C::kUnset, A::kNone, nullptr, {}},

    // ------------------------------------------------------------------ Audio ---
    {"Master volume", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Music volume", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Effects volume", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Voices volume", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Menu music", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Menu music volume", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Menu sounds", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Menu sound volume", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Sound", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Sound to play", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Volume", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Open sounds folder", C::kUnset, C::kUnset, A::kNone, nullptr, {}},
    {"Reset page", C::kUnset, C::kUnset, A::kNone, nullptr, {}},

    // --------------------------------------------------------------- Controls ---
    {"Input", C::kNone, C::kUnset, A::kAtOnce, nullptr,
     {{"Controller", "Any controller works automatically."},
      {"Keyboard & mouse", "The game's hints show the keys and the mouse turns the camera."}}},
    {"Button prompts", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Camera sensitivity", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Camera response", C::kNone, C::kUnset, A::kAtOnce, nullptr,
     {{"Original", "The Xbox 360's own: small pushes up and down turn much more slowly than sideways."},
      {"Modern", "The same in every direction, so circles and diagonals feel even."}}},
    {"Stick deadzone", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Camera horizontal", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Camera vertical", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Left stick", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Aim", C::kNone, C::kUnset, A::kAtOnce, nullptr,
     {{"Hold", "Keep the left trigger held to raise the gun, as on the console."},
      {"Toggle", "Press it once to raise the gun and again to lower it. Pausing lowers it."}}},
    {"Vibration", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Vibration strength", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Mouse sensitivity", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Mouse camera", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Mouse vertical", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Button remapping", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Keyboard bindings", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Graphics panel", C::kUnset, C::kUnset, A::kNone, nullptr, {}},

    // --------------------------------------------------------------- Gameplay ---
    {"Subtitles", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Language", C::kNone, C::kUnset, A::kAuto, "The menus follow it too.", {}},
    {"Aiming visor", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Inventory", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Notifications", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Pop-up style", C::kNone, C::kUnset, A::kAtOnce, nullptr,
     {{"Xbox 360", "Unfolds from a green ring at the bottom of the screen."},
      {"Steam", "Slides up in the bottom-right corner."}}},
    {"Field of view", C::kLow, C::kUnset, A::kAtOnce,
     "A wider view shows more of the scene, so the game draws a little more.", {}},
    {"Camera shake", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Camera shake strength", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Startup logos", C::kNone, C::kUnset, A::kAuto, nullptr,
     {{"Play", "The logo movies play before the title screen."}, {"Skip", "Straight to the title screen."}}},

    // ----------------------------------------------------------------- Cheats ---
    {"Cheats", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Cheat", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},  // each row of the cheat list (help_key)

    // --------------------------------------------------------------- Advanced ---
    {"Notify me when downloads finish", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"PS2 cutscenes", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Download PS2 cutscenes", C::kUnset, C::kUnset, A::kNone, nullptr, {}},
    {"Remove PS2 cutscenes", C::kUnset, C::kUnset, A::kNone, nullptr, {}},
    {"PS2 movie folder", C::kUnset, C::kUnset, A::kNone, nullptr, {}},
    {"Copy diagnostics", C::kUnset, C::kUnset, A::kNone, nullptr, {}},
    {"Saved crash reports", C::kUnset, C::kUnset, A::kNone, nullptr, {}},

    // ------------------------------------------------------- About's switches ---
    {"Show this launcher", C::kNone, C::kUnset, A::kNextStart, nullptr, {}},
    {"Check for updates", C::kNone, C::kUnset, A::kNextStart, nullptr, {}},
    {"Online player count", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
    {"Discord status", C::kNone, C::kUnset, A::kAtOnce, nullptr, {}},
};

inline const Help* Find(std::string_view key) {
  for (const Help& h : kHelp)
    if (key == h.key) return &h;
  return nullptr;
}

// The note for one of a row's options (English labels), or nullptr.
inline const OptionNote* NoteFor(const Help& h, std::string_view option) {
  for (const OptionNote& n : h.options)
    if (n.option && option.substr(0, std::string_view(n.option).size()) == n.option) return &n;
  return nullptr;
}

// The impact labels (English, translated as drawn).
inline const char* PerfLabel(Cost c) {
  switch (c) {
    case Cost::kNone: return "Performance impact: None";
    case Cost::kLow: return "Performance impact: Low";
    case Cost::kMedium: return "Performance impact: Medium";
    case Cost::kHigh: return "Performance impact: High";
    case Cost::kVaries: return "Performance impact: Varies";
    default: return nullptr;
  }
}
inline const char* VramLabel(Cost c) {
  switch (c) {
    case Cost::kNone: return "VRAM: None";
    case Cost::kLow: return "VRAM: Low";
    case Cost::kMedium: return "VRAM: Medium";
    case Cost::kHigh: return "VRAM: High";
    case Cost::kVaries: return "VRAM: Varies";
    default: return nullptr;
  }
}
// How full a label's meter is (0..3; -1 for none, Varies has no meter).
inline int Level(Cost c) {
  switch (c) {
    case Cost::kNone: return 0;
    case Cost::kLow: return 1;
    case Cost::kMedium: return 2;
    case Cost::kHigh: return 3;
    default: return -1;
  }
}
inline constexpr const char* kChangesAtOnce = "Changes at once";
inline constexpr const char* kAppliesAtStart = "Applies when the game starts";
inline constexpr const char* kNotWhilePlaying = "Change from the main menu";  // in game: the launcher is "the main menu"
inline constexpr const char* kNextStartLabel = "Applies the next time you start the game";

}  // namespace kk::help
