## Peter Jackson's King Kong PC Port v1.4.0

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

### How to update
Download **KingKong-v1.4.0-windows-x64.zip** below and copy everything in it over your KingKong folder. After this, the launcher updates itself.

### New install
1. Unzip **KingKong-v1.4.0-windows-x64.zip** anywhere and run **king_kong.exe**.
2. Click **Install from disc image...** and pick your own King Kong disc image (USA/Europe, title ID `555307D3`, version `0.0.0.1`).
3. Click **Download shader pack**.
4. Press **PLAY**.

**No game files are included.** See the README for details.

Requires Windows 10 or 11 (64-bit) and a DirectX 12 graphics card. Linux version in development.

This port was made with AI (Claude Code). See the README for details.
