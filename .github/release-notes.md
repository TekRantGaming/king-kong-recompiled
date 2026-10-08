## Peter Jackson's King Kong PC Port v1.8.0

### What's new
- **Shader freezes cut from 2 seconds to under half a second.** A new **Balanced** choice for **Shader preparing** on the **Graphics** page, now the default for everyone. New effects are prepared on many threads at once in the background, and each frame waits a moment for them, instead of the game stopping while they're prepared one at a time. Once in a while an object can appear a moment late the first time it's seen. **Wait** and **Background** are still there if you prefer them. The numbers are below.
- **The shader pack looks after itself.** Each time the launcher opens, it downloads the newest shader pack if you don't have it yet. There's no button to click and nothing to remember. The Play page shows how it went, and if you press **PLAY** while a download is still going, the game starts as soon as it finishes.
- **Prepared effects are kept after a crash.** If the game closed while saving a newly prepared effect, everything prepared after that point was ignored and had to be prepared again. Now only the damaged entry is dropped, and the rest is kept.
- **New defaults.** FXAA is on, motion blur is off, **Controller sensitivity** is 150% and the **Stick deadzone** is 5%. The achievement sound is the Xbox 360's when you have `Xbox_360.wav` in the `sounds` folder; otherwise the built-in chime plays as before. Settings you had changed from the old defaults stay as you set them.
- The "share your shaders" pop-up and the **Share my shaders** button are gone.

### Performance
Tested on the V-Rex chapter at 30 FPS on its first visit, with the graphics driver's own shader cache bypassed so every effect really was new.

| | Wait (old default) | Balanced (new default) |
| --- | :---: | :---: |
| Longest freeze | 2.08 s | **0.45 s** |
| All the freezes added up | 3.9 s | **1.3 s** |
| Longest freeze with shader pack 2 | 0.33 s | **0.16 s** |

The freezes came as the chapter started, when the game meets the most new effects at once: up to about 240 in a second, each taking the graphics driver about 20 ms to prepare. In the minute of play after that, neither mode froze. Without a shader pack, Balanced drew about 80 objects a frame late in that minute; with shader pack 2, none.

### Full changelog
- Shader preparing: `async_shader_compilation` is now on by default with a new `async_shader_wait_ms`. A draw whose effect is still being prepared waits for it within a budget for each frame of half a frame at your frame rate cap (16 ms at 30 FPS, 8 at 60, 4 from 120 or unlimited), and is skipped only once that is used up. **Background** sets the wait to 0, and **Wait** turns preparing in the background off.
- GPU plugin: `rexgpu-xenos.dll` is now built from the ReXGlue SDK 0.10.0 source with the port's patch for that wait. The patch and build steps are in `tools/rexglue-patches`.
- Shader cache: at startup the port checks the cache's records and drops any that were only partly written, instead of the runtime ignoring everything after the first one.
- Shader pack: downloaded automatically each time the launcher opens, whether or not update checks are on. The **Shader pack update** pop-up and the **Download shader pack** button are gone. Shader pack 2 is still the latest; it covers about 40% of the game's effects, and Balanced handles the rest smoothly.
- Defaults: `swap_post_effect` fxaa, `kk_motion_blur` false, `kk_camera_sensitivity` 150, `kk_deadzone` 5, `kk_achievement_sound_file` `Xbox_360.wav` (the built-in chime if that file isn't in the `sounds` folder). VSync off (`d3d12_allow_variable_refresh_rate_and_tearing`), 4x texture filtering (`anisotropic_override` 3) and the Modern camera response were already the defaults; the port now sets the first two itself.
- Launcher: the share-shaders poster (`kk_share_poster`) and the **Share my shaders** button are removed.
- `tools/shader_coverage.py`: measures how many of the game's own effects (listed in its `Shaders/xeshaders.bin`) a shader cache has prepared.

### How to update
If you have v1.4.0 or later, the launcher offers this update when it opens: click **Update now**. Otherwise download **KingKong-v1.8.0-windows-x64.zip** below and copy everything in it over your KingKong folder. Your settings move to **Balanced** shader preparing on their own.

### New install
1. Unzip **KingKong-v1.8.0-windows-x64.zip** anywhere and run **king_kong.exe**. The shader pack downloads by itself.
2. Click **Install from disc image...** and pick your own King Kong disc image (USA/Europe, title ID `555307D3`, version `0.0.0.1`).
3. Press **PLAY**.

**No game files are included.** See the README for details.

Requires Windows 10 or 11 (64-bit) and a DirectX 12 graphics card. Linux version in development.

This port was made with AI (Claude Code). See the README for details.
