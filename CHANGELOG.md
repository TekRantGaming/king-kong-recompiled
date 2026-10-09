# Changelog

Every release of the King Kong PC port, newest first. The launcher shows this on its About page, and a
version's notes the first time you start it.

## v1.9.6 (9 October 2026)

### What's new
- **Play on Linux and Steam Deck now, through Proton (experimental).** This is not the native Linux version: it's the Windows download running through Steam's Proton, so Linux players can play while the native Linux version is finished. That will come in a future update. To play, add **king_kong.exe** to Steam as a non-Steam game and set it to use **Proton Experimental** (the README has the steps). Tested on a ROG Ally (Z1 Extreme) with Bazzite: the launcher, installing from a disc image and the game all work, and it held 120 FPS on the 10 W power profile. The first start takes about half a minute while Steam sets Proton up for the game; after that it starts straight away. Under Proton the launcher uses your system's font, and **Add to Steam** is hidden, since the game is already in Steam.
- **Fixed: the game could freeze on the black LOADING screen** as the main menu loads. When a load starts, the loading screen and the game's frame presenter both need the graphics device, and if the loading screen got it first, each waited for the other forever. It happened often on Linux; on Windows the presenter nearly always got there first, but it could happen there too.
- **Fixed: one controller press counting twice in the launcher while Steam is open.** Steam's desktop controller layout also types keys for the controller's buttons (arrow keys, Enter), so the selection moved two places and switches flipped twice and stayed as they were. A press and the key Steam types for it now count once.
- **Fixed: the Home menu running into the status line in a small window.** Its entries now shrink to fit.

### Full changelog
- Loading screen: `loading_fix.cpp` hooks the present that the graphics device's worker thread makes each frame (`sub_8278D0C8`). While the loading screen thread (`sub_8278E450`) is up, it skips that present, since the loading screen draws every frame itself. Found by tracing the handshake between the two (`KK_DEV_LOAD_TRACE` in developer builds).
- Launcher input: `launcher_ui.cpp` reads the controllers before the keyboard and treats a key and a controller press of the same action within 0.25 s as one press.
- Proton: when Windows' fonts aren't there (Wine), the launcher loads Noto Sans (or DejaVu Sans, or Liberation Sans) from the Linux system through Wine's `Z:` drive. Proton plays videos it can't convert as colour bars, so stills that are colour bars are no longer used as backdrops; the launcher takes its artwork again once (a few seconds) to drop any it already took.
- Native Linux version (in development, not in this release): the launcher no longer swallows the window's paint events when it reads controllers, draws at 60 FPS (it used 72% of a CPU core), downloads with curl, takes its stills with ffmpeg and can update its AppImage. `tools/rexglue-patches` 0011 frees the game's memory however it ends (a crash or kill left it all in `/dev/shm` until a reboot), and 0012 adds the frame log to the Vulkan backend. The Vulkan renderer still draws some scenes wrong, which is why Linux goes through Proton for now.

## v1.9.5 (9 October 2026)

### What's new
- **A new launcher.** Rebuilt from scratch with a cleaner look, and it now works fully with a controller: move with the stick or D-pad, change pages with the bumpers, and the button pictures follow your **Button prompts** setting (Xbox 360, Xbox Series, PlayStation 5, PlayStation 2 or keyboard). Keyboard and mouse work everywhere too, and the text is sharp at any Windows display scale.
- **The game's own artwork.** The launcher takes the King Kong logo and stills from the movie trailer from your copy of the game, and the Home screen shows them as a slow slideshow. Nothing from the game is in the download. On a new install the launcher starts on a setup screen: pick your disc image, it installs, prepares the artwork, then opens.
- **The game's menu music and sounds.** The main menu music plays in the launcher, and moving around makes the game's own menu sounds. Both come from your copy of the game, and each has a switch and a volume slider under **About > Launcher**.
- **Add to Steam.** A new **Add to Steam** entry on the Home screen adds the port to your Steam library as a non-Steam game, with artwork from SteamGridDB, so you can start it from Steam, Big Picture or a Steam Deck. Steam closes for a moment while it's added and opens again afterwards. Steam Input is switched off for the game, so your controller keeps working.
- **Updates.** **Check for updates** is on the Home screen, and an update now downloads on a screen of its own with a progress bar.
- **The launcher in your language.** It follows the game's **Language** setting (English, French, German, Spanish or Italian), and the setup screen lets you pick it before you install.
- **New defaults.** New installs start with **AMD FSR 1** at **Quality**, and the **Medium** preset now uses it. If you're updating, your picture settings stay as they were. The **startup logos** are now skipped by default; turn them back on on the **Gameplay** page.
- **Shader preparing is always Balanced.** The setting is gone: Wait could pause the game for up to 2 seconds the first time an effect appeared, and Background could make objects vanish or flash.

