<div align="center">

<img src="docs/images/v2/mark.svg" alt="" width="96">

# King Kong PC Port

### Version 2.0.0: King Kong Remastered

Peter Jackson's King Kong, the Xbox 360 edition, as a native PC game for Windows, Linux and Steam Deck.<br>
Now with online co-op, our own renderer with modern lighting, and a launcher made for the couch.

[<img src="https://img.shields.io/badge/Download-Windows-e8a33c?style=for-the-badge&logo=windows&logoColor=white&labelColor=0d0b09" alt="Download for Windows" height="44">](https://github.com/TekRantGaming/king-kong-recompiled/releases/latest)
&nbsp;
[<img src="https://img.shields.io/badge/Download-Linux-e8a33c?style=for-the-badge&logo=linux&logoColor=white&labelColor=0d0b09" alt="Download for Linux" height="44">](https://github.com/TekRantGaming/king-kong-recompiled/releases/latest)
&nbsp;
[<img src="https://img.shields.io/badge/Watch-the%20trailer-c0392b?style=for-the-badge&logo=youtube&logoColor=white&labelColor=0d0b09" alt="Watch the trailer" height="44">](https://www.youtube.com/watch?v=P5wmya5mT7E)
&nbsp;
[<img src="https://img.shields.io/badge/Discord-Join%20the%20community-5865F2?style=for-the-badge&logo=discord&logoColor=white&labelColor=0d0b09" alt="Join the Discord" height="44">](https://discord.gg/8eakYSvBZS)

[![Latest release](https://img.shields.io/github/v/release/TekRantGaming/king-kong-recompiled?style=flat-square&label=release&color=e8a33c&labelColor=0d0b09)](https://github.com/TekRantGaming/king-kong-recompiled/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/TekRantGaming/king-kong-recompiled/total?style=flat-square&color=8a5a1c&labelColor=0d0b09)](https://github.com/TekRantGaming/king-kong-recompiled/releases)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20Steam%20Deck-5b7a8c?style=flat-square&labelColor=0d0b09)
[![Website](https://img.shields.io/badge/website-kong.tekrantgaming.com-5b7a8c?style=flat-square&labelColor=0d0b09)](https://kong.tekrantgaming.com)
[![Listed on Quiver Launcher](https://img.shields.io/badge/listed%20on-Quiver%20Launcher-5b7a8c?style=flat-square&labelColor=0d0b09)](https://quiverlauncher.com/apps/peter-jackson-s-king-kong-the-official-game-of-the-movie-king-kong-pc-port)

<br>

<img src="docs/images/v2/coop.jpg" alt="The launcher's Play Online Co-op page" width="100%">

<sub>The original Xbox 360 game code, translated to native PC code with the <a href="https://github.com/rexglue/rexglue-sdk">ReXGlue SDK</a>. <b>No game files included</b>: bring your own copy of the King Kong Xbox 360 disc.</sub>

</div>

<br>

## What's in 2.0.0

| | | |
| :--- | :--- | :--- |
| 👥 **Online co-op** for two players (early access) | 🎨 **Our own renderer**, DirectX 12 or Vulkan, up to 8K | ☀️ **Global illumination**, ambient occlusion, sharper shadows, water reflections, HDR |
| 🔍 **DLSS, FSR 3.1, FSR 4 and XeSS**, plus FSR 1 and NIS | 🎞️ **Any frame rate**, with the game's motion right at each one | 🖥️ **Ultrawide built in**, 21:9 and 32:9 with nothing stretched |
| 🎮 **The launcher is the main menu**, with all 42 chapters open | 🧭 **Auto detect** sets the game up for your PC | ⏸️ **A new pause menu**, with settings and photo mode |
| 🧩 **The Workshop**: texture packs and mods | 🎬 **Restored PS2 cutscenes**, the Weta Artbook and the manual | 📊 **Benchmark** on the main menu |
| 🚀 **16,842 graphics pipelines** prepared before you play | 🔄 **Updates in one click**, put back if anything goes wrong | 🐧 **Native Linux AppImage**, ready for Steam Deck |

Full notes are in the [changelog](CHANGELOG.md) and on the [releases page](https://github.com/TekRantGaming/king-kong-recompiled/releases). The [website](https://kong.tekrantgaming.com) has a page for every feature.

## Getting started

**You need:** Windows 10 or 11 (64 bit) or Linux (x86 64 bit), a graphics card that supports DirectX 12 or Vulkan, about 7 GB of free space, and a disc image made from your own Peter Jackson's King Kong Xbox 360 disc (see [Which version you need](#which-version-you-need)).

### Windows

1. Download **KingKong-...-windows-x64.zip** from the [latest release](https://github.com/TekRantGaming/king-kong-recompiled/releases/latest) and unzip it anywhere.
2. Run **king_kong.exe**. The launcher opens on its first setup.
3. Pick your disc image. The launcher checks it, copies the game files (about 6.3 GB) into a `game` folder next to the exe and takes its artwork from them.
4. The graphics are prepared once, with one progress bar. Then setup offers the HD texture pack and the PS2 cutscenes, and walks you through the graphics options and button prompts.
5. Press **Play**.

The download contains only this port. **No game files are included**: they come from your own disc. Your saves and settings stay in `Documents\king_kong`.

### Linux and Steam Deck

The Linux version is a native program, not the Windows version under a translation layer. It's one AppImage file with the Vulkan renderer. The name never changes, so a Steam shortcut to it keeps working from one version to the next.

1. Download **KingKong-linux-x86_64.AppImage** from the [latest release](https://github.com/TekRantGaming/king-kong-recompiled/releases/latest) and put it in a folder of its own, for example in your home folder.
2. In your file manager, open its Properties and allow it to run as a program, or run `chmod +x` on it in a terminal.
3. Start it. The launcher opens on its first setup: pick your disc image and set up as on Windows.
4. To play from Steam's Game Mode, use **Add to Steam** on the launcher's Home screen, which also adds the artwork. You can also choose **Games > Add a Non Steam Game to My Library** in Steam's desktop mode.

The game files, `king_kong.toml` and the `logs` folder sit next to the AppImage, and the launcher's About tab opens your save folder in one click. It needs FUSE 2 to mount the AppImage, and `curl` for downloads.

**What differs from Windows:** DirectX 12 is a Windows API, so Linux always uses Vulkan. DLSS and FSR 3.1 are Windows only for now, FSR 4 needs DirectX 12, and Intel has no Linux version of XeSS, so on Linux the upscalers are FSR 1 and NIS. Linux support is newer than Windows support, so please [tell us](#reporting-a-problem) if something doesn't work.

A Mac version is planned, and it may also lead to Arm builds. Until then, Windows on Arm PCs can try the Windows version through Prism.

### Which version you need

The port is made for this disc only. The 2005 PC version and discs for other platforms will not work.

| | |
| --- | --- |
| Game | Peter Jackson's King Kong: The Official Game of the Movie |
| Disc | Xbox 360, USA and Europe (English, French, German, Spanish, Italian, Dutch, Swedish, Norwegian, Danish, Finnish) |
| Title ID | `555307D3` |
| Game version | `0.0.0.1` (the original disc release) |
| Disc image | `.iso`, 7,834,892,288 bytes |
| SHA-1 | `075F43709E9C9A095E099AB5056CC04B768288D1` |
| MD5 | `35D671ED4E9EAD9E6B288A3441B6C8F9` |

The launcher checks the title ID before it installs. To check your disc image yourself, run this in PowerShell and compare the result with the SHA-1 above:

```powershell
Get-FileHash "C:\Games\King Kong.iso" -Algorithm SHA1
```

The [game files page](https://kong.tekrantgaming.com/game-files.html) explains how to make an image of your own disc. We can't provide, link to or help you find game files.

## Online co-op (early access)

Two players, one story. The host picks a chapter or one of their saves, a friend joins, and both games load it together.

<img src="docs/images/v2/lobby.jpg" alt="The co-op lobby" width="100%">

1. Open **Play Online Co-op**, directly under **Play** on the Home screen.
2. **Host a game** and choose any chapter or one of your saves.
3. Invite a Steam friend to the lobby, or share your 6 character code.
4. Both players press **Ready**, and both games load the chapter together.

**Joining:** accept an invite, pick a friend who is playing now from the list, or type a code. Invites and the friends list need Steam to be open, and joining with a code works without it. Windows and Linux players can play together.

**Playing together:** the host's game runs the story and the enemies, and your shots count in it. You see your partner in the world with their name over their head. Fall, and you come back next to your partner. The guest's own saves are kept apart from the host's. There's no voice chat in the game, so use Steam or Discord.

**You need:** the same version of the port on both PCs, and each player needs their own copy of the game.

> [!WARNING]
> **Online co-op is in early access.** It works, and it's still rough in places. Not every chapter has a player model for the guest yet, the guest's game sometimes reloads to catch up with the host, people and mission state can drift out of step between the two games, and now and then a player is moved out of the map. Some chapters may not let you go on. For the smoothest game, play single player. If something goes wrong, the launcher's co-op pages have an **Open logs folder** button: please send the logs to us on the [Discord](https://discord.gg/8eakYSvBZS).

## Graphics

The game is drawn by a renderer made for it, with DirectX 12 (the default on Windows) or Vulkan, at any resolution up to 7680 x 4320. If the one you chose doesn't start, the game uses the other and tells you. Vulkan is the default on Intel integrated graphics and the only choice on Linux.

<img src="docs/images/v2/graphics.jpg" alt="The Graphics settings, with the explanation panel" width="100%">

- **Original or Remastered**: Original is the game as the Xbox 360 showed it, at 720p and 30 FPS with the console's antialiasing, its 69° field of view, motion blur and fog. Remastered opens up every graphics setting and plays at 60 FPS.
- **Presets** from Low to Ultra, plus one for the Steam Deck. **Use recommended for my PC** picks one for you.
- **Global illumination**: light bounces, so sunlit rock tints the shadow beside it and the green of the jungle falls on the people in it. Low, Medium or High.
- **Ambient occlusion**: soft shading in corners, creases and under rocks, grass and people, at Light, Medium or Strong.
- **Shadows**: High draws them at twice the detail with soft edges, and Ultra softens them the farther they fall. Off by default.
- **Water reflections**: rocks, trees and the ship mirror in the sea, rivers and swamps. Off by default.
- **Light shafts**: soft shafts of sunlight through the fog. Off by default.
- **HDR**: follows Windows by default, with On and Off in Settings. Peak brightness comes from the Windows HDR Calibration app.
- **Ultrawide**: on a 21:9 or 32:9 screen the game fills the whole width and shows more on the left and right, with nothing stretched. The HUD, subtitles and menus stay in the middle.
- **Brightness and gamma**, **texture filtering** up to 16x, and **motion blur**, **screen blur** and **distance fog**, each on or off.

### Upscalers

The game can draw fewer pixels and rebuild a sharp picture from several frames. Only the upscalers your graphics card can run are listed, and the best one is chosen for you, at Quality. There's no frame generation: every frame you see is drawn by the game.

| Upscaler | Runs on | Windows | Linux |
| --- | --- | :---: | :---: |
| NVIDIA DLSS | GeForce RTX cards | ✅ | |
| AMD FSR 4 | Radeon RX 9000 cards, with DirectX 12 | ✅ | |
| AMD FSR 3.1 | Almost any card | ✅ | |
| Intel XeSS | Best on Intel Arc, runs on other cards too | ✅ | |
| AMD FSR 1 and NVIDIA NIS | Any card, light scaling and sharpening | ✅ | ✅ |

**Upscaler mode** picks how many pixels the game draws: Native AA (DLAA with DLSS), Quality, Balanced, Performance and Ultra Performance.

## Performance

- **Any frame rate**: 30 like the console, 60, 120, 144, 240 or unlimited. Animations, physics, particles, water, the camera and timing run at the right speed, from the rowing crew to Kong's grabs.
- **Ready before you play**: the graphics are prepared once on the first start. The port ships with a list of 16,842 graphics pipelines, built with the help of players who chose to share theirs, so new places stutter far less.
- **Benchmark** on the main menu: a recorded route through four chapters with your graphics settings. A standard run takes about 10 minutes, and the results stay on your PC.
- **Performance metrics** with <kbd>F2</kbd>: FPS only, or Full, with a frame time graph, the graphics card's load, temperature, clock and memory, the processor and memory use, the graphics API and the upscaler.

## The launcher

The launcher is the game's main menu. It works with a controller, the keyboard or the mouse, speaks English, German, Spanish, French and Italian, and looks and sounds like the game, using your own copy of it.

- **Play** opens Continue, New game, Load game, Chapter select and Extras. The game starts right there, with no logos or title screen.
- **Chapter select** has all 42 chapters open, with your best scores.
- **Load game** lists your five saves. Your saves are copied to a backup folder before you play, and before one is deleted or overwritten.
- **First setup** with Auto detect: side by side pictures made from your own copy of the game, your button prompts, then Add to Steam and the Discord.
- **Settings** has eight tabs: Display, Graphics, Audio, Controls, Gameplay, Cheats, Advanced and About. The panel beside them explains each setting: what it does, what it costs and its default.
- **Add to Steam** puts the game in your Steam library with its artwork.
- **Discord status** shows on your profile that you are playing. It only talks to the Discord app on your PC.
- **Achievements** with popups in the Xbox 360's style or Steam's, and the sound of your choice.

<img src="docs/images/v2/display.jpg" alt="The Display settings" width="49%">
<img src="docs/images/v2/controls.jpg" alt="The Controls settings" width="49%">

<img src="docs/images/v2/setup.jpg" alt="The first setup" width="49%">

## In the game

**Pause menu:** press Start or Esc for Resume, Photo mode, Controls, Gameplay, Settings, Load last checkpoint, Save game where the game offers it, Quit to main menu and Quit game. Settings that can change while you play do so at once.

**Photo mode:** fly a free camera around the paused scene, with looks and tints, depth of field, vignette, film grain, letterbox bars, cel shading and more. Pictures save at full resolution with no menus.

**Saving:** the game saves when you choose Save game, and when you quit from the pause menu with Quit to main menu or Quit game. Checkpoints are not saves, so if you close the window another way, for example with Alt+F4, progress since your last save is lost.

## Workshop and Extras

**The Workshop** on the main menu has texture packs and mods, downloaded and switched on from the launcher:

- **HD texture pack** (2.6 GB, offered at setup): textures at four times their original size, keeping the game's own look.
- **Kong UHD Textures**: real photographed materials mixed with AI upscaling, so the look changes from the original.
- **Comic Book Texture Pack**, best with the built in **Cel shading** mod.
- **Your own**: import a texture pack or a mod as a ZIP. [MODDING.md](MODDING.md) and the [Mods page](https://kong.tekrantgaming.com/mods.html) explain how to make them.

The texture packs are separate downloads, and none is needed to play.

**Extras:**

- **PS2 cutscenes** (1.9 GB, offered at setup): the PlayStation 2 version's cutscenes between the missions, upscaled. The next chapter loads while one plays.
- **The Weta Artbook**: six concept paintings from the PS2 bonus disc, with zoom and Save to PC. The launcher downloads the bonus disc from archive.org and keeps only the paintings.
- **The user manual**, and **every Extras item** open from the start.
- **Cheats as switches**: fast healing, one hit kills, 999 bullets, unlimited spears, and the revolver, machine gun, shotgun and sniper rifle.

## Controls

- **Any controller**, with every button remappable, plus camera sensitivity, deadzone and vibration strength.
- **Button prompts** that match the controller you hold: Xbox 360, Xbox Series, PlayStation or Steam Deck, or the classic PlayStation 2 symbols, or your keyboard keys.
- **Keyboard and mouse** with real mouse look and a modern layout: the left mouse button shoots, the right one aims, R reloads, W A S D moves and Esc pauses. Every key and mouse button can be rebound.
- **Camera response**: Modern turns the camera the same way in every direction, and Original is the Xbox 360's own.
- **Comfort**: subtitles, camera shake strength, motion blur, field of view from 69° to 110°, toggle aim, and a hidden HUD.

## Xbox 360 vs PC port

| | Xbox 360 | PC port |
| --- | :---: | :---: |
| Resolution | 1280 x 720 | up to 7680 x 4320 |
| Frame rate | 30 FPS | 30 to 240 FPS, or unlimited |
| Screen shape | 16:9 | 16:9, 21:9 and 32:9 |
| Upscaling | console | DLSS, FSR 3.1, FSR 4, XeSS, FSR 1 and NIS |
| Lighting | console | global illumination, ambient occlusion, sharper shadows, HDR |
| Water | console | optional reflections |
| Textures | console | HD, UHD and Comic Book packs, or your own |
| Controls | Xbox 360 controller | any controller, remapping, keyboard and mouse |
| Field of view | 69° | 69° to 110° |
| Multiplayer | none | online co-op for two (early access) |
| Between missions | none | PS2 cutscenes |
| Cheats | typed as codes | switches in the launcher |
| Original look | always | one setting away |

## Good to know

### Hotkeys

| Key | Action |
| --- | --- |
| <kbd>F2</kbd> | performance metrics |
| <kbd>F8</kbd> | graphics panel, to try settings while you play |
| <kbd>F7</kbd> (hold) | compare with your saved settings |
| <kbd>F9</kbd> | free camera |
| <kbd>F10</kbd> | hide the HUD, the ammo counter and the subtitles |
| <kbd>Shift</kbd> while starting | bring the launcher back if you turned it off |

### Known issues

- A few character animations are not quite right above 30 FPS.
- Online co-op has its own [list above](#online-co-op-early-access).
- Three bugs from the original Xbox 360 game are still there: the V-Rex tail can get stuck at the wall in the Call Kong level, the swamp chapter can freeze while loading, and the Kong pillar lifting prompt can vanish or leave Kong stuck lifting.

### Updates

When a new version is out, the launcher offers it as it opens. **Update now** installs every file of the new version and restarts the launcher, and everything is put back as it was if something goes wrong. Your installed game, saves and settings stay as they are.

### Online players and privacy

**Online players** shows how many people are playing right now. It's explained at setup and is a switch in **Settings > About**. While it's on, the launcher checks in every 5 minutes with four things: a random number that isn't linked to you, the port's version, Windows or Linux, and whether you're in the game. No name, account, IP address or anything from your PC is kept, and switching it off deletes the random number.

The same switch shares a little more to help the game run well on every PC: anonymous graphics data and a short performance summary when you quit (frame rate, graphics card, driver and main settings), and a crash report, if there was one, on the next start. Nothing is sent while it's off. All of it goes to the project's own small server.

### Reporting a problem

Everything useful is in the `logs` folder next to `king_kong.exe` (next to the AppImage on Linux). `king_kong.log` describes your settings and where stutter happens. If the game crashes, a report is saved on your PC with your user name and personal folders removed: **Settings > Advanced > Saved crash reports** opens the folder.

Please attach them to an [issue](https://github.com/TekRantGaming/king-kong-recompiled/issues) together with your CPU, graphics card and the resolution you play at, or ask on the [Discord](https://discord.gg/8eakYSvBZS).

## About the port

*Peter Jackson's King Kong: The Official Game of the Movie* came out in 2005 on PC and on every console of the time. The Xbox 360 version was a launch title for the console, with upgraded graphics and its own achievements.

No emulator runs while you play. The Xbox 360 runs PowerPC code, and a recompiler translates every function of the game's program, read from your own disc image, into code a PC processor runs directly. Our renderer then takes the game's requests to draw and draws them with DirectX 12 or Vulkan, with the console's shaders translated for your graphics card.

Version 1.0.0 came out on 5 October 2026. 25 days and 17 public beta versions later came 2.0.0, and the community's reports shaped every one.

### The "VENTURE" freeze

On Xenia this game gets stuck at the start, looking at the ship's name "VENTURE" ([xenia-project/game-compatibility#1302](https://github.com/xenia-project/game-compatibility/issues/1302)). The cause is how disc reads finish. The game streams music and voice lines with reads it expects to finish right away. The emulator finishes them but reports them as still in progress, so the game thinks it got no data. No voice line ever plays, and the opening waits forever for one to end.

This port finishes those reads the way the console does (`kk/src/io_fix.cpp`), so music, voice lines and the opening all work.

<details>
<summary><b>Building from source (for developers)</b></summary>

You need Visual Studio 2022 Build Tools with Clang, CMake and Ninja.

```powershell
.\setup.ps1 -Iso "C:\Games\King Kong.iso"   # downloads the SDK, unpacks your disc, translates the code
kk\build.bat kk-release                      # compiles
```

The finished game is in `kk\out\build\kk-release`. `Build King Kong.bat` does all of this in one go and puts the result in a `KingKong` folder. Point the game at your files with `--game_data_root`, or keep them in a `game` folder next to the exe. The Linux build is described in [native-renderer/docs/linux.md](native-renderer/docs/linux.md).

</details>

## Credits

- **Peter Jackson's King Kong: The Official Game of the Movie** by Ubisoft Montpellier, published by Ubisoft in 2005.
- **The team**: TekRantGaming (project lead), Jimmykings (contributor), and JedThackeray, GoldenXchains and Master2766 (internal testers).
- **Everyone** who helped test the port, reported a problem, sent a log or a video, or suggested a feature, among them GrummelFritz, leocmp, MustopuhGgi, sammyjo802, Tedwards75, ulyssesaot, Songo, Cardoor, Godilla99, Sproccoli, Trashdows11, tytonibae, Shaner and REVPOD.
- [**ReXGlue SDK**](https://github.com/rexglue/rexglue-sdk), which does the code translation and runs the game, built on the work of [**Xenia**](https://github.com/xenia-project/xenia) and [**XenonRecomp**](https://github.com/hedge-dev/XenonRecomp).
- The native renderer builds on [**XenosRecomp**](https://github.com/hedge-dev/XenosRecomp) (shader translation) and [**NVRHI**](https://github.com/NVIDIA-RTX/NVRHI) (the graphics layer), both MIT licensed.
- Upscaling: NVIDIA DLSS, the AMD FidelityFX SDK (FSR 3.1 and FSR 4) and Intel XeSS, each under its own licence, plus [**AMD FidelityFX Super Resolution 1**](https://github.com/GPUOpen-Effects/FidelityFX-FSR) and the [**NVIDIA Image Scaling SDK**](https://github.com/NVIDIAGameWorks/NVIDIAImageScaling), both MIT licensed. Their notices come with the download.
- Button prompt pictures from [**Xelu's Free Controllers & Keyboard Prompts**](https://thoseawesomeguys.com/prompts/) and [**Kenney's Input Prompts**](https://kenney.nl/assets/input-prompts) (CC0).
- The Discord and Steam marks in the launcher from [**Simple Icons**](https://simpleicons.org) (CC0). Discord and Steam are trademarks of their owners.
- The artwork **Add to Steam** uses comes from [**SteamGridDB**](https://www.steamgriddb.com/game/5249080).

NVIDIA, GeForce RTX, RTX and DLSS are trademarks and/or registered trademarks of NVIDIA Corporation in the U.S. and other countries. AMD, FidelityFX and FSR are trademarks of Advanced Micro Devices, Inc. Intel and XeSS are trademarks of Intel Corporation. This project is not sponsored or endorsed by NVIDIA, AMD or Intel.

> [!NOTE]
> **AI disclosure:** this port was made almost entirely with Claude Code (Anthropic). The repository owner directed and tested the work; the AI did the analysis, code, tools and documentation.

> [!IMPORTANT]
> This project is not affiliated with or endorsed by Ubisoft, Universal Studios, WingNut Films, Microsoft, Xbox, Valve or Discord. It contains no game code or assets (the button prompt pictures are free CC0 art, not taken from the game). Do not open issues asking for game files.
