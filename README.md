# Peter Jackson's King Kong Recompiled

A native PC port of the Xbox 360 version of **Peter Jackson's King Kong: The Official Game of the Movie** (Ubisoft, 2005), made with static recompilation using [ReXGlue](https://github.com/rexglue/rexglue-sdk).

> **Work in progress.** The game boots, saves, and plays past the opening on the ship into real gameplay. Full play-through testing, screenshots and the first release are still to come.

No game files are included. You need your own copy of the Xbox 360 disc as a disc image (.iso).

## Features

- **Modern launcher** with Skull Island artwork, opened before the game starts (hold Shift to open it at any time)
- **Install from your own disc image** right from the launcher
- **PC settings**: resolution, window or fullscreen, anti-aliasing (FXAA / MSAA), frame-rate limit
- **Controls**: remap any button, play with keyboard and mouse, invert camera on either axis
- **Achievements**: Xbox 360 style pop-ups with sound, an achievements page with the game's 9 achievements, a test button and your choice of pop-up sound
- **Gamertag**: set your own name

## Building

Windows 10 or 11, about 15 GB of free space.

1. Download this repo.
2. Double-click **Build King Kong.bat** and pick your disc image.

The builder installs the tools it needs, translates the game code on your PC, compiles it and puts the finished game in the `KingKong` folder.

Linux (AppImage): coming soon.

## Status

| Area | State |
| --- | --- |
| Boot, intro videos, menus | Working |
| Saving and loading | Working |
| Music and voice lines | Working (fixed, see below) |
| Opening on the ship | Working (fixed, see below) |
| Frame rate | 60 FPS and above |
| Full play-through | Not tested yet |
| Release | Not yet |

### The "VENTURE" freeze

On Xenia this game gets stuck at the start, looking at the ship's name "VENTURE" ([xenia-project/game-compatibility#1302](https://github.com/xenia-project/game-compatibility/issues/1302)). The cause is how disc reads finish. The game streams music and voice lines with reads it expects to finish right away. The emulator finishes them but reports them as still in progress, so the game thinks it got no data. No voice line ever plays, and the opening waits forever for one to end.

This port finishes those reads the way the console does (`kk/src/io_fix.cpp`), so music, voice lines and the opening all work.

## Legal

This project contains no game code or assets from King Kong. You must own the game. King Kong is a trademark of Universal Studios. This project is not affiliated with Ubisoft, Universal or Microsoft.

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)
- Port by [TekRantGaming](https://github.com/TekRantGaming)
