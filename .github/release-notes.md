## Peter Jackson's King Kong PC Port v1.8.0

### What's new
- **No more shader pauses.** A new **Balanced** choice for **Shader preparing** on the **Graphics** page, now the default for everyone. New effects are prepared on many threads at once in the background, and each frame waits a moment for them, so the game no longer stops for a second or two when it meets an effect for the first time. In tests of the V-Rex chapter with nothing prepared yet, **Wait** (the old default) paused for up to 2 seconds at a time while playing; **Balanced** didn't pause at all while playing, and its longest wait was under half a second while the chapter loaded. Once in a while an object can appear a moment late the first time it's seen. **Wait** and **Background** are still there if you prefer them.
- **The shader pack looks after itself.** Each time the launcher opens, it downloads the newest shader pack if you don't have it yet. There's no button to click and nothing to remember. The Play page shows how it went, and if you press **PLAY** while a download is still going, the game starts as soon as it finishes.
- **First-run setup.** The first time the launcher opens, a few steps take you through installing the game from your disc image, the shader pack and your main settings (window mode, frame rate, input and startup logos). You can skip it, and everything in it is on the launcher's pages too.
- **Prepared effects are kept after a crash.** If the game closed while saving a newly prepared effect, everything prepared after that point was ignored and had to be prepared again. Now only the damaged entry is dropped, and the rest is kept.
- The "share your shaders" pop-up that appeared when the launcher opened is gone. **Share my shaders** is still on the Play page for anyone who wants to help with future packs.

### Full changelog
- Shader preparing: `async_shader_compilation` is now on by default with a new `async_shader_wait_ms`. A draw whose effect is still being prepared waits for it within a budget for each frame of half a frame at your frame rate cap (16 ms at 30 FPS, 8 at 60, 4 from 120 or unlimited), and is skipped only once that is used up. **Background** sets the wait to 0, and **Wait** turns preparing in the background off.
- GPU plugin: `rexgpu-xenos.dll` is now built from the ReXGlue SDK 0.10.0 source with the port's patch for that wait. The patch and build steps are in `tools/rexglue-patches`.
- Shader cache: at startup the port checks the cache's records and drops any that were only partly written, instead of the runtime ignoring everything after the first one.
- Shader pack: downloaded automatically each time the launcher opens, whether or not update checks are on. The **Shader pack update** pop-up and the **Download shader pack** button are gone. Shader pack 2 is still the latest; it covers about 40% of the game's effects, and Balanced handles the rest smoothly.
- Launcher: first-run setup (`kk_setup_done`). It shows on a fresh install until it is finished or skipped, and not when updating from an earlier version. The share-shaders poster (`kk_share_poster`) is removed.
- `tools/shader_coverage.py`: measures how many of the game's own effects (listed in its `Shaders/xeshaders.bin`) a shader cache has prepared.

### How to update
If you have v1.4.0 or later, the launcher offers this update when it opens: click **Update now**. Otherwise download **KingKong-v1.8.0-windows-x64.zip** below and copy everything in it over your KingKong folder. Your settings move to **Balanced** shader preparing on their own.

### New install
1. Unzip **KingKong-v1.8.0-windows-x64.zip** anywhere and run **king_kong.exe**. A short setup opens.
2. Click **Install from disc image...** and pick your own King Kong disc image (USA/Europe, title ID `555307D3`, version `0.0.0.1`). The shader pack downloads by itself meanwhile.
3. Pick your main settings, then press **Play**.

**No game files are included.** See the README for details.

Requires Windows 10 or 11 (64-bit) and a DirectX 12 graphics card. Linux version in development.

This port was made with AI (Claude Code). See the README for details.
