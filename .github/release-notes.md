## Peter Jackson's King Kong PC Port v1.3.0

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

### How to update
Download **KingKong-v1.3.0-windows-x64.zip** below and copy everything in it over your KingKong folder. Then open the launcher and click **Download shader pack**. Your installed `game` folder, saves and settings stay as they are.

### New install
1. Unzip **KingKong-v1.3.0-windows-x64.zip** anywhere and run **king_kong.exe**.
2. Click **Install from disc image...** and pick your own King Kong disc image (USA/Europe, title ID `555307D3`, version `0.0.0.1`).
3. Click **Download shader pack**.
4. Press **PLAY**.

**No game files are included.** See the README for details.

Requires Windows 10 or 11 (64-bit) and a DirectX 12 graphics card. Linux version in development.

This port was made with AI (Claude Code). See the README for details.
