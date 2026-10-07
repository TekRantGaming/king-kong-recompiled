## Peter Jackson's King Kong PC Port v1.7.3

### What's new
- **Controller sensitivity speeds up the camera itself.** Before, the slider could only make smaller pushes of the stick reach the game's top turning speed: a full push turned just as fast whatever you chose. Now 200% turns twice as fast at every push, a full push included, both looking around and while aiming (#19).
- **Modern camera response, on by default.** A new **Camera response** setting on the **Controls** page. **Modern** turns the camera the same way in every direction, so circles and diagonals feel even. **Original** is the Xbox 360's own response, where small pushes up and down turn much more slowly than small pushes sideways. A full push straight across or straight up turns at the game's own top speed either way.
- **90 FPS.** The frame rate choices now include 90 FPS. They're a dropdown now, so every choice fits even in a small launcher window. As with any rate above 30, the launcher explains the animation issue when you press Play.

### Full changelog
- Controller sensitivity: scales the camera's own turn each frame (the turn the game's camera code applies left and right and up and down) instead of the stick, which the game caps at a full push. Keyboard and mouse still uses **Mouse sensitivity**.
- Camera response: `kk_camera_modern`, on by default. The game reads the right stick one direction at a time, with a 15% deadzone on each, then turns by the square of the push left and right and the cube of the push up and down. Modern reshapes the stick the camera reads so both follow the square of how far you push, in the direction you push. A half push now gives a quarter of the top speed in any direction; before, it was 17% left and right and 7% up and down.
- Frame rate: 90 added to `kk_frame_rate`'s choices, and the setting is a dropdown.

### How to update
If you have v1.4.0 or later, the launcher offers this update when it opens: click **Update now**. Otherwise download **KingKong-v1.7.3-windows-x64.zip** below and copy everything in it over your KingKong folder. Shader pack 2 is still the latest.

### New install
1. Unzip **KingKong-v1.7.3-windows-x64.zip** anywhere and run **king_kong.exe**.
2. Click **Install from disc image...** and pick your own King Kong disc image (USA/Europe, title ID `555307D3`, version `0.0.0.1`).
3. Click **Download shader pack**.
4. Press **PLAY**.

**No game files are included.** See the README for details.

Requires Windows 10 or 11 (64-bit) and a DirectX 12 graphics card. Linux version in development.

This port was made with AI (Claude Code). See the README for details.
