## Peter Jackson's King Kong PC Port v1.1.0

Play the Xbox 360 version of Peter Jackson's King Kong natively on Windows, with a launcher and modern PC options.

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

### How to update
Download **KingKong-v1.1.0-windows-x64.zip** below and copy everything in it over your KingKong folder, including the new `glyphs` folder. Your installed `game` folder, saves and settings stay as they are.

### New install
1. Unzip **KingKong-v1.1.0-windows-x64.zip** anywhere and run **king_kong.exe**.
2. Click **Install from disc image...** and pick your own King Kong disc image (USA/Europe, title ID `555307D3`, version `0.0.0.1`).
3. Press **PLAY**.

**No game files are included.** See the README for details.

Requires Windows 10 or 11 (64-bit) and a DirectX 12 graphics card. Linux version in development.

This port was made with AI (Claude Code). See the README for details.
