# The native renderer beta: launcher changes (for the release session)

Status: 12 October 2026, branch `beta-launcher` (from `beta` = `nr-integration` 1ba7339, launcher v1.9.11). Three
launcher features for the beta, the settings they add, what switching between beta and stable does with them, and
what was checked. Runs and captures: `F:\KK-native-renderer\analysis\beta-launcher\runs`.

## What a player sees

### 1. Setup ends with "Choose a look"

After the disc image is installed, the artwork taken from it and the native renderer's shaders prepared (or sent to
the background with "Continue in the background"), the setup screen shows one more step, **Choose a look**, step
"03 Play" of the stepper. Two cards side by side, each with a still from the player's own game files (the first
launcher backdrop; the Original card shows a smaller copy of it), a caption, a title, two lines and a fact line:

| Card | Applies |
|---|---|
| **Original**, "The Xbox 360 as it was" | the Original look (`kk_original_look`, `OriginalLookSettings`: 720p, 30 FPS, the console's AA and filtering, 69 degree FOV, motion blur, screen blur, fog, no AO) |
| **Remastered**, "Sharper, with soft shadows" | the Modern look, renamed **Remastered** on the Graphics page, with the preset below, **60 FPS** (`kk_frame_rate` 60), `ao_mode` 1 at strength 1, and with the native renderer `native_ao_method = gtao`, `native_ao_apply = opaque`, `native_ao_quality = auto` |

Both keep the native renderer (`kk_renderer = native`). Left / Right (and Up / Down) move between the cards, A or
Enter or a click picks one; the footer shows Change and Select. The step cannot be skipped (it has no Back); it ends
the setup and shows "Installed. Your copy of King Kong is ready to play." (or the shaders-not-prepared message).

**Which preset Remastered takes** (`RemasteredPresetIndex` in `launcher.cpp`; the card's fact line names it and the
graphics card):

| Machine | Preset | Why |
|---|---|---|
| Steam Deck (`SteamDeck=1`, or the board is Jupiter / Galileo) | Steam Deck (+ GTAO) | the Deck's own preset |
| Intel graphics, or a card with under 3 GB of its own memory (integrated) | Medium (+ GTAO) | draws at Quality (fewer pixels) |
| 8 GB or more of video memory | Ultra | Native render quality, 16x filtering, AO |
| any other dedicated card, or a card it can't tell (Linux) | High | Native render quality, 8x filtering, AO |

With `--native_render_scale=auto` (the beta's default) the native renderer draws at the scale the render quality
gives for the screen (`backend.md`, "Defaults"). Medium and Steam Deck with GTAO show as Custom in the Preset row
(those presets have no AO); High and Ultra match their presets.

**Frame rate**: Remastered sets 60 FPS, Original keeps the console's 30 (the Original look sets it). Look ->
Remastered with no settings put aside yet (the look step wasn't used) gives Medium at 60 FPS. The frame rate can
still be changed on the Gameplay page. The warning before Play ("Frame rate above 30 FPS") now opens only above
60 FPS, so a Remastered player is not asked at every Play (this beta has the high-frame-rate fixes).

**Changing it later**: Graphics > Look ("Original Xbox 360" / "Remastered"). Choosing Original at setup first
applies Remastered and then the Original look, so the settings the Original look puts aside (`kk_modern_settings`)
are the Remastered ones: switching Look to Remastered later gives exactly Remastered. With AO on and the native
renderer, Graphics shows **AO method** (GTAO (new) / Classic = `native_ao_method` gtao / legacy; GTAO also sets
`native_ao_apply = opaque`, Classic resets it).

**The renderer**: Graphics > Advanced > **Renderer**: "Native (beta)" or "Classic (emulated GPU)", with a description
saying Classic is there to compare. It is `kk_renderer` (below); changing it restarts the game when Play is pressed
(`gpu_plugin` is in the launcher's restart list), because the plugin is loaded before the launcher opens.

**Shader preparation wording**: wherever the native shaders are being prepared, the launcher says "Your PC may run
slower until this finishes, because preparing the shaders uses its processor." (`KK_SLOW_PC` in `launcher.cpp`):
the setup's shader step, the recompile pop-up, the "Preparing shaders" wait pop-up, and a short faint line under the
SHADERS progress bar on Home ("Your PC may run slower until this finishes.").

### The shader screen, at setup and after every update

The native renderer's shaders are prepared on a screen of their own in Home's look (`DrawShaderScreen` in
`launcher.cpp`): the stills (blurred, as behind the settings), the game's logo above, the launcher music, and a
centred panel like the pop-ups: a caption (PC PORT . SETUP or UPDATE), the title, a line on why ("The game's shaders
are translated for your graphics card from your copy of the game, once, so effects never stutter the first time they
appear."), the slower-PC line, the progress bar with "N of M shaders" and the percentage, and **Continue in the
background** (A, Enter or a click; the footer shows it). After that the build goes on behind Home (SHADERS status) and
Play waits for it. When the build ends on the screen, the screen closes by itself.

- **At setup** it replaces the old zero-asset step (by then the game is installed and the artwork taken): title
  "Preparing the shaders"; it is followed by the look step.
- **After every update** the first start of a version the launcher has not run before (`kk_last_version` differs from
  `KK_VERSION` on an installation that has run before: an update, a beta-to-beta update, a channel switch; the same
  test as What's new) **forces a full rebuild** of the pack (the Home recompile's forced build), whatever the stamp says,
  and shows the screen titled "Updated to vX: preparing the shaders". Not on a fresh install (the setup builds the
  pack), not when nothing changed, not in `KK_AUTOPLAY` test runs. What's new opens over it.

Checked: `runs\1012-162005-after-update` (a settings file from v1.9.10: `shader pack: rebuilding (asked for)`, the
screen with What's new over it, Continue in the background, Play waited, built in 40.5 s, the game started from it,
`kk_last_version` now 1.9.11) and `runs\1012-162109-setup-shaders` (`setup:4`, Continue, the look step, Remastered,
the game started). Dev token `updated` acts as the first start after an update.

### 2. Recompile shaders on Home

The SHADERS column of Home's status row (the native renderer's pack, shown when it is the renderer) is a button:

- controller / keyboard: Down from the menu's last entry (Quit) or Up from the first (Play) moves to it (the menu
  wraps through it); Up goes back to Quit, Down to Play, B back to the menu; the footer shows "A Recompile shaders";
- mouse: hovering shows it as a tile, a click opens the pop-up.

The pop-up: **Recompile shaders**, "Recompile your shaders? This takes about a minute.", **Yes** / **No** (B = No).
Yes starts a forced build (`NativeShaderPack::Start(..., force = true)`, the plugin's `start(..., force)`, interface
version 2): the status line shows "Preparing N%" with its progress bar, and Play waits for it with the existing
"Preparing shaders" pop-up. While a check or build runs, selecting it says "The shaders are being prepared right
now." Decided: **native only**. With the Classic renderer the column is the downloaded SHADER PACK (the runtime's
pipeline storage merged from GitHub); "recompiling" that would mean deleting the player's prepared pipelines, which
the runtime then prepares again as they play: not useful, and not easy to make safe.

Also fixed on the way: B while a Home side panel (Steam, Discord) had the focus opened the Quit pop-up (the global
Back ran first); it now goes back to the menu, as the footer said.

### 3. Display > Window mode, Resolution and Refresh rate (Beta 2)

**Window mode** is Windowed (`fullscreen` off), Borderless (`fullscreen` on, `kk_fullscreen_mode` `desktop`: a
window at the desktop's resolution, as v1.9.11's fullscreen) or Fullscreen (`kk_fullscreen_mode` a mode). Each shows
its own size rows: Windowed the Window size, Fullscreen **Resolution** (the monitor's sizes) and **Refresh rate**
(that size's rates, highest first), Borderless none. Choosing Fullscreen starts at the desktop's own mode
(`rex::ui::GetDesktopDisplayMode`). The launcher itself stays borderless in Fullscreen; the screen changes when the
game starts. Since Beta 2 the Display page also has **Frame rate** (from Gameplay) with VSync, and **Upscaling**
(Upscaler, Upscaler mode or Render quality, Internal resolution: from Graphics); the Reset page lists moved with
them. The list is the display's fullscreen modes (the Monitor setting's display, in SDL's order as the SDK
uses it, or the window's for Default), from 1280 x 720 up, each size once; a rate a hair under a whole one (59.94,
119.88) is left out when the whole one is there. On this PC's monitor SDL reports 307 modes: 22 sizes and up to 9
rates each in the rows. A saved size the display doesn't have stays listed as "(not on this display)".

How: SDL is linked into both `rexruntime.dll` (the SDK, which owns the window) and `king_kong.exe`, so the port's SDL
copy sees neither the game's window nor the displays (Beta 1 found this and shipped without the row). SDK patch 0015
(`tools/rexglue-patches`) adds `rex::ui::GetFullscreenDisplayModes` and `rex::ui::SetFullscreenDisplayMode`, which
use the SDK's SDL: SDL keeps a fullscreen mode per window and uses it every time the window is fullscreen, so
`kk::ApplyFullscreenMode` (`kk/src/display_modes.*`) sets it on the UI thread when Play is pressed (and when the
launcher is skipped), before the render quality is applied; the launcher itself stays borderless. Every later
fullscreen switch of the SDK's (the fullscreen setting) uses the mode. A mode the display doesn't have falls back to
the closest, and a failure to Desktop, logged as `KK: fullscreen mode ...`.

The render scale follows it: `OutputSize()` (`king_kong_app.h`) returns the chosen size in fullscreen, so
`ApplyRenderPreset` sets the draw resolution scale for it and `--native_render_scale=auto` takes that scale
(`AutoRenderScale`). The Graphics page's "Draws ... for your ... screen" shows the chosen size too. The native
shader pack is unaffected (with `auto` it always carries the render scale variants).

Checked: the mode list, the rows and the pages in the launcher (windowed, `runs\b11-launcher-*`). Not checked on
this PC: Borderless and Fullscreen themselves, which would cover or change the user's screen; the user tests them.

Alt+Tab (GPUnity, Beta 1: "Can't fully alt+tab out of the game"): the SDK's Vulkan presenter created its swapchain
without `VK_EXT_full_screen_exclusive`, which leaves it to the driver, and drivers may take a borderless window that
covers the screen into exclusive fullscreen by themselves (the stable version presents through DXGI, which doesn't).
SDK patch 0016 enables the extension (with `VK_KHR_get_surface_capabilities2` on the instance) and creates the
swapchain with `VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT`; `--vulkan_allow_fullscreen_exclusive` turns it back.

### 4. Beta 2: Discord status, GTAO only, no Supersampling

- **Discord status** (About > Launcher, `kk_discord_status`, on): Rich Presence through the Discord app's local
  connection (`kk/src/discord_presence.*`, the named pipe `discord-ipc-N`; a Unix socket on Linux), no Discord SDK.
  Shows "In the launcher" or "In the game", the time since, "PC port v<version>", and buttons to the website and the
  Discord server. Tries again every 20 seconds while Discord isn't running; switched off, the activity is cleared.
  The row is shown only when the port has a Discord application ID (`kApplicationId`; `--kk_discord_app_id` for
  testing). Checked against this PC's Discord with an invalid ID: the handshake, and Discord's refusal, logged.
- **GTAO only.** `native_ao_method` `gtao` and `native_ao_apply` `opaque` are the port's defaults
  (`ApplyPortDefaults`; the plugin's own defaults stay `legacy`/`resolve` for its tests). The AO method row is gone;
  a settings file's `native_ao_method`/`native_ao_apply` (Beta 1's choice) is reset at start (`ApplyFixedSettings`).
  Remastered no longer writes them. **AO strength** is now Light (1, recommended), Medium (2) and Strong (3): with GTAO
  it sets XeGTAO's final power (1.6, 2.2, 3.0) and the radius (6, 8, 12 near-plane distances), see `ao.md`. The
  Classic renderer keeps its own AO and the Intel note.
- **Supersampling** (`kk_render_quality` `supersample`) is gone: the row, the preset and the disabling it did. A
  settings file with it gets `native` (`ApplyFixedSettings`). Custom's Internal resolution stays.

## Settings

| Setting | Values (default first) | Where | Notes |
|---|---|---|---|
| `kk_renderer` | `native`, `classic` | `settings.cpp`, Graphics > Advanced > Renderer | at start `ApplyRendererChoice` (`launcher.cpp`, called from `OnPreSetup`) sets **gpu_plugin's default** to `native` or `xenos` (always `xenos` when `rexgpu-native` isn't there). So `gpu_plugin` itself is never written to the settings file; a `gpu_plugin` from the command line or the settings file still wins (the Renderer row says so, and is disabled for the command line). Choosing a renderer in the launcher takes a settings-file `gpu_plugin` out. |
| `kk_fullscreen_mode` | `desktop`, `WIDTHxHEIGHT@HZ` | `settings.cpp`, Display > Resolution and Refresh rate | set on the game's window when it starts (Beta 2) |
| `kk_discord_status` | `true`, `false` | `settings.cpp`, About > Discord status | Beta 2 |
| `kk_original_look` | (existing) | Graphics > Look | the label Modern is now Remastered |
| `native_ao_method`, `native_ao_apply`, `native_ao_quality` | (the plugin's) | no row since Beta 2 | the port's defaults are `gtao`, `opaque`, `auto` (`ApplyPortDefaults`); the plugin's stay `legacy`, `resolve`, `auto` for the golden-scene harness |

Reset page (Graphics) also resets `kk_renderer` and the three `native_ao_*`; Reset page (Display) `kk_fullscreen_mode`.

## Stable and beta

**Which update a player is offered** (`PickUpdate` in `kk/src/update.cpp`, used by `CheckForUpdate`):

| Channel | Offered |
|---|---|
| stable | the main repo's latest release when newer; betas never |
| beta | the beta repo's latest release when newer; a stable release only when its numbers (x.y.z) are at least the running build's and it is newer, i.e. the beta's own final release (stable 2.0.0 for 2.0.0-beta.N) or later; a lower stable (a 1.9.12 hotfix) is ignored. Beta repo empty or not answering: no update (not stable). Neither repo answering: "Could not check for updates." |
| Change version to stable | the main repo's latest, when it differs from this build (older too) |
| Change version to beta | the beta repo's latest, when it differs; none: "There's no beta build right now" (as before) |

Checked with the dev token `update-test` (`RunUpdatePickTests`, 13 cases, all ok in
`runs\1012-161018-beta1-changes`): beta on 2.0.0-beta.1 sees beta.2; ignores stable 1.9.12 (with and without a beta);
takes stable 2.0.0 (also over a newer beta) and 2.1.0; beta repo empty -> no update; stable never sees betas; both
switches. `update.h`'s header comment says the same. CHANGELOG.md's v1.9.10 notes ("until there is, the beta gets
stable versions", "the beta channel takes whichever of the two is newer") describe the old rule: the release notes of
the beta should say the new one.

From the release session's review of the update channel:

1. **Switching back to stable** (`FinishChannelSwitch`, when a stable version was found and is about to be
   installed): `ResetBetaOnlySettings` puts every setting only the beta has back to its default before the settings
   are saved: every `native_*` (and anything in the "Native renderer" category), `kk_renderer`, `kk_fullscreen_mode`
   and `gpu_plugin` (never saved by the beta, but an earlier test build's settings file may have `gpu_plugin =
   "native"`, which would stop stable at start: "Failed to load GPU plugin"). Command-line values are left alone.
   Saves, `kk_original_look` and every setting stable knows stay. The "Switch back to stable?" pop-up says so. The
   renderer choice is a beta-only setting mapped to `gpu_plugin` at start, as suggested, so a stable build that
   reads a beta's settings file sees no `gpu_plugin` at all (an unknown key such as `kk_renderer` is ignored by
   the SDK's config reader and dropped at the next save).
2. **Version order** (`changelog.cpp`): `CompareVersions` now uses the updater's `IsNewerVersion`, so
   `2.0.0-beta.1` comes after `1.9.11` and before `2.0.0-beta.2` and `2.0.0` (it read `2.0.0.1` before, after
   `2.0.0`). What's new uses it to pick the notes up to this version.
3. **2.0.0-beta.N** in the launcher: Home's VERSION column and About show `v` + `KK_VERSION` as it is
   (`v2.0.0-beta.1` fits the column), What's new is titled "What's new in v2.0.0-beta.1" and, with the ordering
   fix, shows the notes newer than the version the player had, up to this one. **CHANGELOG.md needs a
   `## v2.0.0-beta.1 (date)` entry**, or What's new opens empty after the update (checked with the dev token
   `whats-new` on v1.9.11: no newer entry gives an empty pop-up). `KK_VERSION` is set in `kk/CMakeLists.txt`
   (`1.9.11` today) or with `-DKK_VERSION=`. Not built with a beta version number here.
4. **The updater** copies top-level files plus `glyphs/` and `launcher_art/` (`kUpdatedDirs`). The beta needs only
   top-level files beside `king_kong.exe`: `rexgpu-native.dll`, `dxcompiler.dll`, and the shipped pipeline list
   `kkpipelines.list` when the release has one (`plugin_settings.cpp` looks for it in the program's folder). The
   plugin finds DXC in the program's folder too. The GTAO shaders are built into the plugin. Nothing lives in a new
   subfolder (the build folder's `native-renderer/` holds build intermediates only, not shipped).

## Checked

Developer build `kk-dev-vulkan` of this branch, RTX 4070 Ti, Vulkan, private user folders, launcher captures with
the `shot:` aid (`F:\KK-native-renderer\analysis\beta-launcher\runs`):
uns`):

| Run | What | Result |
|---|---|---|
| `1012-150926-pages1` | `setup:5`, keyboard Left / Right, mouse hover, A on Remastered; Home: Up from Play to SHADERS, A while a build runs, B back; Display; Graphics (Look Remastered, preset Ultra, AO method, Renderer) | all as designed; settings file: `kk_render_quality native`, `anisotropic_override 5`, `ao_mode 1`, `native_ao_method gtao`, `native_ao_apply opaque`, no `gpu_plugin` |
| `1012-151044-game-remastered` | the game with those settings, `KK_AUTOPLAY` (Play waits for the pack, built in 47.6 s) | `AO method gtao, apply opaque, quality auto`; GTAO computed every frame at the opaque pass's end; render scale auto 3840x2160; menus reached |
| `1012-151800-recompile` | Home: mouse click on SHADERS, the pop-up, Enter on Yes, Play during the build | `shader pack: rebuilding (asked for)`, status "Preparing N%", "Preparing shaders" pop-up, built in 42.4 s, the game started from the new pack |
| `1012-153324-fullscreen-mode` | fullscreen with a mode | found the SDL problem above; the feature is now off |
| `1012-154855-classic-switch` | Graphics > Renderer to Classic, Play | the launcher restarted the game; `KK: renderer xenos (kk_renderer classic)`; the Xenos renderer reached the menus; `kk_renderer = "classic"` saved, no `gpu_plugin` |
| `1012-161018-beta1-changes` | `update-test`; `setup:4` wording; look step -> Remastered; Home recompile with the new wording; Play at 60 FPS | the 13 update cases ok; `kk_frame_rate = 60` saved; no frame-rate warning at Play, the wait pop-up and the Home line show the new wording |
| `1012-155323-channel-checks` | a settings file with `gpu_plugin = "native"`, `kk_renderer`, `kk_fullscreen_mode`, `native_ao_*`; dev token `reset-beta` | all five reset, the saved file keeps only stable's settings |

## Decisions

- Remastered = the Modern look under a new name + a preset by graphics card + GTAO (opaque, quality auto). The plugin's
  AO defaults are not changed (the harness), the values are written to the settings file.
- The renderer is a beta-only setting (`kk_renderer`) mapped to `gpu_plugin`'s default at start, never `gpu_plugin`
  itself. Native is the default when the plugin is there.
- The look step is part of a fresh install only (players who update keep their settings).
- Recompile is native only (above).
- Fullscreen resolution: designed as SDL's per-window fullscreen mode set by the port; it needs an SDK addition
  (above), so it is off for the first beta rather than half working.

## Open problems

- The look step was checked with `KK_DEV_LAUNCHER_INPUT=setup:5` and by code reading (this PC has no disc image to run a
  real install, as for the shader step in `shaders.md`).
- The GPU tiers use dedicated video memory from DXGI (Windows); on Linux every card but the Deck gets High.
- GTAO on Intel graphics is not checked (the Remastered preset turns it on there too, with Medium).
- `launcher_text_table.inc` has no translations of the new strings.
- Display > Resolution's exclusive switch is not checked on this PC (above).
- Discord status needs the port's Discord application ID in `discord_presence.cpp`.
- The look step's Original card shows a smaller copy of the same still; the difference is slight.
- The Monitor row lists monitors in Windows' order (`EnumDisplayMonitors`) while the SDK and the Resolution row use
  SDL's order; with several monitors these can differ (as before this work).
