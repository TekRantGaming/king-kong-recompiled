## Peter Jackson's King Kong PC Port v1.7.0

### What's new
- **Field of view slider.** A new **Field of view** setting on the launcher's **Gameplay** page widens the view from 69° (the original) up to 110°. The game draws the wider area too, so nothing pops in at the edges. Jack's gun keeps its usual size, the menus keep their original look, and cutscenes widen by the same amount (#3).
- **Controller sensitivity works up and down too.** The **Controller sensitivity** slider on the **Controls** page (it was called Camera speed) now changes how fast you look up and down, not just left and right. A full push of the stick still turns at the game's own top speed. **Mouse sensitivity** and **Mouse camera** are now always shown on the Controls page.
- **Motion blur on or off.** A new **Motion blur** setting on the **Graphics** page. Off removes the ghost trail the game blends over fast moments, mostly in Kong's sequences and some transitions.
- **Skip the startup logos.** A new **Startup logos** setting on the **Gameplay** page skips the Ubisoft, Universal and WingNut movies, so the title screen appears within a few seconds. The story movies still play.
- **Reset a page.** Every settings page has a **Reset page** button that puts just that page's settings back to their defaults.
- **What's new and the changelog.** After an update, the launcher shows what changed the first time it opens. The **About** page now lists the notes for every version.

### Full changelog
- Field of view: `kk_fov`, in degrees for Jack's camera (69 is the original). Every camera is widened by the same amount, so close-ups stay closer than wide shots. The area the game draws widens with it. Jack's gun is drawn with its own fixed angle and doesn't change. Nothing is widened while the menus show.
- Controller sensitivity: `kk_camera_sensitivity` now scales the right stick as the game reads it, instead of the raw stick, so both directions change together and it is no longer limited below a full push. It no longer applies to keyboard and mouse, which has its own **Mouse sensitivity**.
- Motion blur: `kk_motion_blur`, on by default as on the console. Off runs the game's motion blur effect at zero strength, the game's own way of having no blur.
- Startup logos: `kk_skip_intros`, off by default.
- Reset page: the Display, Graphics, Gameplay, Controls (including button remapping and keyboard keys), Cheats and Achievements pages each reset only their own settings. Press **Save** to keep them.
- Changelog: `CHANGELOG.md` is built into the launcher, and the launcher remembers the last version it ran (`kk_last_version`) to know what's new.

### How to update
If you have v1.4.0 or later, the launcher offers this update when it opens: click **Update now**. Otherwise download **KingKong-v1.7.0-windows-x64.zip** below and copy everything in it over your KingKong folder. Shader pack 2 is still the latest, so there's no new pack to download.

### New install
1. Unzip **KingKong-v1.7.0-windows-x64.zip** anywhere and run **king_kong.exe**.
2. Click **Install from disc image...** and pick your own King Kong disc image (USA/Europe, title ID `555307D3`, version `0.0.0.1`).
3. Click **Download shader pack**.
4. Press **PLAY**.

**No game files are included.** See the README for details.

Requires Windows 10 or 11 (64-bit) and a DirectX 12 graphics card. Linux version in development.

This port was made with AI (Claude Code). See the README for details.
