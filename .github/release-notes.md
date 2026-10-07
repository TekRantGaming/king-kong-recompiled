## Peter Jackson's King Kong PC Port v1.5.1

### What's new
- **No more antivirus warnings.** Some antivirus programs, including Windows Defender, wrongly flagged a file that comes with the port (`rexruntime.dll`, part of the ReXGlue toolkit the port is built with) as a virus. It was a false alarm, but it could block the download. This release ships that file rebuilt from the exact same ReXGlue source, and antivirus programs no longer flag it.
- **Saving always works.** Some players were told to sign in before they could save, and the game never offered their save slots. This happened when the PC saw more than one controller, which is common with Steam Input, DS4Windows, or a handheld's built-in controls. The game then treated the player as player 2, and only player 1 can save. Every controller, plus keyboard and mouse, now counts as player 1, so saving works whichever one you use (#9).
- **Freeze reports.** If the game ever stops responding for 20 seconds, the port now writes a freeze report to the `logs` folder. If the game recovers, the log notes how long it froze. Attaching those files to a bug report lets us find what caused the freeze (#11).

### Full changelog
- Ships `rexruntime.dll` rebuilt from the same ReXGlue v0.10.0 source, unchanged. The official build was an antivirus false positive (rexglue/rexglue-sdk#485). `rexgpu-xenos.dll` is the official one, which was never flagged.
- Controllers: all controllers and keyboard & mouse now count as player 1. Before, each controller got its own player number in connection order, and the game refuses to save for players 2 to 4 because they have no profile.
- Freeze reports: a watchdog writes `logs\hang-<date-time>.txt` (where every part of the game was at that moment) and `logs\hang-<date-time>.dmp` when no frame has been drawn for 20 seconds. Time the PC spends asleep isn't counted.
- Crash and freeze reports can now be traced to the exact part of the game's code, so problems players report are quicker to fix.

### How to update
If you have v1.4.0 or later, the launcher offers this update when it opens: click **Update now**. Otherwise download **KingKong-v1.5.1-windows-x64.zip** below and copy everything in it over your KingKong folder.

### New install
1. Unzip **KingKong-v1.5.1-windows-x64.zip** anywhere and run **king_kong.exe**.
2. Click **Install from disc image...** and pick your own King Kong disc image (USA/Europe, title ID `555307D3`, version `0.0.0.1`).
3. Click **Download shader pack**.
4. Press **PLAY**.

**No game files are included.** See the README for details.

Requires Windows 10 or 11 (64-bit) and a DirectX 12 graphics card. Linux version in development.

This port was made with AI (Claude Code). See the README for details.