### Full changelog
- Launcher: rewritten (`launcher.cpp` and a small toolkit in `launcher_ui.cpp`), with controllers read through SDL3 while it's open (let go before the game starts) and button pictures from the `glyphs` folder, plus the Xbox 360 set cut from the game's own button sheet.
- Sharp text: `rexruntime.dll` is rebuilt with `tools/rexglue-patches` 0010, which lets ImGui 1.92 bake its fonts at the size they're drawn.
- Artwork: `launcher_art.cpp` reads the front end's texture pack in `KKTextures.bf` (LZO1X chunks; the logo is a tiled 8:8:8:8 texture, the button sheet DXT5) and takes keyframes from `Video/Trailer.wmv` with Windows Media Foundation. It's kept in `Documents\king_kong\cache\art`.
- Music and sounds: from `Sound/Sound_Common.bf` through `SoundHeaders.db` (Xbox ADPCM, `game_sound.cpp`). The menu music is key 0x060002C9; the move, select and back sounds are 0x87003BF2, 0x87003BF4 and 0x87003D8A, the ones the game's main menu plays.
- Add to Steam: `steam_shortcut.cpp` adds the shortcut to `userdata/<account>/config/shortcuts.vdf` (binary VDF; every other entry is kept byte for byte) and sets `UseSteamControllerConfig` to 0 in `localconfig.vdf`, while Steam is closed, keeping a `.kk-backup` of each. The artwork is the top-scored grid, wide grid, hero, logo and icon of SteamGridDB game 5249080.
- Settings: `kk_launcher_music`, `kk_launcher_music_volume`, `kk_launcher_sounds`, `kk_launcher_sounds_volume`. `kk_settings_version` 2 keeps the old upscaler defaults (`present_effect` bilinear, `kk_render_quality` native) for settings files from earlier versions. `kk_skip_intros` defaults to on. Shader preparing is fixed at Balanced (`async_shader_compilation` on, the default `async_shader_wait_ms`).
- Translations: the launcher's 370 lines of text in German, Spanish, French and Italian.

## v1.9.2 (9 October 2026)

