## Peter Jackson's King Kong PC Port v1.2.0: stability pass

This release is about making the game run properly on more PCs. Thanks to everyone on Reddit who reported problems.

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

### Tested
On an RTX 4070 Ti, gameplay held a locked 60 FPS at 720p, 1440p and 2160p, including with the game limited to 2 CPU threads and with an empty shader cache. Short pauses remain when the game loads a new area (as on the Xbox 360). The pre-rendered videos also pause briefly every couple of seconds; that is the video player keeping to the video's timing and is being looked into.

### How to update
Download **KingKong-v1.2.0-windows-x64.zip** below and copy everything in it over your KingKong folder. Your installed `game` folder, saves and settings stay as they are.

### New install
1. Unzip **KingKong-v1.2.0-windows-x64.zip** anywhere and run **king_kong.exe**.
2. Click **Install from disc image...** and pick your own King Kong disc image (USA/Europe, title ID `555307D3`, version `0.0.0.1`).
3. Press **PLAY**.

**No game files are included.** See the README for details.

Requires Windows 10 or 11 (64-bit) and a DirectX 12 graphics card. Linux version in development.

This port was made with AI (Claude Code). See the README for details.
