## Peter Jackson's King Kong PC Port v1.9.1

### What's new
- **Fixed: the whole game drawing at low resolution for some players** (#32). The game picks its picture size from the TV it thinks it's connected to, and the port was giving it your window size. Windows display scaling shrinks the size the launcher saves (a 1280 x 720 window at 125% is kept as 1024 x 576), and below 720 lines the game falls back to standard definition: 640 x 480, stretched to widescreen, even in fullscreen and whatever your **Render quality**. It now always sees a 720p TV, as on an Xbox 360, and **Render quality** scales that up as intended. Thanks to GrummelFritz and leocmp for the reports and the log.

### Full changelog
- Video mode: the port sets `video_mode_width` and `video_mode_height` to 1280 x 720 each time it starts (they aren't saved to `king_kong.toml`). Before, the runtime reported `window_width` and `window_height` as the console's video mode whenever a window size was set. The log now notes the video mode at startup (`KK: video mode 1280x720 (window ...)`).

If the game looked low resolution for you, in Necropolis, Brontosaurus or anywhere else, please update and let us know in [#32](https://github.com/TekRantGaming/king-kong-recompiled/issues/32) whether it's sharp now.

### How to update
If you have v1.4.0 or later, the launcher offers this update when it opens: click **Update now**. Otherwise download **KingKong-v1.9.1-windows-x64.zip** below and copy everything in it over your KingKong folder. Your settings stay as they are.

### New install
1. Unzip **KingKong-v1.9.1-windows-x64.zip** anywhere and run **king_kong.exe**. The shader pack downloads by itself.
2. Click **Install from disc image...** and pick your own King Kong disc image (USA/Europe, title ID `555307D3`, version `0.0.0.1`).
3. Press **PLAY**.

**No game files are included.** See the README for details.

Requires Windows 10 or 11 (64-bit) and a DirectX 12 graphics card. Linux version in development.

This port was made with AI (Claude Code). See the README for details.