### What's new
- **Fixed: Necropolis and Brontosaurus looking low resolution** (#32). Those levels turn on one of the game's own effects, a light blur over the whole picture (any other level that uses it is fixed too). The blur is sized for the Xbox 360's 720p, so whatever resolution you played at, those levels came out about as sharp as 720p. It's a new **Screen blur** setting on the **Graphics** page, off by default, so those levels are as sharp as the rest of the game. The **Original Xbox 360** look keeps it on. Thanks to GrummelFritz and leocmp for sticking with this one and sending logs and screenshots.

### Full changelog
- Screen blur: `kk_big_blur` (off by default). The game's "BigBlur" after effect averages four slightly offset copies of the frame, with offsets for 1280 x 720; off skips it. Found by turning the game's after effects off one at a time in Necropolis and identifying the draw it adds by its shader key.

## v1.9.1 (8 October 2026)

### What's new
- **Fixed: the whole game drawing at low resolution for some players** (#32). The game picks its picture size from the TV it thinks it's connected to, and the port was giving it your window size. Windows display scaling shrinks the size the launcher saves (a 1280 x 720 window at 125% is kept as 1024 x 576), and below 720 lines the game falls back to standard definition: 640 x 480, stretched to widescreen, even in fullscreen and whatever your **Render quality**. It now always sees a 720p TV, as on an Xbox 360, and **Render quality** scales that up as intended. Thanks to GrummelFritz and leocmp for the reports and the log.

### Full changelog
- Video mode: the port sets `video_mode_width` and `video_mode_height` to 1280 x 720 each time it starts (they aren't saved to `king_kong.toml`). Before, the runtime reported `window_width` and `window_height` as the console's video mode whenever a window size was set. The log now notes the video mode at startup (`KK: video mode 1280x720 (window ...)`).

## v1.9.0 (8 October 2026)

### What's new
- **Ambient occlusion.** A new setting on the **Graphics** page adds soft shading where surfaces meet: in corners and creases, and on the ground under rocks, grass and people. It's off by default. Choose strength 1 (recommended), 2 or 3. It stays under the game's distance fog, so far-off walls don't show through the haze. It costs about 1 ms a frame at 4K, and works with NVIDIA and AMD graphics for now (not Intel yet) (#24).
- **AMD FSR 1 and NVIDIA Image Scaling.** A new **Upscaler** setting: **Off** (the default), **AMD FSR 1** or **NVIDIA NIS**. Both work on any graphics card. Each has **Native**, **Quality**, **Balanced** and **Performance** modes, and the launcher shows the resolution the game draws at in each one. They scale the picture up to your screen and sharpen it, so a lower resolution still looks crisp.
- **Original Xbox 360 or Modern.** A new **Look** setting at the top of the **Graphics** page. **Original** is the game as it was on the console: 720p at 30 FPS, the console's own anti-aliasing and texture filtering, the 69° field of view, motion blur and fog, no ambient occlusion and no upscaler. **Modern** gives you every setting, and switching back to it puts your own settings back.
- **Graphics presets.** **Low**, **Medium** (the default settings), **High**, **Ultra** and **Steam Deck** set the upscaler, render quality, anti-aliasing, texture filtering and ambient occlusion in one click. Change any of them yourself and it shows **Custom**.
- **Distance fog on or off.** **Off** clears the haze over far scenery. It's on by default, as the fog is part of Skull Island's look and also hides the edges of each area, so some empty backdrops can show with it off.
- **Performance draws one step lower.** The game draws in steps of its original 720p, so on a 4K screen **Performance** used to come out the same as **Quality** (1440p). It now goes one step lower where there is one: 720p at 4K, 2160p at 8K.

### Graphics presets
- **Low:** AMD FSR 1 in Performance mode, FXAA, 2x texture filtering.
- **Medium** (the default settings): no upscaler, Native, FXAA, 4x texture filtering.
- **High:** Native, FXAA, 8x texture filtering, ambient occlusion.
- **Ultra:** Supersample, FXAA Extreme, 16x texture filtering, ambient occlusion.
- **Steam Deck:** Native, FXAA, 2x texture filtering.

### Full changelog
- Ambient occlusion: built into the GPU plugin (`rexgpu-xenos.dll`, patches 0003 to 0007 in `tools/rexglue-patches`), set with `ao_mode` (0 off, 1 on) and `ao_strength` (1 to 3). Once a frame, at half resolution, it reads the scene depth the game copies out before lighting, works out the occlusion from 10 samples, blurs it and scales it up to full resolution. It's applied to the scene before the game's post effects, so the HUD isn't touched, and it uses the game's own fog values so only the part of the scene in front of the fog is darkened. Measured at 0.7 to 1.0 ms a frame at 3840 x 2160 on an RTX 4070 Ti. It needs the host render target path that NVIDIA and AMD graphics use; Intel graphics use a different path that isn't supported yet.
- Distance fog: `kk_fog` (on by default). Off skips the game's fog pass.
- Look: `kk_original_look`. While it's on, your Modern settings are kept in `kk_modern_settings`, and the Original values are set again each time the game starts in case the settings file was edited.
- Upscalers: `present_effect` (`bilinear` for Off, `fsr`, `nis`) and `present_nis_sharpness` (0.5). `rexruntime.dll` is now built from the ReXGlue SDK source with patch 0008: AMD FSR 1 and CAS without the AMD SDK, and the NVIDIA Image Scaling SDK v1.0.3 as a compute pass, run more than once for scale factors over 2x like FSR's own passes. The patch also fixes a heap in the presenter that only had room for two in-between images, too few for FSR from 720p to 8K. Their MIT licences are in `THIRD-PARTY-NOTICES.txt`.
- Upscaler mode: sets `kk_render_quality` (`native`, `quality`, `balanced`, `performance`). While an upscaler is on, it replaces the **Render quality** setting. `performance` now goes one 720p step below `quality` where there is one, with or without an upscaler.
- Presets: the Graphics page shows which preset your settings match. Motion blur and fog aren't part of them.

## v1.8.0 (8 October 2026)

### What's new
- **Shader freezes cut from 2 seconds to under half a second.** A new **Balanced** choice for **Shader preparing** on the **Graphics** page, now the default for everyone. New effects are prepared on many threads at once in the background, and each frame waits a moment for them, instead of the game stopping while they're prepared one at a time. Once in a while an object can appear a moment late the first time it's seen. **Wait** and **Background** are still there if you prefer them. The numbers are below.
- **The shader pack looks after itself.** Each time the launcher opens, it downloads the newest shader pack if you don't have it yet. There's no button to click and nothing to remember. The Play page shows how it went, and if you press **PLAY** while a download is still going, the game starts as soon as it finishes.
- **Prepared effects are kept after a crash.** If the game closed while saving a newly prepared effect, everything prepared after that point was ignored and had to be prepared again. Now only the damaged entry is dropped, and the rest is kept.
- **New defaults.** FXAA is on, motion blur is off, **Controller sensitivity** is 150% and the **Stick deadzone** is 5%. The achievement sound is the Xbox 360's when you have `Xbox_360.wav` in the `sounds` folder; otherwise the built-in chime plays as before. Settings you had changed from the old defaults stay as you set them.
- The "share your shaders" pop-up and the **Share my shaders** button are gone.

### Performance
Tested on the V-Rex chapter at 30 FPS on its first visit, with the graphics driver's own shader cache bypassed so every effect really was new.
- **Longest freeze:** 2.08 seconds with Wait (the old default), 0.45 seconds with Balanced.
- **All the freezes added up:** 3.9 seconds with Wait, 1.3 seconds with Balanced.
- **With shader pack 2:** the longest freeze was 0.33 seconds with Wait and 0.16 seconds with Balanced.

The freezes came as the chapter started, when the game meets the most new effects at once: up to about 240 in a second, each taking the graphics driver about 20 ms to prepare. In the minute of play after that, neither mode froze. Without a shader pack, Balanced drew about 80 objects a frame late in that minute; with shader pack 2, none.

### Full changelog
- Shader preparing: `async_shader_compilation` is now on by default with a new `async_shader_wait_ms`. A draw whose effect is still being prepared waits for it within a budget for each frame of half a frame at your frame rate cap (16 ms at 30 FPS, 8 at 60, 4 from 120 or unlimited), and is skipped only once that is used up. **Background** sets the wait to 0, and **Wait** turns preparing in the background off.
- GPU plugin: `rexgpu-xenos.dll` is now built from the ReXGlue SDK 0.10.0 source with the port's patch for that wait. The patch and build steps are in `tools/rexglue-patches`.
- Shader cache: at startup the port checks the cache's records and drops any that were only partly written, instead of the runtime ignoring everything after the first one.
- Shader pack: downloaded automatically each time the launcher opens, whether or not update checks are on. The **Shader pack update** pop-up and the **Download shader pack** button are gone. Shader pack 2 is still the latest; it covers about 40% of the game's effects, and Balanced handles the rest smoothly.
- Defaults: `swap_post_effect` fxaa, `kk_motion_blur` false, `kk_camera_sensitivity` 150, `kk_deadzone` 5, `kk_achievement_sound_file` `Xbox_360.wav` (the built-in chime if that file isn't in the `sounds` folder). VSync off (`d3d12_allow_variable_refresh_rate_and_tearing`), 4x texture filtering (`anisotropic_override` 3) and the Modern camera response were already the defaults; the port now sets the first two itself.
- Launcher: the share-shaders poster (`kk_share_poster`) and the **Share my shaders** button are removed.
- `tools/shader_coverage.py`: measures how many of the game's own effects (listed in its `Shaders/xeshaders.bin`) a shader cache has prepared.

## v1.7.3 (8 October 2026)

### What's new
- **Controller sensitivity speeds up the camera itself.** Before, the slider could only make smaller pushes of the stick reach the game's top turning speed: a full push turned just as fast whatever you chose. Now 200% turns twice as fast at every push, a full push included, both looking around and while aiming (#19).
- **Modern camera response, on by default.** A new **Camera response** setting on the **Controls** page. **Modern** turns the camera the same way in every direction, so circles and diagonals feel even. **Original** is the Xbox 360's own response, where small pushes up and down turn much more slowly than small pushes sideways. A full push straight across or straight up turns at the game's own top speed either way.
- **90 FPS.** The frame rate choices now include 90 FPS. They're a dropdown now, so every choice fits even in a small launcher window. As with any rate above 30, the launcher explains the animation issue when you press Play.

### Full changelog
- Controller sensitivity: scales the camera's own turn each frame (the turn the game's camera code applies left and right and up and down) instead of the stick, which the game caps at a full push. Keyboard and mouse still uses **Mouse sensitivity**.
- Camera response: `kk_camera_modern`, on by default. The game reads the right stick one direction at a time, with a 15% deadzone on each, then turns by the square of the push left and right and the cube of the push up and down. Modern reshapes the stick the camera reads so both follow the square of how far you push, in the direction you push. A half push now gives a quarter of the top speed in any direction; before, it was 17% left and right and 7% up and down.
- Frame rate: 90 added to `kk_frame_rate`'s choices, and the setting is a dropdown.

## v1.7.2 (7 October 2026)

### What's new
- **999 bullets lasts the whole chapter.** The **999 bullets** cheat was undone as soon as a chapter started: the game hands out each chapter's own weapons and ammo, so in V-Rex you got the machine gun's usual 30 rounds and nothing more. The port now gives the cheat again right after a chapter or checkpoint starts, so you really have 999 rounds. The weapon cheats (revolver, machine gun, shotgun and sniper rifle) had the same problem and are fixed too.

### Full changelog
- Cheats: about a second after a chapter or checkpoint hands out its loadout, the port gives the chosen weapon and ammo cheats again. Before, they were only given at the main menu.

## v1.7.1 (7 October 2026)

### What's new
- **The About page is always reachable.** Since the Cheats page was added in v1.6.0, the launcher's last page, **About**, was cut off the bottom of the page list when the launcher window was small (for example 1280 x 720), with no way to scroll to it. The list now shrinks to fit, so every page is always there.

### Full changelog
- Launcher: the page list's entries get shorter when the window doesn't have room for all of them at full height.

## v1.7.0 (7 October 2026)

### What's new
- **Field of view slider.** A new **Field of view** setting on the launcher's **Gameplay** page widens the view from 69° (the original) up to 110°. The game draws the wider area too, so nothing pops in at the edges. Jack's gun keeps its usual size, the menus keep their original look, and cutscenes widen by the same amount (#3).
- **Controller sensitivity works up and down too.** The **Controller sensitivity** slider on the **Controls** page (it was called Camera speed) now changes how fast you look up and down, not just left and right. A full push of the stick still turns at the game's own top speed. **Mouse sensitivity** and **Mouse camera** are now always shown on the Controls page.
- **Motion blur on or off.** A new **Motion blur** setting on the **Graphics** page. Off removes the ghost trail the game blends over fast moments, mostly in Kong's sequences and some transitions.
- **Skip the startup logos.** A new **Startup logos** setting on the **Gameplay** page skips the Ubisoft, Universal and WingNut movies, so the title screen appears within a few seconds. The story movies still play.
- **Reset a page.** Every settings page has a **Reset page** button that puts just that page's settings back to their defaults.
- **What's new and the changelog.** After an update, the launcher shows what changed the first time it opens. The **About** page now lists the notes for every version.

### Full changelog
- Field of view: `kk_fov`, in degrees for Jack's camera (69 is the original). Every camera is widened by the same amount, so close-ups stay closer than wide shots. The area the game draws widens with it. Jack's gun is drawn with its own fixed angle and doesn't change. Nothing is widened while the menus show.
- Controller sensitivity: `kk_camera_sensitivity` now scales the right stick as the game reads it, instead of the raw stick, so both directions change together and it is no longer limited below a full push. It no longer applies to keyboard and mouse, which has its own **Mouse sensitivity**.
- Motion blur: `kk_motion_blur`, on by default as on the console. Off runs the game's motion blur effect at zero strength, the game's own way of having no blur.
- Startup logos: `kk_skip_intros`, off by default.
- Reset page: the Display, Graphics, Gameplay, Controls (including button remapping and keyboard keys), Cheats and Achievements pages each reset only their own settings. Press **Save** to keep them.
- Changelog: `CHANGELOG.md` is built into the launcher, and the launcher remembers the last version it ran (`kk_last_version`) to know what's new.

## v1.6.0 (7 October 2026)

### What's new
- **No more freezes with PlayStation, Xbox Series or keyboard button prompts.** If you picked any button prompts other than the original Xbox 360 ones, the game could freeze for 1 to 5 seconds at a time, again and again, even during the intro videos. The port was searching the game's memory for the button picture in a way that held up the graphics. It now finds it instantly, and the freezes are gone (#11).
- **Cheats page.** All ten of the game's cheats are now switches on the launcher's new **Cheats** page: all chapters, all bonus content, fast healing, one-hit kills, 999 bullets, unlimited spears, and the revolver, machine gun, shotgun and sniper rifle. Switch them on, and they're active as soon as the game reaches its main menu. No more typing codes every time you play (#29).
- **Shader pack 2, and shader pack updates in the launcher.** A new shader pack covers more of the game, including the V-Rex fight and the damage effects. The launcher now checks for a newer shader pack when it opens, as well as for a newer version of the port, and offers whichever is out: they're released separately, so you're always on the latest of both.

### Full changelog
- Button prompts: the port looked for the game's button picture every second by asking Windows about each 64 KB of the game's 512 MB of memory. Each search took about 5 seconds and held the memory lock the graphics need. It now reads the emulator's own record of which memory is in use, so a search takes a few milliseconds.
- Cheats: the launcher's **Cheats** page (`kk_cheats`, `kk_cheat_*`). The port makes the same changes the game makes when a code is typed on its Cheat screen, when the main menu appears, and again each time you return to the main menu.
- Updates: the startup check (and **Check for updates now** on the About page) also checks the shader pack. Players with an older pack get a **Shader pack update** pop-up with **Download now** and **Later**; it waits until after a port update, since that restarts the launcher.
- Shader pack 2: 4,931 pipelines and 2,438 shaders (pack 1 had 4,652 and 2,362).
- For bug reports: a hidden setting, `kk_hitch_report_ms`, writes a report to the `logs` folder when a frame takes longer than that many milliseconds, showing what the game was waiting on. Off by default.

## v1.5.1 (7 October 2026)

### What's new
- **No more antivirus warnings.** Some antivirus programs, including Windows Defender, wrongly flagged a file that comes with the port (`rexruntime.dll`, part of the ReXGlue toolkit the port is built with) as a virus. It was a false alarm, but it could block the download. This release ships that file rebuilt from the exact same ReXGlue source, and antivirus programs no longer flag it.
- **Saving always works.** Some players were told to sign in before they could save, and the game never offered their save slots. This happened when the PC saw more than one controller, which is common with Steam Input, DS4Windows, or a handheld's built-in controls. The game then treated the player as player 2, and only player 1 can save. Every controller, plus keyboard and mouse, now counts as player 1, so saving works whichever one you use (#9).
- **Rumble reaches your controller.** The game shakes the controller, for example when you fire a gun, but that rumble only went to the first controller Windows listed. With Steam Input, a wireless receiver or more than one pad, that wasn't always the one in your hands. It now reaches every connected controller, so you feel it whichever one you use.
- **Freeze reports.** If the game ever stops responding for 20 seconds, the port now writes a freeze report to the `logs` folder. If the game recovers, the log notes how long it froze. Attaching those files to a bug report lets us find what caused the freeze (#11).

### Full changelog
- Ships `rexruntime.dll` rebuilt from the same ReXGlue v0.10.0 source, unchanged. The official build was an antivirus false positive (rexglue/rexglue-sdk#485). `rexgpu-xenos.dll` is the official one, which was never flagged.
- Controllers: all controllers and keyboard & mouse now count as player 1. Before, each controller got its own player number in connection order, and the game refuses to save for players 2 to 4 because they have no profile. The game's rumble for player 1 now goes to every connected controller too.
- Freeze reports: a watchdog writes `logs\hang-<date-time>.txt` (where every part of the game was at that moment) and `logs\hang-<date-time>.dmp` when no frame has been drawn for 20 seconds. Time the PC spends asleep isn't counted.
- Crash and freeze reports can now be traced to the exact part of the game's code, so problems players report are quicker to fix.

## v1.5.0 (7 October 2026)

### What's new
- **The Language setting works.** Picking Deutsch, Español, Français or Italiano in the launcher now changes the game's language. Before, the game always came up in English whatever you chose (#6).
- **Toggle aim.** A new **Aim** setting on the launcher's **Controls** page: **Hold** (the default, as on the console) or **Toggle**. On Toggle, press the left trigger once to raise the gun and again to lower it, without holding it down. Pausing (Start or Back) lowers it. It works with keyboard and mouse too (#21).

### Full changelog
- Game language: the game asks the system which language to use, and the runtime always answered English. It now answers with the launcher's Language setting (English, German, French, Spanish or Italian, the languages on the disc).
- Controls: **Aim: Hold / Toggle** (`kk_toggle_aim`). Each press of the left trigger flips between released and fully held. Start, Back or unplugging the controller releases it.

## v1.4.0 (7 October 2026)

### What's new
- **Updates from the launcher.** When the launcher opens it checks whether a newer version of the port is out. If one is, it offers to update: **Update now** downloads it, replaces the port's own files and restarts the launcher, with your installed game, saves and settings left as they are. **What's new** opens the release notes. You can turn the check off, or check by hand, on the **About** page. From this version on, you won't need to download releases yourself.
- **Share my shaders.** The shader pack only covers what has been played so far, and you can help fill in the rest. Once you have played a good part of the game, open the **Play** page and click **Share my shaders**. It packs your shaders into one small file, shows it to you and opens a GitHub form to drop it into. The file holds only shader data: nothing personal, no saves or settings. Shaders that at least two players have sent go into the next shader pack, so everyone after you gets a smoother first play-through.
- **Your country needs your shaders.** A pop-up when the launcher opens explains the above, with a poster to match. Tick **Don't show this message again** and it won't appear again.
- **MSAA is no longer a setting.** 2x MSAA is how the Xbox 360 drew those surfaces, so it now always matches the console, and the shader pack covers everyone.

### Full changelog
- Launcher: automatic update check at startup (About page: Updates, Off / At startup, plus **Check for updates now**), with **Update now**, **What's new** and **Later**. Updates are staged next to the old files and swapped in, so a failed download never leaves a half-updated game.
- Launcher: **Share my shaders** on the Play page, and the **Share shaders** issue form on GitHub.
- Launcher: the "Players wants your shaders" pop-up at startup, with **Don't show this message again**.
- Launcher: the Multisampling setting is removed (2x MSAA stays on, as on the Xbox 360). The About page shows the port's version.
- New `tools/collect_shader_shares.py` builds the next shader pack from players' submissions, taking only records that at least two players sent.
- The poster is Alfred Leete's 1914 "Britons wants you" (public domain), re-lettered.

## v1.3.0 (6 October 2026)

### What's new
- **Shader pack: no more pauses for new effects.** The game prepares each effect (a shader) the first time it appears, which caused the short pauses many of you reported on a first play-through. A test build has now played every chapter on its own and collected those effects into a shader pack. On the launcher's **Play** page, click **Download shader pack**: it is added to your shader cache, and every time the game starts it prepares everything in the cache before you play. Anything your own game has already prepared is kept.
  - Pack 1 holds 4,652 prepared effects from all 42 chapters. In a hand-played test of a chapter, it already covered about 97% of what the game needed, and the rest showed up only as a few tiny pauses (under 75 ms, about two frames). Bigger packs will follow, and **Check for a newer pack** on the Play page fetches them without needing a new release of the port.
- **A warning before playing above 30 FPS.** Pressing Play with a frame rate above 30 (or unlimited) now explains that some animations can look wrong, and offers to play at 30 or 60 FPS instead. 30 FPS plays every animation correctly. At 60 the issues are still there but much less noticeable. This will be fixed in a future update.

### Full changelog
- Launcher: **Download shader pack** on the Play page. It downloads from the `shader-packs` release on GitHub and merges into `Documents\king_kong\cache`. The launcher only contacts GitHub when you click.
- Launcher: frame-rate pop-up when pressing Play above 30 FPS, with Play at 30, Play at 60, Keep and Back.
- New `tools/make_shader_pack.py` builds a pack from one or more shader caches.
- Developer test build: the automatic tour that plays every chapter (cheats, chapter select, skip videos, walk around), used to build the pack. It is never in release builds.
- README: a section on the shader pack.

## v1.2.0 (5 October 2026)

### What's new
- **No more vanishing objects, silhouettes or bright flashes the first time you play.** While the game prepared a new effect in the background, it skipped drawing everything that needed it, so characters could turn into silhouettes for a moment and some frames flashed bright. Effects are now prepared before they are drawn. The first time you see something new there can be a short pause instead, and only once, because prepared effects are saved for next time. If you prefer the old behaviour, set **Graphics > Shader preparing** to Background.
- **Steadier frame pacing on more PCs.** The game now asks Windows for precise 1 ms timers and opts out of power throttling. Without this, Windows 10 and 11 can stretch the game's short waits to about 16 ms, and on laptops with Intel's newer mixed P-core/E-core processors Windows 11 can move the game onto the slower cores. Both cause stutter.
- **Better bug reports.** Every 10 seconds the log notes the average frame rate, the 1% low, the worst frame and how many frames took over 50 or 100 ms. If the game ever crashes, it writes `crash-<date>.txt` and `crash-<date>.dmp` into the `logs` folder. The README explains what to send.

### Full changelog
- Shaders now finish preparing before they are used (no skipped draws). The new launcher setting **Graphics > Shader preparing** switches between Wait (default) and Background.
- 1 ms timer resolution and Windows power-throttling opt-out at startup. The log records how long a 1 ms sleep really takes.
- Frame-time summary in the log every 10 seconds.
- Crash reports (`.txt` and minidump) in the `logs` folder.
- New developer test build (`kk-dev` preset, never in releases). It can skip straight into gameplay and walk around on its own, for testing performance.
- README: a new "Reporting a problem" section, and updated known issues.

## v1.1.0 (5 October 2026)

### What's new
- **Button prompts for other controllers.** The game can now show PlayStation 5, PlayStation 2, Xbox Series or keyboard buttons instead of the Xbox 360 ones, in menus and hints. Pick it in the launcher under **Controls > Button prompts**.
  - **Keyboard** shows the keys you have actually bound, so the prompts change when you remap keys.
  - **PlayStation 2** uses the classic coloured triangle, circle, cross and square.
  - The pictures are free CC0 art from Xelu's Free Controllers & Keyboard Prompts, not taken from any game.
- **Steadier launcher.** Fixed a bug where closing the launcher could use its pictures after they were freed. It crashed the game on Linux and was a hidden risk on Windows.

### Full changelog
- Added the Button prompts setting (Xbox 360, Xbox Series, PlayStation 5, PlayStation 2, Keyboard), with a `glyphs` folder of button pictures next to the exe.
- Added `tools/make_glyphs.py`, which rebuilds the `glyphs` folder from the prompt pack.
- Fixed the launcher freeing its pictures while the last frame still used them.
- Linux: work in progress on a Linux version (game setup, file picker, sounds, fonts and two crash fixes). It is not ready yet, so there is no Linux download.
- README: added the new setting and the credit for the button pictures, and marked Linux as in development.

## v1.0.1 (5 October 2026)

### Fixed
- **Render quality presets now change the resolution** ([#1](https://github.com/TekRantGaming/king-kong-recompiled/issues/1)). Every preset (Native, Quality, Supersample and so on) was rendering at the original 720p. Only Custom worked. Presets now render at the resolution shown next to them in the launcher.

## v1.0.0 (5 October 2026)

### What's new
- The game plays past the "VENTURE" opening, which freezes on Xenia, with working music and voice lines
- A launcher that installs the game from your disc, with art from your copy of the game
- Up to 8K resolution, FXAA, 2x MSAA and 16x texture filtering
- 30 fps like the console, or up to 240 and unlimited (some animations are not right above 30 yet)
- Xbox 360 style achievement pop-ups, with your own choice of sound
- Camera inversion and speed, deadzone, vibration, button remapping, keyboard and mouse
