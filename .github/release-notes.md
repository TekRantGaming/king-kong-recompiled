## Peter Jackson's King Kong PC Port v1.6.0

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

### How to update
If you have v1.4.0 or later, the launcher offers this update when it opens: click **Update now**. After it restarts, it offers shader pack 2 if you have pack 1. Otherwise download **KingKong-v1.6.0-windows-x64.zip** below and copy everything in it over your KingKong folder.

### New install
1. Unzip **KingKong-v1.6.0-windows-x64.zip** anywhere and run **king_kong.exe**.
2. Click **Install from disc image...** and pick your own King Kong disc image (USA/Europe, title ID `555307D3`, version `0.0.0.1`).
3. Click **Download shader pack**.
4. Press **PLAY**.

**No game files are included.** See the README for details.

Requires Windows 10 or 11 (64-bit) and a DirectX 12 graphics card. Linux version in development.

This port was made with AI (Claude Code). See the README for details.
