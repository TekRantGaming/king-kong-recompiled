## Peter Jackson's King Kong PC Port v1.9.2

### What's new
- **Fixed: Necropolis and Brontosaurus looking low resolution** (#32). Those levels turn on one of the game's own effects, a light blur over the whole picture (any other level that uses it is fixed too). The blur is sized for the Xbox 360's 720p, so whatever resolution you played at, those levels came out about as sharp as 720p. It's a new **Screen blur** setting on the **Graphics** page, off by default, so those levels are as sharp as the rest of the game. The **Original Xbox 360** look keeps it on. Thanks to GrummelFritz and leocmp for sticking with this one and sending logs and screenshots.

### Full changelog
- Screen blur: `kk_big_blur` (off by default). The game's "BigBlur" after effect averages four slightly offset copies of the frame, with offsets for 1280 x 720; off skips it. Found by turning the game's after effects off one at a time in Necropolis and identifying the draw it adds by its shader key.

If Necropolis, Brontosaurus or another level looked low resolution for you, please update and let us know in [#32](https://github.com/TekRantGaming/king-kong-recompiled/issues/32) whether it's sharp now.

### How to update
If you have v1.4.0 or later, the launcher offers this update when it opens: click **Update now**. Otherwise download **KingKong-v1.9.2-windows-x64.zip** below and copy everything in it over your KingKong folder. Your settings stay as they are, and **Screen blur** starts off.

### New install
1. Unzip **KingKong-v1.9.2-windows-x64.zip** anywhere and run **king_kong.exe**. The shader pack downloads by itself.
2. Click **Install from disc image...** and pick your own King Kong disc image (USA/Europe, title ID `555307D3`, version `0.0.0.1`).
3. Press **PLAY**.

**No game files are included.** See the README for details.

Requires Windows 10 or 11 (64-bit) and a DirectX 12 graphics card. Linux version in development.

This port was made with AI (Claude Code). See the README for details.
