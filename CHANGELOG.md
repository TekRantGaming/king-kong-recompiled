# Changelog

Every release of the King Kong PC port, newest first. The launcher shows this on its About page, and a
version's notes the first time you start it.

## v2.0.3 (31 October 2026)

### Fixed
- **Crash as a level loads on Windows 10 and early Windows 11 with DirectX 12**: the game sent the graphics card a command these versions of Windows refuse, and the graphics device was lost. Fixed.

## v2.0.2 (31 October 2026)

### Fixed
- **AMD FSR 4 on Radeon RX 9000 cards**: v2.0.1 hid it on these cards too by mistake. It is back on RX 9000 cards.
- **When a graphics card stops responding**: the game now closes with a message instead of crashing, and the crash report says what the card was doing, so these crashes can be tracked down.

## v2.0.1 (31 October 2026)

### Changed
- **AMD FSR 4 is offered on Radeon RX 9000 cards only.** Other AMD cards use FSR 3.1, and a saved FSR 4 on those cards moves to FSR 3.1.

### Fixed
- **Reports and player count**: crash reports, performance summaries, pipeline lists and the online player count were switched off by mistake in v2.0.0 and work again. They follow the Online player count setting, as before.
- **Linux and SteamOS**: RenderDoc's "Capturing Vulkan" text no longer appears at the top of the screen on systems that have RenderDoc installed.

## v2.0.0 (30 October 2026)

King Kong Remastered: everything from the 17 beta versions, now on the stable line. Updating from 1.9.13 keeps your saves, settings and game files.

### What's new
- **Online co-op (early access)**: play the story with a friend. Choose Play Online Co-op on the Home screen, pick a chapter or one of your saves, and invite a Steam friend or share a 6 character code. Both games load the chapter together. It is still rough in places, and your reports help shape it.
- **Our own renderer**: the game is drawn with DirectX 12 (the default on Windows) or Vulkan, at your resolution, up to 8K.
- **Modern graphics**: global illumination, ambient occlusion, sharper shadows, water reflections, light shafts and HDR. The Original Xbox 360 look is one setting away.
- **DLSS, FSR 3.1, FSR 4 and XeSS**: only the upscalers your graphics card can run are listed.
- **Any frame rate**: the boat, the water, the fish, motion blur and the camera are right at high frame rates.
- **Ultrawide is built in**: 21:9 and 32:9 fill the whole width with nothing stretched, and the HUD and menus stay in the middle.
- **The launcher is the main menu**, with the game's own start menu scene live behind it: continue, start a new game or pick any of the 42 chapters.
- **A new first time setup** with Auto detect, which picks the recommended settings for your PC.
- **Every setting explains itself**: what it does, what it costs and its default.
- **A new pause menu** with settings, photo mode with a free camera, and brightness and gamma.
- **The Workshop**: texture packs such as HD, Kong UHD Textures and Comic Book, Kong mods with the built in Cel shading mod, and importing your own.
- **Restored PS2 cutscenes** between missions, the Weta Artbook and the user manual in Extras, and every bonus item unlocked.
- **A built in benchmark** on the main menu.
- **Ready before you play**: the shaders and 16,842 graphics pipelines are prepared before you start, from a list players helped build, so new places stutter far less.
- **Updates install every file** of a new version, and everything is put back as it was if something goes wrong.
- **Linux (experimental)**: a native AppImage, also for the Steam Deck.

### Changed since Beta 17
- **Online co-op (early access)** is new: no beta had it.
- **More graphics pipelines are prepared before you play**: the list that comes with the game now has 16,842.
- **Six more TRG projects in the Workshop**: AVP Evolution: PC Edition, Cartoon Network Racing, Muppet RaceMania, Trials: ReVolution Collection, Angry Birds: The Re-Hatched Collection and The Whopping Recompilation Collection.

## v2.0.0-beta.17 (23 October 2026)

### What's new
- **The Home screen surprise now also works from the keyboard**, and the cheat codes from the original PS2 and Xbox games open it too, on any controller. While it runs, Esc opens the pause menu as Enter and Start do.
- **Ultrawide is built in**: on a 21:9 or 32:9 screen or window the game fills the whole width and shows more on the left and right, with nothing stretched. The HUD, subtitles, menus, pause and photo mode stay in their places in the middle. Nothing changes on a 16:9 screen. The Ultrawide mod is gone: there is nothing to switch on.
- **Ultrawide sizes on any screen**: Window size, Resolution (now also in Borderless) and the first time setup list 21:9 and 32:9 sizes (2560 x 1080 up to 7680 x 2160, as tall as your screen at most). The game draws the wide picture at that size and fits it into your window or screen with black bars above and below, so you can play or try ultrawide on a 16:9 screen. A size wider than your screen gets a window of the same shape that fits.
- **First time setup: a progress bar, Next page, Skip all and Auto detect** at the top right of every page until the last one. Auto detect picks the recommended settings for your PC on every page, then takes you to the texture pack and PS2 cutscene questions. Skip all asks first, because a game that is not set up well may play worse.
- **The setup's Workshop page explains the Workshop**: the HD, UHD, Comic Book, Cartoon and Tropical Holiday texture packs and the mods, how to download them and switch them on, and how big the packs are, with their pictures.
- **A countdown to the King Kong Remastered update on the Home screen**: the time left until Friday 30 October, 18:00 UK time (also shown in your own time), with a Join the Discord button. When the time comes it says the update is out and offers Check for updates.
- **The updater installs every file of an update**: it checks the download first, installs every file the new version ships, removes old files the game no longer uses (never your saves, settings or mods), and puts everything back as it was if something goes wrong. On the first start of a new version the game also checks its own files and fetches any that are missing.

### Changed
- **Field of view is 80 by default**, marked recommended. 69, the original, is still there and marked as the original. If you already play, your field of view stays as it is.
- **The shader screen stays until the shaders are ready**: Continue in the background and Skip all are gone from it, so the game never starts while its shaders are still being prepared.
- **PS2 cutscenes now play first and the next level loads after them**, so nothing loads under the clip.
- **A new Discord invite link** in the launcher and the game.
- **Plainer wording** in the launcher and the game menus, in all five languages.
- **Switching back to stable keeps the renderer settings** once stable 2.0.0 is out. Only a stable version before 2.0.0, which has no native renderer, still puts them back to their defaults.
- **Intel integrated graphics (UHD, Iris, HD) use Vulkan by default**, and the setup no longer prepares the DirectX 12 shaders for them, which saves a long wait. Intel Arc is not affected. You can still pick DirectX 12 in Settings.
- **More graphics pipelines are prepared before you play**: the list that comes with the game is updated (16,548).

### Fixed
- **Some Intel graphics crashed at every launcher start** while the shaders were being prepared. If the graphics driver ends the game there, the next start now leaves that step out instead of crashing again.
- **AMD FSR 4 is offered on AMD graphics cards only**, as intended. On other cards it could make the game hang. A saved FSR 4 on another card goes back to the recommended upscaler for that card.
- **Quitting could hang**: if the game has not closed 10 seconds after you quit, it now closes itself.
- **Crash reports note overlays** such as ReShade, RivaTuner, RenoDX and Overwolf, to help find crashes they cause. Nothing changes on screen.

## v2.0.0-beta.16 (22 October 2026)

### What's new
- **A little surprise on the Home screen** for anyone who knows a certain famous controller code.

## v2.0.0-beta.15 (21 October 2026)

**Beta 15**: a quick fix for Beta 14, from the beta channel (**About** > **Change version**). Your saves and settings are kept. Windows only for this beta.

### Fixed
- **Windows Defender wrongly flagged a game file** (`rexgpu-native.dll`) in Beta 13 and Beta 14 as a threat and removed it, so the game showed "Failed to load GPU plugin" and would not start. It was a false alarm. Beta 15 ships a freshly built file that antivirus programs do not flag. If the game will not start, download Beta 15 from the releases page and unpack it over your game folder; your saves are kept.
- **A rare crash when changing several graphics settings quickly** (for example in the F8 panel): the game now waits for the graphics card to finish before it switches effects on or off.
- **Updating from inside the launcher skipped some files**: the startup movies, menu videos and built in mods are now updated too.

## v2.0.0-beta.14 (21 October 2026)

**Beta 14**: fixes and performance, from the beta channel (**About** > **Change version**). Your saves are kept; the Display, Graphics and Controls settings start again from the new defaults (see below). Windows only for this beta.

### What's new
- **DirectX 12 is now the default on Windows**, for new players and for everyone updating. Vulkan is used automatically if DirectX 12 cannot start on your PC, and you can still choose it in Settings > Graphics.
- **A fresh first time setup**: updating to Beta 14 takes everyone through the setup once more, starting from the new defaults and the preset recommended for your PC. Your saves, game files and downloads are kept, and the game files page is skipped. The graphics are prepared again for DirectX 12 the first time, which can take a few minutes.
- **Graphics presets now set every graphics setting**, including shadows, water reflections and light shafts: High gives High shadows, water reflections and low light shafts; Ultra gives Ultra shadows, water reflections and high light shafts. Changing any one setting shows Custom.

### Fixed
- **A crash a few seconds into the benchmark** on some runs.
- **Updating from inside the launcher skipped some files**: the startup movies, menu videos and built in mods were not updated. They are now, and updating from Beta 13 brings the missing ones in on the first start.
- **The game closing by itself at the start on some PCs** (seen on Intel UHD Graphics): the check of which upscalers your PC offers now runs on its own, so a graphics library that fails during the check only turns that upscaler off.
- **Stutter in busy scenes with DLSS, FSR and XeSS, such as Ann's chapter**: the work that prepares motion information for the upscaler grew much faster than the number of objects on screen. It now takes a fraction of the time, with exactly the same result: in our test of Ann's chapter, frames over 20 ms dropped from about 210 to 3.
- **The live main menu now always loads behind the loading screen**, including on the Steam Deck and after setup or an update. A slow or failed start tries again instead of falling back to the slideshow of pictures.
- **Up to a 10 second freeze at the start when Windows was slow to open the sound device**: the game, the menu music and the button sounds now open their sound in the background, so the game keeps running while Windows gets the device ready.
- **A long freeze when Windows was slow to write the game's log file**: the log is now written in the background, so a slow or busy drive can no longer hold up a frame.
- **A rare crash at the start on some NVIDIA graphics cards**: the launcher now checks which upscalers your PC offers before it opens its window, and remembers the answer until your graphics card, driver or the game changes. The first start after such a change opens a second or two later.

### Changed
- **A new TRG Studios startup movie**, with sound.
- **HDR is now off by default** and no longer in Settings or the setup. You can still try it as **HDR (experimental)** in the F8 graphics panel.
- **Faster shadows with DLSS, FSR and XeSS**: soft shadows and the game's own shadows are now worked out at the upscaler's resolution instead of the full output size, and the soft shadows at High and Ultra use slightly lighter internal settings with no visible difference. About 25% more frames at 4K in our tests.
- **Smoother chapter starts after the PS2 cutscenes**: while a story cutscene plays, the game now prepares most of the next chapter's textures, so far fewer have to load in the first minute of play.
- **Lighter defaults on weak hardware**: the first setup picks a lighter preset for handhelds, integrated graphics, entry gaming laptops and PCs with little video memory. Existing players keep their settings.
- **Medium preset**: uses the original shadows, which saves a lot of graphics time on midrange PCs.
- **Less work for your CPU**: parts of the renderer that run for every draw are faster, with exactly the same picture.
- **More graphics prepared ahead of time**: the shipped list now has 16,295 graphics pipelines (1,598 more), from players who share them with Online players on, so fewer things hitch the first time you see them.

### Known issues
- **With FSR, a few short stutters remain in very busy scenes** such as parts of Ann's chapter, when the camera swings quickly. The long ones are gone, and DLSS is not affected. We are still working on it.

## v2.0.0-beta.13 (20 October 2026)

**Beta 13**: a live main menu, from the beta channel (**About** > **Change version**). Your saves and settings are kept. Windows only for this beta.

### What's new
- **The main menu is the game's own start menu scene, live**: the Venture rolls on the stormy sea under Skull Island behind the launcher, moving, with the menu's own music and the sound of the sea (Settings > Audio > Menu music and its volume apply to it). It replaces the slideshow of pictures. The pictures still show during setup, on the first start after an update, while shaders are prepared, on low end PCs such as the Steam Deck, and if the scene could not start last time.
- **Loading screens for the main menu**: when you open the game, a screen explains when the game saves (when you choose Save game or quit from the pause menu) while the main menu gets ready. When you come back from the game, **Returning to the main menu** shows until the main menu is ready, with **Game saved at your last checkpoint.** after a story game you quit from the pause menu, and a tip about the game, a different one each time.
- **A new first time setup**: after the game files and the shaders, the full **Display**, **Graphics** and **Controls** settings pages, then button prompts, the HD texture pack, the PS2 cutscenes, what the **Workshop** is, and Join our Discord / Add to Steam. **Skip all** at the top of every page after the game files goes straight on, with every setting as it is; all of them can be changed later in Settings on the main menu. Players who update keep the one time New in this update screen.
- **Startup movies**: the Ubisoft, Universal and WingNut logo movies from your copy of the game, then a TRG Studios movie, play once after the setup, and then each time you open the game unless Settings > Gameplay > Startup logos is set to Skip (it skips the TRG Studios movie too). The loading screen follows them until the main menu is ready.
- **Extras > Original main menu**: the game's own Xbox 360 main menu, as it was, with its pages, options and saves. Start a game from it, then pause and quit to come back to the launcher. The next time you open the game, the launcher's menu is back.
- **Extras > User manual**: the game's Xbox 360 manual, page by page, with zoom. It downloads in the background (about 2 MB) the first time the launcher runs.
- **Workshop**: the other TRG Studios projects show their proper artwork, downloaded the first time you open the Workshop.

### Changed
- **More graphics prepared ahead of time**: the shipped list now has 14,697 graphics pipelines (59 more), from players who share them with Online players on.

### Fixed
- **After the first time setup, the old pictures showed before the live main menu.** The startup movies and the loading screen now cover the main menu while it gets ready.

## v2.0.0-beta.12 (19 October 2026)

**Beta 12**: fixes from the beta channel (**About** > **Change version**). Your saves and settings are kept. Windows only for this beta.

### Fixed
- **A cheat gun could still reach the Hayes chapter.** A Revolver, Machine gun, Shotgun or Sniper rifle cheat switched on in On Kong's tracks stayed with Jack into Hayes and could leave the raptor fight stuck. The weapon cheats are now also not available in Sacrifice and On Kong's tracks, so nothing is carried into Hayes. Your cheat settings are not changed.
- **Kong could get stuck lifting the stone slab away from the gate in The Skull Islanders** at 60 FPS and above, and the gang could not go on. The lift now works at any frame rate.
- **The Cheats page in the pause menu no longer disappears** when the main Cheats switch is off, so you can switch it on again while you play.
- **Keyboard and mouse pictures**: Settings > Controls shows the mouse buttons and the mouse as pictures (the keyboard bindings rows and the mouse rows), and photo mode shows the mouse for looking around.
- **Keyboard and mouse: the game's stick prompts no longer flicker through key pictures.** "Right stick to aim" shows the mouse, and the move prompts show W A S D. Controller prompts are unchanged.
- **Photo mode's free camera turned the wrong way up and down with the mouse.** Moving the mouse up now looks up, as in play, and Mouse vertical in Settings > Controls turns it round. The F9 free camera is the same. The controller is unchanged.
- **When the game saves**: in story play, the pause menu's Quit to main menu and Quit game questions now say that the game saves when you quit from that menu or choose Save game, not at checkpoints, so closing the window another way loses unsaved progress.

### Changed
- **More graphics prepared ahead of time**: the shipped list now has 14,638 graphics pipelines (258 more), from players who share them with Online players on, so fewer things hitch the first time you see them.

## v2.0.0-beta.11 (19 October 2026)

**Beta 11**: new features and fixes, from the beta channel (**About** > **Change version**). Your saves and settings are kept. Windows only for this beta.

### What's new
- **HDR follows Windows**: HDR is on when Windows HDR is on for the screen the game is on, and off otherwise. Settings > Display > HDR has **Follow Windows** (the default), **On** and **Off**.
- **HDR brightness from Windows**: by default, Peak brightness comes from the Windows HDR Calibration app and Paper white from the SDR content brightness in Windows. Choose **Manual** to use the sliders. The HDR rows show the values Windows reports.
- **Open Windows HDR Calibration** button in Settings > Display (the Microsoft Store page when the app is not installed). The values are read again when you come back.
- **New HDR highlights**: HDR now brightens highlights smoothly, up to a 1000-nit cap by default. This fixes overexposed light shafts with a grey core. The strength, the starting point and the cap are sliders in the F8 menu.
- **Setup graphics pictures from your own game**: the first setup's graphics pages no longer use screenshots shipped with the download. Their pictures are made from your copy of the game while the launcher runs, and options that only show in the game itself show a still from your game with a description.
- **Workshop pictures and details**: every texture pack and built in mod has a details table (what it changes, how it looks, what it goes well with, its size, what it needs) in five languages, and the texture packs and Cel shading have a picture of what they do. Kong UHD Textures and Kong Comic Book show their own pictures before you download them (about 1 MB each); the HD texture pack and Cel shading pictures are made from your copy of the game. The **Other TRG projects and games** row comes up in a new random order each time you open the Workshop.
- **Two more texture packs in the Workshop**: **Kong's Tropical Holiday Texture Pack** and **Kong Cartoon Texture Pack**, downloads in Workshop > Texture packs like the UHD and Comic Book packs. Just for fun, very different from the original look. One texture pack is on at a time.
- **Ultrawide (experimental)**, a built in mod in the Workshop, off unless you switch it on: on a 21:9 or 32:9 screen or window the game fills the width and shows more on the sides instead of black bars, without stretching. The ammo counter and the subtitles stay in a 16:9 box in the middle of the screen. It may not work as intended: menus and some effects are not adapted yet. Photo mode is wide too, and Settings > Display > Aspect ratio says when the mod is on and the size of the picture. Without the mod nothing changes.

### Fixed
- **Long stutters in Ann's chapter with DLSS, FSR or XeSS on.** When a lot of objects were on screen, the renderer's motion tracking for the upscaler took far too long each frame and the picture froze in bursts. It is now many times faster, with the same picture.
- **The launcher could crash again and again with a black screen on some AMD graphics cards** when the driver's own shader cache was damaged. The launcher now notices when it ended unexpectedly: the next start clears the game's own shader cache, explains how to empty the AMD shader cache folders and offers to empty them for you if you agree, and after two such ends in a row it switches to the other graphics API (Vulkan or DirectX 12).
- **The launcher or game could close without leaving a trace.** Every close now leaves evidence in the logs: a crash report with the reason, how long the launcher or game ran before it stopped, and what Windows recorded about it. This helps us find why some launchers closed by themselves.
- **Closing the game now lets shader preparation finish first**, so the game no longer closes in the middle of it. This helps against the crash on some AMD graphics cards.
- **Garbled letters in Settings**: the Language row showed "EspaÃ±ol" and "FranÃ§ais", and Field of view showed "69Â° (original)". They now read Español, Français and 69° (original).
- **The Upscaler description is translated**, and in the pause menu it no longer says things that only apply in the main menu.
- **Field of view in the pause menu**: with a field of view wider than 69°, the game behind the pause menu zoomed in to 69° and a new field of view showed only after Resume. The paused picture now keeps your field of view, and changing it in the pause menu shows at once, as bright as the moment you paused.
- **Hide HUD and F10 now also hide the ammo counter and the subtitles.** Loading screens, fades and the game's own pages (Score, Save game, Options) still show.
- **Ambient occlusion now shades Jack's hands** and the gun he holds in chapters where they were drawn without it (such as the Hayes chapters): the creases of his fingers and the gun are darker like the world around him. With ambient occlusion off nothing changes.
- **Finishing a chapter from Chapter select now plays the PS2 cutscene that follows it** (when the PS2 cutscenes are restored), as in story play, before the Score pages. Afterwards you are back in the main menu's Play menu.
- **Fewer long freezes when a chapter starts while shaders are still being prepared.** The game no longer waits long for a missing shader: some things may appear a moment later instead.
- **Graphics driver resets and running out of video memory** no longer crash the game silently. If it happens while the game starts, it starts again on the other graphics API and tells you; during play it closes with a crash report. A first start that still has to prepare its shaders no longer counts that time as part of the start, so a slow first start is not reported as a hang and still gets this protection. The launcher no longer closes when shader preparation hits a graphics error.
- **Photo mode pictures are no longer much darker than the game**: the photo mode view and the saved pictures are as bright as the game in play. Very bright sunlit scenes can come out slightly brighter.
- **A rare hang when pressing Play in the main menu** while the sound device was being opened again: the game now keeps the sound system running across the switch, and the game's own sound scheduling now works as intended.

### Changed
- **Smoother chapter starts.** Each chapter's own shaders are prepared first when it loads, the textures it needs are prepared ahead on the loading screen, and on DirectX 12 textures and buffers are made far faster. Fewer hitches in the first seconds of a chapter.
- **Simpler downloads.** A texture pack or the PS2 cutscenes start downloading straight away and show their progress with Pause, Run in background and Cancel; there is no longer a question first. With **Notify me when downloads finish** on, a notification still tells you when a download running in the background has finished.
- **No more crash report popup when the launcher starts.** Crash reports are still saved (and sent with the online switch on, as before); Settings > Advanced > **Saved crash reports** opens their folder.
- **No loading screen after the PS2 cutscenes.** When the restored PS2 cutscenes play between chapters, the next chapter now loads while the cutscene plays.
- **The game stays smooth when other programs are busy.** The game's own threads now get priority over background work on your PC.
- **Every setting explains itself.** The panel beside the settings, in the launcher and in the pause menu, now says what each setting does, what you will notice in the game, what each option changes, and shows its performance and video memory impact and when a change takes effect. In five languages.
- **New pause menu.** A short list on the left over the paused game: Resume, Photo mode, Controls, Gameplay and Settings, then Load last checkpoint (and Score or Save game where the game offers them), Quit to main menu and Quit game. **Settings** opens a second list with Display, Graphics and Audio; Back takes you to the list you came from. **Load last checkpoint** takes you back to the last checkpoint you reached, as the game does after a death (but it does not count as one), after you confirm; it is there in every chapter, also outside chapter select runs. Display, Graphics, Audio, Controls and Gameplay each open their own page, with the settings you can't change while playing greyed at the bottom under **Change from the main menu**. A strip at the bottom says what the selected item does. Works with a controller, keyboard and mouse.
- **Weapon cheats are not available in the Hayes chapter.** The Revolver, Machine gun, Shotgun and Sniper rifle cheats could leave the raptor fight with Hayes stuck, so in that chapter you play with the weapons the chapter gives you. In the pause menu their cheats are greyed with "Not available in this chapter." Your cheat settings are not changed: in every other chapter they work as before.
- **Some settings moved to another tab**, in the launcher and the pause menu alike: Field of view, Motion blur and Camera shake are now in Settings > Display > Camera, and Performance metrics in Settings > Gameplay > Overlay. The frame rate cap stays in Display.

## v2.0.0-beta.10 (18 October 2026)

**Beta 10**: an emergency fix update, from the beta channel (**About** > **Change version**). Your saves and settings are kept. Windows only for this beta.

### Fixed
- **Jack's arm and what he holds flickered with an upscaler on** (DLSS, FSR or XeSS). In some places, for example in the Hayes chapter, parts of the arm and of the gun, spear, bone or lever turned into flat patches in the colour of the fog for single frames. They are now drawn whole.
- **A bug from the original game in the Hayes chapter**: the raptor that runs round to attack the sailor could stop for good on a ledge near the stone stairs, out of reach, so it could never be killed and Hayes never came down. Players often hit it with the machine gun cheat on, but it can also happen without any cheat. A raptor that stands still on its path there now carries on along it, so the fight always ends.

### Changed
- **Kong Comic Book is now called Comic Book Texture Pack** in the Workshop.
- **More shader data from players**: the list of graphics pipelines prepared at setup now includes those collected from players, so fewer short pauses the first time new effects appear.

## v2.0.0-beta.9 (18 October 2026)

**Beta 9**: new features and fixes, from the beta channel (**About** > **Change version**). Your saves and settings are kept. Windows only for this beta.

### What's new
- **Brightness and gamma** in Settings > Display > Picture and in the pause menu. Brightness raises or lowers the darkest parts of the picture, Gamma the middle tones. Changes show at once, also while paused. Only the game's picture changes, not the launcher or the menus. At 0 and 1.00 the picture is exactly as before. With HDR, white stays at the Paper white brightness.
- **Photo mode** in the pause menu: the game stays paused while you fly a free camera around (left stick and right stick, triggers down and up, or W A S D and the mouse), with roll and field of view. Jack's floating arms and the HUD are hidden. Looks (black and white, sepia, tints), exposure, contrast, saturation, white balance, depth of field, vignette, film grain, letterbox bars and a mirror, plus cel shading, sharpening, brightness and gamma for the picture only. **A** (Enter) saves a full resolution picture without any menus in the screenshots folder of your user data. Nothing you change in photo mode is kept or changes your settings.
- **Texture packs in the Workshop**: every texture pack you have is a card with its picture, name, author and version and a clear **Enabled**, **Disabled** or **Not installed** badge: the original textures, the HD texture pack, **Kong UHD Textures** (new, a download in the Workshop), packs you imported and your own folder. Select a card for its page: Enable or Disable, Download, Remove (it asks first), its credits. **Only one texture pack is on at a time**: switching one on switches the one before off, everywhere (the Workshop, Settings > Graphics, the pause menu, after a download or import). Settings from an older version with several packs on keep one.
- **Kong UHD Textures**: every texture at up to four times its resolution, with real photographed materials for rock, moss, ground, wood and more. Its page says plainly, before the download starts, that some of its materials were not made for this game, so the game will not look exactly as its developers intended.
- **Comic Book Texture Pack**: a comic book style texture pack, a second download in the Workshop's Texture packs row. We recommend also enabling the Cel shading mod: its page has a button that switches it on.
- **Import texture pack** and **Import mod** buttons in the Workshop, for ZIP files laid out as MODDING.md describes. The ZIP is checked first: if something is wrong (no mod.toml, no pack.txt, folders where they are not allowed, a mod given to the texture pack button), it says what, and nothing is installed or changed. An imported texture pack keeps its name, picture and credits and becomes the one texture pack on; it never replaces the HD texture pack. A long import shows how far it is and can be cancelled. **Build your own mods and texture packs** opens the website's Mods page with the modding tools.
- **Performance metrics** replace the frame counter (Settings > Display > Overlay, or **F2** while playing): Off, FPS only, or Full with a frame time graph, the graphics card's load, temperature, clock and memory, the processor and memory use, the graphics API and the upscaler. A frame counter that was on becomes FPS only.

### Fixed
- **Vulkan crash when loading a chapter on AMD graphics cards.** The faster pipeline building added in Beta 7 is now used on NVIDIA cards only, where it was tested; other cards build pipelines the earlier way.
- **Crash during shader setup on laptops with two graphics chips.** The shader setup, the launcher and the game now always use the more powerful graphics card instead of the built in Intel or AMD chip. A graphics card chosen in the settings file is still respected.
- **Pause menu in the Weta artwork galleries.** In a gallery started from Extras, **Start** or **Esc** threw you straight back to Extras. It now opens the pause menu, as in any chapter: **Resume**, **Settings**, **Return to main menu**. A gallery never writes to your save.
- **The launcher flickered.** When it was busy for a moment (for example while unpacking a large ZIP), it rebuilt its picture each time it came back, which showed as a flash. It now does that only when the window really stopped drawing.
- **Importing a big texture pack ZIP replaced the HD texture pack** and named it "textures". An imported pack is now a pack of its own beside the HD texture pack.
- **Shaders said "Not prepared" after setup.** After the first setup or an update the shaders are prepared once, and the main menu now shows them as **Ready** from then on, including after restarting the launcher. They are not prepared again when you play, change graphics settings or restart, only when you choose **Rebuild shaders**. If the launcher is closed before they finish, they are prepared the next time it opens.
- **DLSS missing from the upscaler list in the pause menu.** While playing, the list could say DLSS was not available on cards that support it. It now lists what the running game actually supports.

### Changed
- **The Old movie filter is gone from Graphics and Extras.** The game no longer turns on its sepia filter from a save. Filters are in photo mode.
- **All bonus content is unlocked.** Every Extras item can be played straight away, with or without a save, the same way every chapter is open in Chapter select. It is not a cheat to the game: scores and completion are recorded as usual. The "All bonus content" cheat is gone: it isn't needed any more. In Extras there is no save to choose any more, and the Artbook is first in the list.
- **Benchmark on the main menu** in place of About. About is the last tab in Settings.
- **The Workshop is one page with three rows:** Texture packs, Mods (many on at once, in load order; the modding guide and your mods folder are at the end of the row) and Other TRG projects and games. Up and Down move between the rows, Left and Right along them; Back from a card's page returns to the same card.
- **Settings > Graphics > Texture pack** is now a choice of the packs you have installed (or the original textures). Downloading, importing and removing packs are in the Workshop.
- **First setup** no longer skips the HD texture pack and PS2 cutscene pages without a word when they are already installed: the page says "Already installed" and you press Continue.
- **No frame rate popup** when pressing Play with a frame rate above 60 FPS.
- **More settings in the pause menu.** Settings that cannot change while you play (window, resolution, VSync, render quality, look, preset, graphics API, language) are now shown greyed out with a note to change them from the launcher's main menu. The upscaler (DLSS, FSR 4, FSR 3.1, XeSS or Off), its mode and the frame rate cap now change while you play. A new **Graphics** entry opens the graphics settings directly.

## v2.0.0-beta.8 (17 October 2026)

**Beta 8**: a fix update, from the beta channel (**About** > **Change version**). Your saves and settings are kept. Windows only for this beta.

### Fixed
- **Shaders are prepared only once.** At the first start and after an update the launcher prepares the shaders one time. Playing a chapter or going back to the launcher no longer prepares them again. Settings > Graphics > Advanced > Rebuild shaders still does it when you ask.

### Changed
- **Global illumination distance goes up to Far in the launcher** (Ultra now uses Far). Full is still in the F8 panel; it costs too much for most PCs.

## v2.0.0-beta.7 (17 October 2026)

**Beta 7**: from the beta channel (**About** > **Change version**). Expect bugs, and please tell us how it runs on the [Discord](https://discord.gg/8eakYSvBZS). Your saves and settings are kept, and you can switch back to stable at any time. Windows only for this beta.

### What's new
- **Kong mods in the Workshop.** Put a mod in your `mods` folder and turn it on or off in the Workshop. Mods can be texture packs, settings presets or replacement cutscenes. See MODDING.md for how to make one.
- **Built in Cel shading mod** (ink outlines and flat colours), on or off in the Workshop.
- **Artbook in Extras.** The six Weta concept paintings from the PS2 bonus disc, downloaded in the background, with zoom and Save to PC.
- **Smoother ambient occlusion.** Soft, rounded shading instead of hard creases on characters, Kong and rocks. On by default with the Remastered look.
- **Better water reflections.** The screen space reflections no longer break up on moving water and fade softly at their edges (Settings > Graphics, off by default).
- **Fewer stutters for everyone.** The list of graphics pipelines prepared during setup is about 50% larger, including pipelines shared by a player.
- **Help reduce stutter.** With Online player count on, the game shares its graphics pipeline data and an anonymous performance summary when you quit, and crash reports on the next start. Nothing personal is sent.
- **Two new projects in the Workshop:** South Park: Tenorman's Revenge Recompiled and South Park: Let's Go Tower Defense Play! Recompiled.

### Known issues
- FSR 4 runs on DirectX 12 only.
- Vulkan uses more memory than DirectX 12.
- Kong's fur is not outlined by the Cel shading mod.

### Full changelog
- AO: edge aware smoothing of the depth rebuilt normals before GTAO (`native_ao_normal_smoothing`, High with Strong GTAO).
- Water reflections: traced from the water's flat plane with a small ripple (`native_ssr_ripple`), soft fade where rays miss (`native_ssr_soft_edge`).
- Cel shading screen effect (`native_cel_*`, F8 "Cel shading").
- Uploads (only with Online player count on): pipeline record file and performance summary on quit, crash and hang reports with the last 200 log lines (names and personal folders removed) on the next start.
- Artbook: the PS2 USA bonus disc is downloaded from archive.org, checked and read by the launcher; only the paintings and their readme are kept.
- Shipped pipeline list: 11,666 records (was 7,594).

## v2.0.0-beta.6 (17 October 2026)

**Beta 6**: a big update, from the beta channel (**About** > **Change version**). Expect bugs, and please tell us how it runs on the [Discord](https://discord.gg/8eakYSvBZS). Your saves and settings are kept, and you can switch back to stable at any time. Windows only for this beta.

### What's new
- **A new first time setup.** After the shaders, setup offers an optional HD texture pack, the restored PS2 cutscenes, four pages of graphics options with side by side comparisons, your choice of button prompts, and links to the Discord and Add to Steam. New installs start with the Remastered look; the Original look is in Settings > Graphics.
- **Optional HD texture pack.** A separate 2.6 GB download with sharper textures at four times their original size. It downloads in the background and can be turned on or off at any time in Settings > Graphics. You can also install a texture pack ZIP you already have.
- **Restored PS2 cutscenes.** The cutscenes between missions from the PlayStation 2 version, upscaled. A separate 1.9 GB download, on or off in Settings > Advanced.
- **Downloads tell you when they finish**, with a sound and a Windows notification, and pause while you play.
- **High frame rates fixed for the boat, the hanging boxes, the fish, the water, motion blur and the camera** at the start of chapters. They now behave the same at 60, 120 and higher as at 30.
- **Far fewer stutters.** Missing graphics pipelines no longer freeze the game, Vulkan builds new pipelines in under a millisecond on supported drivers, the shipped pipeline list covers much more of the game, and short hitches around 30 seconds into each chapter and when saving files are gone.
- **Upscalers (DLSS, FSR, XeSS) now draw at the resolution you choose in the launcher**, and without an upscaler the game never draws above it. This is faster on screens below 1440p.
- **Sharper textures with DLAA and native antialiasing.**
- **Settings reorganised**: Display, Graphics, Audio, Controls, Gameplay, Cheats, Advanced and About. About is also on the main menu.
- **Workshop** on the main menu: our other projects, with their pages and download links. Kong mods are coming soon.
- **Free camera** (F9) and hide the HUD (F10) are now part of the game. See FREECAM.txt.
- **Built in benchmark** on About > Run benchmark, fully usable with a controller.
- **Shadows, water reflections and light shafts** can be turned on in Settings > Graphics (off by default, still being improved).

### Known issues
- The HD texture pack is checked in several chapters; tell us if you see a texture that looks wrong.
- FSR 4 runs on DirectX 12 only.
- Vulkan uses more memory than DirectX 12.
- On the very first start after installing or a driver update, Vulkan prepares pipeline parts in the background for about half a minute.

### Full changelog
- Launcher: Extras in Play; one shader progress bar; no shader compile at Play; no desktop between screens; Rebuild shaders in Graphics > Advanced; Display page opens instantly; keeps working while minimised; ignores controllers while it is not the active window.
- Content downloads: resume after a lost connection or restart, SHA-256 check before installing, free space check, installed into the user folder.
- Renderer: per list volatile constant budget (heavy frames cannot overflow), retired resources released over several frames, cache files written on a background thread, driver pipeline cache saved before exit and safe when two devices save it, texture pack limit is a quarter of graphics memory, texture pack replacements freed at chapter changes and shared between identical textures.
- Pipelines: scene draws wait at most 4 ms per frame for a pipeline, then skip that draw until it is ready; Vulkan graphics pipeline library with linked pipelines replaced by optimised ones in the background.
- High frame rate fixes: game time at a chapter start, boat roll, hanging box wind, camera pitch after landing, motion blur fade.
- Benchmark: other programs' GPU use counts only 3D work on the game's graphics card.

## v2.0.0-beta.5 (16 October 2026)

**Beta 5**: a small update to Beta 4, from the beta channel (**About** > **Change version**). Expect bugs, and please tell us how it runs on the [Discord](https://discord.gg/8eakYSvBZS). Your saves and settings are kept, and you can switch back to stable at any time.

### What's new
- **Global illumination is now on by default** and follows the graphics preset: Low, Medium or High (Ultra uses High, Steam Deck uses Low). The Original look keeps it off. If you chose a level yourself, it is kept.
- **An experimental native Linux version.** A Linux AppImage (`KingKong-v2.0.0-beta.5-linux-x86_64.AppImage`) runs the game natively with the Vulkan renderer. It was tested on a ROG Ally with Bazzite, including Steam's Game Mode (add it to Steam as a non Steam game). DLSS, FSR 3/4 and XeSS are Windows only; FSR 1 is available. This is an early test build: please report how it runs.
- **The renderer does much less CPU work for each thing it draws.** This helps most on handhelds and slower processors, and with high frame rates.
- **Smoother Vulkan presentation.** The final step that puts each frame on screen no longer waits for the graphics card and rebuilds itself every frame.
- **Fewer stutters the first time you reach new places.** The list of graphics pipelines prepared before you play now covers more of the game.
- **Settings changed in the pause menu are saved.** In Beta 4 they were lost when the game closed.
- **Faster texture loading** when a chapter starts.

### Known issues
- Short stutters can still happen in the first seconds after a chapter loads.
- Ann's tessellated model (experimental, F8 panel) looks broken from certain angles.
- With global illumination on and tessellation off, Ann can look odd in some lighting.
- FSR 4 is untested on the cards that support it, and runs on DirectX 12 only.
- In a few chapters the camera starts a pixel or two higher than on the console.
- With motion blur on, its trail is shorter at high frame rates.
- Coming in Beta 6: sharper shadows, reflections on water and volumetric light shafts, a low graphics memory mode, support for older graphics cards, high frame rates done properly, frame pacing and a built in benchmark.

### Full changelog
- Graphics presets set and match `native_gi`: Low = Low, Medium = Medium, High/Ultra = High, Steam Deck = Low. Remastered and new installs use the hardware recommendation. Settings version 3 migrates GI that was never saved, preserves saved choices, and keeps GI with the saved Modern look while Original is off.
- SDK patch 0038 (Vulkan presenter keeps the swapchain format of its final pipeline; before, every paint waited for the previous one and rebuilt the pipeline), now on Windows too; 0039/0040 (Linux: timed waits sleep, the timer queue waits instead of spinning).
- The render thread receives only the device state blocks that changed; shader constants are compared and converted in 64-byte blocks; indexed draws scan their indices only for dynamic vertex buffers; the AO rules test shader hashes first; texture untiling and copying do less work per block.
- The pause menu's settings use the settings file path from the start (the dialogs were made before the path was known).
- The shipped pipeline list adds the pipelines recorded on Linux.
- Linux: the launcher process exits after starting the game, the pipeline setup is not repeated at every start, and the per thread CPU times in the frame timing log are measured.

## v2.0.0-beta.4 (15 October 2026)

**Beta 4**: the fourth test build of the next big update, from the beta channel (**About** > **Change version**). Expect bugs, and please tell us how it runs on the [Discord](https://discord.gg/8eakYSvBZS). Your saves and settings are kept, and you can switch back to stable at any time. Windows only for now.

### What's new
- **Native rendering only.** The game now always uses the native renderer, on **Vulkan** (the default) or **DirectX 12**. The Classic renderer is gone. If you had Classic selected, you start on Vulkan. **Graphics** > **Advanced** > **Graphics API** offers **Auto**, **Vulkan** and **DirectX 12**.
- **Safer start.** If the graphics API you chose fails to start, the game tries the other one and tells you. If neither starts, it explains how to update your graphics driver and where to get help. Graphics cards that can't run Vulkan are told so clearly, with a suggestion to use DirectX 12.
- **No more texture flashing at chapter starts.** The game now waits for a new shader to be ready instead of skipping what it draws.
- **Fixes for heavy firing.** Long bursts of machine gun fire, especially at 4K, no longer make the game fall apart: the renderer no longer lets unfinished frames pile up.
- **VSync now works on Vulkan.**
- **Smoother running.** The screen is now only redrawn when the game has a new frame, or a menu is open or moving. Before, the same frame could be sent to the screen many times a second. Overlays such as NVIDIA's now show the same frame rate as the game's own counter (**F2**). The renderer also does less timing and logging work on every draw.
- **Global illumination (new, off by default).** **Graphics** > **Global illumination**: **Low**, **Medium** or **High**. Light bouncing off sunlit surfaces softly lights and tints nearby shaded ones. It only adds light, and it works best in bright outdoor scenes.
- **The F8 graphics panel.** Press **F8** in play to change any graphics setting live, including the upscaler. Hold **F7** to compare with your saved settings, and **F6** saves a screenshot with the settings beside it.
- **Tessellation test on Ann (experimental, off by default).** In the **F8** panel only: **Tessellation**, with **Smoothness** and **Detail** sliders, smooths Ann Darrow's model. Nothing else in the game is changed. The difference is subtle; tell us what you see.
- **Remastered** plays at **60 FPS** with **Strong** ambient occlusion. **Original** keeps 30 FPS.
- **Recommended settings for your graphics card** in the setup.
- **One shader setup.** The setup shows one progress bar from 0 to 100% with each step named, the launcher music waits until it's done, and one CPU thread is kept free. Pressing **Play** doesn't prepare the shaders again.
- **Audio and subtitles.** Volume settings now apply to every save, with no save slot to pick. **Subtitles** is a row on the **Gameplay** page instead of a page of its own. The pause menu's **Settings** uses the same audio and subtitle settings, and changes apply at once.
- **Diagnostics.** **About** > **Copy diagnostics** now includes your screen size and refresh rate. If the game crashes, a report is saved on your PC with personal details removed, ready to send to us.
- The launcher's newer text is translated into German, Spanish, French and Italian.
- The shaders are prepared again the first time you start Beta 4, as after every update.

### Known issues
- FSR 4 is untested on the graphics cards that run it (we have none here), and it runs on DirectX 12 only.
- Coming in Beta 5: sharper shadows, reflections on water and volumetric light shafts (hidden in Beta 4), a low graphics memory mode, support for older graphics cards, high frame rates done properly, frame pacing and a built in benchmark.
- In a few chapters the camera starts a pixel or two higher than on the console, because of how loading is timed.
- With motion blur on, its trail is shorter at high frame rates.

### Full changelog
- The Classic (Xenos emulation) renderer and `rexgpu-xenos.dll` are removed; the build always includes the native renderer. Old `kk_renderer`, `gpu_plugin` and `native_backend` values migrate to native Auto (Vulkan), and old startup recovery state is cleared. Startup recovery: the chosen API, then the other one, then a translated help message and a clean exit.
- At most two unfinished GPU frames; the wait for older frames happens just before submitting, outside the submit lock. First use pipelines are waited for instead of skipped; demanded pipelines take priority over background prewarm, and motion and scale variants are prepared during setup.
- SDK patch 0035 (Vulkan present semaphores per swapchain image), 0036 (AMD FSR luma history format) and 0037 (UI paint on demand: persistent dialogs repaint only for new game frames, input, changes and animation; `ui_paint_on_demand`).
- `native_diagnostics` (default off) gates per call API timing and detailed frame and ring telemetry. The frame timing summary every 600 frames, hitch logs and crash reports stay.
- Screen space GI (`native_gi`): a sunlit source term above the frame's ambient light, sources and receivers must face each other, a minimum sample distance, and each pixel's added light bounded by its own colour.
- Ann tessellation (`native-renderer/docs/tessellation.md`): Phong tessellation for Ann's identified draws only (shader, texture and geometry content keys), with the depth pass reading the paired lit pass's normals so depth, colour and motion vectors match. Its pipelines are prewarmed and its shaders are in the shader packs.
- Effects hidden for Beta 4: shadows, water reflections and light shafts (code kept). Ultra shadows are capped at twice the game's map size.

## v2.0.0-beta.3 (13 October 2026)

**Beta 3**: the third test build of the next big update, from the beta channel (**About** > **Change version**). Expect bugs, and please tell us how it runs on the [Discord](https://discord.gg/8eakYSvBZS). Your saves and settings are kept, and you can switch back to stable at any time. Windows only for now.

### What's new
- **The launcher is now the game's main menu.** **Play** opens a Play menu: **Continue** (at the top when you have a save), **New game**, **Load game** and **Chapter select**. The game then starts straight there: no logos, no title screen, no game menus. **Load game** lists your five saves to continue or delete. **Chapter select** has all 42 chapters open, with their names and pictures from your own copy of the game and your best scores. Before you play, and before a save is deleted or overwritten, your saves are copied to a backup folder (**About** > **Open save backups**). The "All chapters" cheat is gone: it isn't needed any more.
- **A new pause menu.** **Start** (or **Esc**) in play opens a pause menu in the launcher's style over the frozen game: **Resume**, **Start again**, **Score**, **Save** (in the story), **Options** (the game's own audio, video and controls options), **Settings**, **Return to main menu** (back to the launcher) and **Quit game** (to the desktop). **Settings** has the launcher's **Display**, **Graphics**, **Gameplay**, **Controls** (with remapping) and **Cheats** pages, with every setting that changes at once; the ones that need a restart stay in the launcher. Cheats now switch on when a chapter starts and follow changes made in play.
- **DLSS, FSR 3.1, FSR 4 and XeSS upscaling.** With the native renderer, **Display** > **Upscaling** > **Upscaler** now also offers NVIDIA DLSS, AMD FSR 3.1, AMD FSR 4 and Intel XeSS, next to FSR 1 and NIS. They draw fewer pixels and rebuild a sharp picture at your screen's resolution from several frames, smoothing jagged edges too. Only the ones your graphics card supports are listed: DLSS on NVIDIA RTX cards, FSR 4 on AMD Radeon RX 7000 and 9000 cards with Direct3D 12, XeSS and FSR 3.1 on almost any card. **Upscaler mode** picks how many pixels the game draws: **Native AA** (DLAA with DLSS), **Quality**, **Balanced**, **Performance** or **Ultra Performance**.
- **The best upscaler for your graphics card is now the default**, at **Quality**: DLSS (its newest model) on NVIDIA cards that support it, FSR 4 on AMD cards that run it, otherwise FSR 3.1, XeSS on Intel cards, FSR 3.1 on any other card that runs it, and FSR 1 where none of them runs. The graphics presets and the setup's **Remastered** use it too (**Low** at **Performance**). If you picked an upscaler yourself (FSR 1, NIS or Off), you keep it; if you never did, you get the new default. The **Original** look keeps them off. Choosing one restarts the game when you press **Play**. There is no frame generation.

- **Stronger ambient occlusion by default.** With the native renderer, GTAO's **AO strength** now defaults to **Strong**. A strength you chose yourself is kept.
- **Fixes.** What's new waits until the shaders an update rebuilds are ready, so the launcher stays responsive. On PCs with several monitors, **Monitor** now lists them in the order the game uses, so it matches **Resolution**. Borderless sizes the game for the monitor it's on, a saved resolution that's no longer available falls back cleanly, the Display page refreshes its lists each time it opens, and the Discord status reconnects if Discord stops answering. A beta you unzipped by hand now stays on the beta channel and gets the next beta. The launcher's newer text is translated into German, Spanish, French and Italian.
- **Credits.** **About** > **Upscaler credits** names NVIDIA DLSS, AMD FSR and Intel XeSS.

### Known issues
- The chapter select's last four entries (Venatosaurus, Empire State, Kong's death, Credits) have no picture of their own.
- FSR 4 is untested on the graphics cards that run it (we have none here); on other cards it isn't offered.
- The Vulkan renderer (the default) can't run FSR 4, only Direct3D 12 can.
- Coming in a later beta: screen space global illumination, sharper shadows, reflections on water and volumetric light shafts.
- In a few chapters the camera starts a pixel or two higher than on the console, because of how loading is timed.
- With motion blur on, its trail is shorter at high frame rates.

### Full changelog
- The Play menu and the pause menu (`native-renderer/docs/menus.md`): the launcher sets a start request (`play_session.h`: continue, new game or a chapter, with a save slot) and the port carries it out on the title screen in one frame, loading the save from its file and asking the game for the level, so the game's logos, title, save and main menus never show. Chapter select runs go back to the launcher's Chapter select after their Score pages; Return to main menu reopens the launcher; the game's own save routine can no longer delete a save (it did when a save's continue point was empty). The port's pause menu replaces the game's (`IntMIG_Page_Main`): the game stays paused, the last frame of play is kept behind the menu, the game gets an empty pad, and each item runs the game's own page. Settings that need a restart are hidden in it. `kk_cheat_chapters` is removed.
- The chapter names and pictures are read from the player's game files into the launcher's art cache: the start menu's text in KKMaps.bf (the BIG reader now follows its chained tables) and the chapter select's film frame textures in KKTextures.bf.
- `ao_strength` defaults to 3 with the native renderer (`ApplyRendererChoice`); presets and Remastered use it.
- SDK patch 0017 (`rex::ui::GetDisplays`): the Monitor row in the SDK's display order.
- `native_upscaler` (`off`, `dlss`, `fsr3`, `fsr4`, `xess`) and `native_upscaler_quality`, set by the launcher. Their defaults follow `kk_upscalers_offered`, the launcher's answer kept in the settings (until it has asked once, the card's vendor decides; the plugin's own default is `auto`, so the game's Vulkan device has the extensions for the vendor's upscaler from the first start). A settings file from an earlier version with `present_effect` in it (a choice made on the Upscaler row) gets `native_upscaler` `off`. NIS is a choice only now, no longer NVIDIA cards' default.
- `native_upscaler_dlss_preset` `latest` (the default): DLSS 4.5's preset M, and L for Ultra Performance. The renderer jitters the scene, makes motion vectors (the camera's from the depth, each moving object's from its own vertex shader run with this and last frame's constants) and a reactive mask for water, fur, smoke and particles, and runs the upscaler before the game's post effects and HUD where it can find them, at the end of the frame otherwise.
- The SDKs are loaded from beside the game: `nvngx_dlss.dll` (DLSS 310.9.1), `amd_fidelityfx_vk.dll` and `amd_fidelityfx_dx12.dll` (FSR 3.1.4), `amd_fidelityfx_loader_dx12.dll` and `amd_fidelityfx_upscaler_dx12.dll` (FSR 4.1.1), `libxess.dll` (XeSS 2.0.2). Their notices are in `THIRD-PARTY-NOTICES.txt`.
- The launcher asks the plugin which upscalers run on this PC (`nr_upscaler_query`, on a Direct3D 12 device of its own, at feature level 11_0 or higher, in the background; each SDK's own check decides). A chosen upscaler that can't run falls back to FSR 3.1, and the log says why.
- On Vulkan, the device is made with the extensions DLSS and XeSS need (`native-renderer/sdk-patches/0003`, which the beta's SDK build needs).

## v2.0.0-beta.2 (12 October 2026)

**Beta 2**: the second test build of the next big update, from the beta channel (**About** > **Change version**). Expect bugs, and please tell us how it runs on the [Discord](https://discord.gg/8eakYSvBZS). Your saves and settings are kept, and you can switch back to stable at any time. Windows only for now.

### What's new
- **Windowed, Borderless or Fullscreen.** **Display** > **Window mode** has three choices now. **Borderless** fills the screen with a window at your desktop's resolution, so Alt+Tab is instant. **Fullscreen** switches your screen to the **Resolution** and **Refresh rate** you pick while the game runs. The render quality follows the resolution you pick.
- **A fix for Alt+Tab not getting out of the game properly in fullscreen.** With some graphics drivers, Beta 1's fullscreen window was taken over as exclusive fullscreen. It should now stay a normal borderless window, as in the stable version. Thanks to GPUnity for the report.
- **Frame rate and upscaling are on the Display page now**, next to the resolution settings. **Graphics** keeps the look, the preset, antialiasing, texture filtering, ambient occlusion and the effects.
- **Discord status.** While Discord is running on your PC, your profile shows that you're playing King Kong, in the launcher or in the game, and for how long. **Discord status** on the **About** page switches it off.
- **GTAO is now the ambient occlusion.** With the native renderer, ambient occlusion is always the new GTAO, so the **AO method** choice is gone (if you picked Classic in Beta 1, you now get GTAO). **AO strength** is now **Light** (recommended), **Medium** or **Strong**: how dark the shading gets and how far it spreads.
- **Supersampling is gone.** Pick a **Resolution** instead. If you had it on, the game now draws at **Native**.
- The shaders are prepared again the first time you start Beta 2, as after every update.

### Known issues
- Coming in a later beta: DLSS, FSR 3 and 4, and XeSS upscaling, screen space global illumination, sharper shadows, reflections on water and volumetric light shafts.
- In a few chapters the camera starts a pixel or two higher than on the console, because of how loading is timed.
- With motion blur on, its trail is shorter at high frame rates.

### Full changelog
- Window mode: `fullscreen` off is Windowed; on, `kk_fullscreen_mode` `desktop` is Borderless and `WIDTHxHEIGHT@HZ` is Fullscreen, set as the game window's fullscreen mode when the game starts through the SDK (`rex::ui::GetFullscreenDisplayModes`, `GetDesktopDisplayMode` and `SetFullscreenDisplayMode`, SDK patch 0015). A mode the display doesn't have falls back to the closest one, or to Borderless.
- Vulkan swapchains on Windows are created with `VK_EXT_full_screen_exclusive` set to disallowed (SDK patch 0016), so the driver can't make a window that covers the screen exclusive by itself; `vulkan_allow_fullscreen_exclusive` gives it its say back.
- Discord status: Rich Presence through the Discord app's local connection (`discord-ipc-N`), without the Discord SDK; `kk_discord_status`, on by default. While Discord isn't running it tries again every 20 seconds.
- `native_ao_method` `gtao` and `native_ao_apply` `opaque` are the defaults; Beta 1's AO method in a settings file is reset.
- `kk_render_quality` `supersample` is no longer offered; a settings file with it gets `native`.

## v2.0.0-beta.1 (12 October 2026)

**This is a beta**: an early test build of the next big update, from the beta channel (**About** > **Change version**). Expect bugs, and please tell us how it runs on the [Discord](https://discord.gg/8eakYSvBZS). Your saves and settings are kept, and you can switch back to stable at any time. Windows only for now: on Linux, keep using the stable version through Proton.

### What's new
- **A renderer of our own.** The game is now drawn by a new native renderer on Vulkan, instead of an imitation of the Xbox 360's graphics chip. It draws the game the same way the console did, runs much faster, and draws at higher resolutions for real instead of upscaling a 720p picture. **Graphics** > **Advanced** > **Renderer** switches back to the **Classic** renderer to compare.
- **Shaders prepared from your own game files.** At the end of setup, and again after every update, the launcher prepares the game's shaders from your own disc files, which takes about a minute, so effects don't stutter the first time they appear. Your PC may run slower while it works. The **SHADERS** status on Home shows how it's going; select it to prepare them again if something looks wrong.
- **Original or Remastered.** The last step of setup asks how you'd like the game to look: **Original**, the Xbox 360 as it was (720p, 30 FPS, the console's look), or **Remastered**, sharper and at your screen's resolution with soft ambient occlusion and 60 FPS, set up for your graphics card. **Look** on the **Graphics** page changes it at any time.
- **New ambient occlusion.** With the native renderer, **AO method** on the **Graphics** page picks the new **GTAO**: sharper soft shadows in corners and creases that never darken water, smoke, fire, Kong's fur or the sky, and that now work in the chapters where the old one did nothing. **Classic** is the old one.
- **Higher frame rates done properly.** Things that were tied to the console's 30 FPS now run at the right speed at any frame rate: the lifeboat crew's rowing, particles, Kong's grabs, button mashing and finishers, knockbacks, and the splashes and ripples in water. At 30 FPS nothing changes.

### Known issues
- Coming in a later beta: DLSS, FSR 3 and 4, and XeSS upscaling, screen space global illumination, sharper shadows, reflections on water and volumetric light shafts.
- In a few chapters the camera starts a pixel or two higher than on the console, because of how loading is timed.
- With motion blur on, its trail is shorter at high frame rates.

### Full changelog
- `rexgpu-native`: a GPU plugin that hooks the game's Direct3D calls and draws with its own Vulkan (or Direct3D 12) renderer through NVRHI, on its own render thread, with the game's shaders translated from Xbox 360 microcode (XenosRecomp based) and its textures converted on upload. `kk_renderer` (`native` or `classic`) picks it; switching back to the stable channel resets it and every other beta only setting first.
- The shader pack is built by the launcher from the player's `Shaders/xeshaders.bin` into the user's cache (`native-shaders`), with a stamp so it is rebuilt exactly when needed; no translated shader ships. `dxcompiler.dll` (DirectX Shader Compiler, LLVM Release License) is included for that.
- `kkpipelines.list` lists the game's pipelines across all chapters, made in the background before the game starts; the driver's pipeline cache is kept between runs.
- GTAO: Intel's XeGTAO (MIT), applied before the game's fog pass so transparent effects are drawn over it.
- High frame rate fixes (`kk_hfr_fixes`, on): the rowing crew's animation time, particle drag, a precise frame clock, scripts that clamped the frame time to 0.016 s, per step decays, follow smoothing, footstep splashes and water ripples.
- What's new orders beta versions like the updater does.
- Beta players only get beta updates, plus the final stable release of their beta (a stable with the same or higher version numbers); a stable hotfix of an older version is not offered to them.

## v1.9.11 (11 October 2026)

### What's new
- **See how many people are playing.** The top right of the launcher shows how many players are online. **Online player count** is on by default: the setup screen explains it, players who already have the game are told once, and you can switch it off at any time on the **About** page (or with **Online players** on the setup screen). While it's on, the launcher sends a tiny anonymous check in every 5 minutes with just four things: a random number that isn't linked to you, the port's version (stable or beta), Windows or Linux, and whether you're in the game. No name, account, IP address or anything from your PC is kept, and switching it off deletes the random number.
- **A clearer setup screen.** A **Supported disc** card says exactly which disc image works: a full image (7,834,892,288 bytes) that you made from your own Xbox 360 disc of Peter Jackson's King Kong: The Official Game of the Movie, the USA and Europe release (title ID `555307D3`, version `0.0.0.1`), with its SHA-1 to check it against. An **Online players** card beside it explains the player count.
- **The Steam panel stays on Home.** Once the game is in your Steam library, it says **In your Steam library** and opens Steam, instead of disappearing.

### Full changelog
- `online.cpp`: a background check in (`POST /v1/heartbeat`) every 5 minutes, and at once when it's switched on or the game starts, to the port's own server (a Cloudflare Worker with a D1 database). Settings `kk_online_count` (on by default), `kk_online_asked` and `kk_install_id` (a random UUID, deleted when the count is switched off). Linux and Proton report as `linux`. Nothing is sent while it's off.
- `HttpPost` (WinHTTP on Windows, curl on Linux).

## v1.9.10 (11 October 2026)

### What's new
- **Fixed: the screen shaking nonstop in the V-Rex chapter** (#39). Above 30 FPS, the camera shake from the V-Rex's footsteps could stop fading out and keep going until you died. The shake now fades the same way at any frame rate, as it does on the Xbox 360 at 30 FPS. Thanks to MustopuhGgi for the report.
- **Camera shake settings.** **Camera shake** (on or off) and **Camera shake strength** (10% to 200%) are on the **Gameplay** page, for anyone who finds the shaking too much, or not enough.
- **The best upscaler for your graphics card, by default.** The launcher now checks which graphics card you have (your dedicated card if you have one, otherwise your integrated graphics). On an NVIDIA card the default is now **NVIDIA NIS** at Quality; on AMD, Intel or anything else it stays **AMD FSR 1** at Quality. If you've picked an upscaler yourself, it stays as you set it.
- **Every graphics preset uses that upscaler.** Low draws at Performance, Medium at Quality, and High, Ultra and Steam Deck at Native, where the upscaler only sharpens.
- **Supersampling is its own setting.** **Supersampling** on the **Graphics** page draws the game at more pixels than your screen has, for the cleanest edges. The Ultra preset no longer turns it on.
- **FXAA Extreme is gone.** It cost a lot for little gain. The **Ultra** preset now uses FXAA, and anyone who had Extreme on gets FXAA.
- **Join the official Discord.** A panel at the bottom right of the Home and setup screens (press right to reach it with a controller or the keyboard), and **Discord** on the **About** page: news, upcoming features, early builds, and a place to tell us what you think.
- **Add to Steam is a panel too**, beside the Discord one: it adds the game to your Steam library as a non Steam game, with its artwork, so you can start it from Steam. It's there until the game is in your library. Left and right move between the two panels, and each has its own button: **X** adds to Steam and **Y** opens the Discord (Del and R on the keyboard), shown in your button prompt style.
- **Button prompts that match your controller.** **Button prompts** on the **Controls** page has a new **Automatic** setting, now the default: an Xbox 360 controller shows Xbox 360 buttons, an Xbox One or Series controller shows Xbox Series buttons, a PlayStation 4 or 5 controller shows PlayStation buttons and a Steam Deck shows its own, in the game and in the launcher. Pick any style yourself and it stays as you set it.
- **Stable or beta.** **Change version** on the **About** page switches between the stable version and the beta, for early test builds of upcoming features. You'll see a warning first: betas can have bugs and support for them is limited. The warning has a **Join Discord** button too, to tell us how the beta runs. Your saves and settings are kept either way, and you can switch back at any time. There's no beta build yet; until there is, the beta gets stable versions.
- **Steam Deck button prompts.** **Button prompts** on the **Controls** page now has **Steam Deck**, in the game and in the launcher.
- **Steam style achievement popups.** **Popup style** on the **Achievements** page picks between the Xbox 360 popup and a Steam style one in the bottom right corner.

### Full changelog
- Camera shake: the camera's effects script, CM_Sfx (`sub_8245EC90`), and SFX_ShakeCam's SCC_SFX (`sub_82553F20`) multiply their fall rate once a frame, so above 30 FPS it wore away faster with every footstep until a shake could no longer reach zero. `camera_shake.cpp` keeps the falloff per second (the multiplier to the power of frame time x 30), so 30 FPS is unchanged. `kk_camera_shake` and `kk_camera_shake_strength` scale the shake's size for the call only, so a shake still lasts as long as the game meant.
- Upscaler default: the high performance DXGI adapter's vendor (`DetectGpu` in `platform.cpp`) picks `present_effect`: `nis` on NVIDIA, `fsr` otherwise. The card is written to the log at start. Every graphics preset uses the same upscaler (`auto` in the preset table), and Ultra's render quality is `native`; Supersampling sets `kk_render_quality` to `supersample`.
- `swap_post_effect` `fxaa_extreme` is moved to `fxaa`.
- Updates: `kk_update_channel` (`stable` or `beta`). Beta builds come from the separate `king-kong-recompiled-beta` repo, so the main Releases page only ever shows stable versions; the beta channel takes whichever of the two is newer. Versions like `2.0.0-beta.1` sort before `2.0.0`. Change version installs the other channel's newest version, older or newer.
- `kk_button_prompts` takes `auto` (the new default; `PadPromptsFor` in `glyphs.cpp` turns SDL's controller type into a style, for the game and the launcher) and `steam_deck`; the pictures are from Kenney's Input Prompts (CC0), built by `tools/make_glyphs.py --kenney`.
- `kk_achievement_style` (`xbox360` or `steam`).
- The Discord and Steam marks are from Simple Icons (CC0), built into the launcher by `tools/make_brand_icon.py`.
- New launcher text in German, Spanish, French and Italian.
- `rexruntime.dll` and `rexgpu-xenos.dll` are the same as in v1.9.5 to v1.9.9.

## v1.9.9 (10 October 2026)

### What's new
- **Stronger vibration if you want it** (#35). **Vibration strength** on the **Controls** page now goes up to 200%. The game's own rumble is gentle: each machine gun, revolver or sniper rifle shot runs the controller's big motor at about half speed, and the shotgun at about three quarters. At **160%** the other guns kick about as hard as the shotgun does at 100%, and the shotgun reaches the controller's full strength. 100% is still the default, as on the Xbox 360. Thanks to tedwards75 for the suggestion.

### Full changelog
- `kk_vibration_strength` takes 10 to 200; each motor's speed is capped at its maximum. The game's own values are now put back after each rumble call, so a vibration it sends again is never scaled twice.

## v1.9.8 (9 October 2026)

### What's new
- **Fixed: settings not being kept** (#37). If the path to the game's folder, or to your Windows user folder, had a letter like á, é or ñ in it, the game couldn't read its settings file (`king_kong.toml`), so every start went back to the default settings, the language included, and saved them over yours. The same problem could also stop achievement names and your unlocked achievements from loading. Thanks to ulyssesaot for the report and the video.

### Full changelog
- The exe's manifest makes UTF-8 its code page (Windows 10 version 1903 and later), so the runtime's text paths are UTF-8. It opens its settings and achievement files with toml++, which reads a path as UTF-8, and in the system code page a path with an accented letter couldn't be opened.
- The settings migration no longer saves over a settings file it couldn't read; it keeps a copy (`king_kong.toml.bak`) and logs an error.

## v1.9.7 (9 October 2026)

### What's new
- **Real mouse look.** The mouse now turns the camera directly: the same movement always turns it the same amount, however fast you move, as in other first person games. Before, the mouse was treated like a thumbstick, so small movements did nothing and fast ones were capped at the stick's top speed. **Mouse sensitivity** (1.00x is 0.066 degrees per count) and a new **Mouse vertical** setting (to invert it) are on the **Controls** page.
- **A modern keyboard and mouse layout.** Left mouse button shoots (and takes, uses and repels), right mouse button aims, R reloads, C or Ctrl crouches, middle mouse button or Z zooms, WASD moves, F calls your companion, Space checks your reserve bullets, G drops the spear and Esc pauses. As Kong: F hits, Space jumps, dodges and climbs, Q grabs and throws, G pushes back, bites and goes into fury mode. In the menus: Enter selects, Space or Backspace goes back and the arrow keys move around. Any key you changed yourself stays as you set it; **Reset to defaults** on the **Keyboard bindings** page gives you the whole new layout.
- **Mouse buttons can be bound.** The **Keyboard bindings** page now takes the left, right and middle mouse buttons, and each binding says what its button does in the game, for Jack and for Kong.
- **Choosing Keyboard & mouse sets things up.** It switches the game's button prompts to **Keyboard**, so hints show your keys and mouse buttons, and turns **Mouse camera** on. Going back to **Controller** brings your controller's prompts back.

### Full changelog
- Mouse look: `mouse_look.cpp` adds up the window's raw mouse movement and gives Jack's camera (CM_Cam, `sub_8246F180`) the angle the mouse moved through its yaw and pitch turns (`sub_82711950`, `sub_82712300`). CM_Cam only turns an axis whose stick is past its 15% deadzone, so while the mouse moves, its stick read is nudged just past it. The runtime's own mouse to stick path is kept at its lowest (`mnk_sensitivity` 0.01), where it never gets past that deadzone. While Jack aims, the game turns about 75% as far.
- Settings: `kk_mouse_sensitivity`, `kk_mouse_invert_y` and `kk_pad_prompts` (the controller prompts to bring back). The new `keybind_*` defaults and `mnk_mouse` on are set with the port's other defaults.
- Prompt pictures `LMB`, `RMB` and `MMB` in `glyphs/keyboard`.
- Launcher: key capture takes mouse buttons (a click on Cancel still cancels).

## v1.9.6 (9 October 2026)

### What's new
- **Play on Linux and Steam Deck now, through Proton (experimental).** This is not the native Linux version: it's the Windows download running through Steam's Proton, so Linux players can play while the native Linux version is finished. That will come in a future update. To play, add **king_kong.exe** to Steam as a non Steam game and set it to use **Proton Experimental** (the README has the steps). Tested on a ROG Ally (Z1 Extreme) with Bazzite: the launcher, installing from a disc image and the game all work, and it held 120 FPS on the 10 W power profile. The first start takes about half a minute while Steam sets Proton up for the game; after that it starts straight away. Under Proton the launcher uses your system's font, and **Add to Steam** is hidden, since the game is already in Steam.
- **Fixed: the game could freeze on the black LOADING screen** as the main menu loads. When a load starts, the loading screen and the game's frame presenter both need the graphics device, and if the loading screen got it first, each waited for the other forever. It happened often on Linux; on Windows the presenter nearly always got there first, but it could happen there too.
- **Fixed: one controller press counting twice in the launcher while Steam is open.** Steam's desktop controller layout also types keys for the controller's buttons (arrow keys, Enter), so the selection moved two places and switches flipped twice and stayed as they were. A press and the key Steam types for it now count once.
- **Fixed: the Home menu running into the status line in a small window.** Its entries now shrink to fit.

### Full changelog
- Loading screen: `loading_fix.cpp` hooks the present that the graphics device's worker thread makes each frame (`sub_8278D0C8`). While the loading screen thread (`sub_8278E450`) is up, it skips that present, since the loading screen draws every frame itself. Found by tracing the handshake between the two (`KK_DEV_LOAD_TRACE` in developer builds).
- Launcher input: `launcher_ui.cpp` reads the controllers before the keyboard and treats a key and a controller press of the same action within 0.25 s as one press.
- Proton: when Windows' fonts aren't there (Wine), the launcher loads Noto Sans (or DejaVu Sans, or Liberation Sans) from the Linux system through Wine's `Z:` drive. Proton plays videos it can't convert as colour bars, so stills that are colour bars are no longer used as backdrops; the launcher takes its artwork again once (a few seconds) to drop any it already took.
- Native Linux version (in development, not in this release): the launcher no longer swallows the window's paint events when it reads controllers, draws at 60 FPS (it used 72% of a CPU core), downloads with curl, takes its stills with ffmpeg and can update its AppImage. `tools/rexglue-patches` 0011 frees the game's memory however it ends (a crash or kill left it all in `/dev/shm` until a reboot), and 0012 adds the frame log to the Vulkan backend. The Vulkan renderer still draws some scenes wrong, which is why Linux goes through Proton for now.

## v1.9.5 (9 October 2026)

### What's new
- **A new launcher.** Rebuilt from scratch with a cleaner look, and it now works fully with a controller: move with the stick or D pad, change pages with the bumpers, and the button pictures follow your **Button prompts** setting (Xbox 360, Xbox Series, PlayStation 5, PlayStation 2 or keyboard). Keyboard and mouse work everywhere too, and the text is sharp at any Windows display scale.
- **The game's own artwork.** The launcher takes the King Kong logo and stills from the movie trailer from your copy of the game, and the Home screen shows them as a slow slideshow. Nothing from the game is in the download. On a new install the launcher starts on a setup screen: pick your disc image, it installs, prepares the artwork, then opens.
- **The game's menu music and sounds.** The main menu music plays in the launcher, and moving around makes the game's own menu sounds. Both come from your copy of the game, and each has a switch and a volume slider under **About > Launcher**.
- **Add to Steam.** A new **Add to Steam** entry on the Home screen adds the port to your Steam library as a non Steam game, with artwork from SteamGridDB, so you can start it from Steam, Big Picture or a Steam Deck. Steam closes for a moment while it's added and opens again afterwards. Steam Input is switched off for the game, so your controller keeps working.
- **Updates.** **Check for updates** is on the Home screen, and an update now downloads on a screen of its own with a progress bar.
- **The launcher in your language.** It follows the game's **Language** setting (English, French, German, Spanish or Italian), and the setup screen lets you pick it before you install.
- **New defaults.** New installs start with **AMD FSR 1** at **Quality**, and the **Medium** preset now uses it. If you're updating, your picture settings stay as they were. The **startup logos** are now skipped by default; turn them back on on the **Gameplay** page.
- **Shader preparing is always Balanced.** The setting is gone: Wait could pause the game for up to 2 seconds the first time an effect appeared, and Background could make objects vanish or flash.

### Full changelog
- Launcher: rewritten (`launcher.cpp` and a small toolkit in `launcher_ui.cpp`), with controllers read through SDL3 while it's open (let go before the game starts) and button pictures from the `glyphs` folder, plus the Xbox 360 set cut from the game's own button sheet.
- Sharp text: `rexruntime.dll` is rebuilt with `tools/rexglue-patches` 0010, which lets ImGui 1.92 bake its fonts at the size they're drawn.
- Artwork: `launcher_art.cpp` reads the front end's texture pack in `KKTextures.bf` (LZO1X chunks; the logo is a tiled 8:8:8:8 texture, the button sheet DXT5) and takes keyframes from `Video/Trailer.wmv` with Windows Media Foundation. It's kept in `Documents\king_kong\cache\art`.
- Music and sounds: from `Sound/Sound_Common.bf` through `SoundHeaders.db` (Xbox ADPCM, `game_sound.cpp`). The menu music is key 0x060002C9; the move, select and back sounds are 0x87003BF2, 0x87003BF4 and 0x87003D8A, the ones the game's main menu plays.
- Add to Steam: `steam_shortcut.cpp` adds the shortcut to `userdata/<account>/config/shortcuts.vdf` (binary VDF; every other entry is kept byte for byte) and sets `UseSteamControllerConfig` to 0 in `localconfig.vdf`, while Steam is closed, keeping a `.kk-backup` of each. The artwork is the top scored grid, wide grid, hero, logo and icon of SteamGridDB game 5249080.
- Settings: `kk_launcher_music`, `kk_launcher_music_volume`, `kk_launcher_sounds`, `kk_launcher_sounds_volume`. `kk_settings_version` 2 keeps the old upscaler defaults (`present_effect` bilinear, `kk_render_quality` native) for settings files from earlier versions. `kk_skip_intros` defaults to on. Shader preparing is fixed at Balanced (`async_shader_compilation` on, the default `async_shader_wait_ms`).
- Translations: the launcher's 370 lines of text in German, Spanish, French and Italian.

## v1.9.2 (9 October 2026)

### What's new
- **Fixed: Necropolis and Brontosaurus looking low resolution** (#32). Those levels turn on one of the game's own effects, a light blur over the whole picture (any other level that uses it is fixed too). The blur is sized for the Xbox 360's 720p, so whatever resolution you played at, those levels came out about as sharp as 720p. It's a new **Screen blur** setting on the **Graphics** page, off by default, so those levels are as sharp as the rest of the game. The **Original Xbox 360** look keeps it on. Thanks to GrummelFritz and leocmp for sticking with this one and sending logs and screenshots.

### Full changelog
- Screen blur: `kk_big_blur` (off by default). The game's "BigBlur" after effect averages four slightly offset copies of the frame, with offsets for 1280 x 720; off skips it. Found by turning the game's after effects off one at a time in Necropolis and identifying the draw it adds by its shader key.

## v1.9.1 (8 October 2026)

### What's new
- **Fixed: the whole game drawing at low resolution for some players** (#32). The game picks its picture size from the TV it thinks it's connected to, and the port was giving it your window size. Windows display scaling shrinks the size the launcher saves (a 1280 x 720 window at 125% is kept as 1024 x 576), and below 720 lines the game falls back to standard definition: 640 x 480, stretched to widescreen, even in fullscreen and whatever your **Render quality**. It now always sees a 720p TV, as on an Xbox 360, and **Render quality** scales that up as intended. Thanks to GrummelFritz and leocmp for the reports and the log.

### Full changelog
- Video mode: the port sets `video_mode_width` and `video_mode_height` to 1280 x 720 each time it starts (they aren't saved to `king_kong.toml`). Before, the runtime reported `window_width` and `window_height` as the console's video mode whenever a window size was set. The log now notes the video mode at startup (`KK: video mode 1280x720 (window ...)`).

## v1.9.0 (8 October 2026)

### What's new
- **Ambient occlusion.** A new setting on the **Graphics** page adds soft shading where surfaces meet: in corners and creases, and on the ground under rocks, grass and people. It's off by default. Choose strength 1 (recommended), 2 or 3. It stays under the game's distance fog, so far off walls don't show through the haze. It costs about 1 ms a frame at 4K, and works with NVIDIA and AMD graphics for now (not Intel yet) (#24).
- **AMD FSR 1 and NVIDIA Image Scaling.** A new **Upscaler** setting: **Off** (the default), **AMD FSR 1** or **NVIDIA NIS**. Both work on any graphics card. Each has **Native**, **Quality**, **Balanced** and **Performance** modes, and the launcher shows the resolution the game draws at in each one. They scale the picture up to your screen and sharpen it, so a lower resolution still looks crisp.
- **Original Xbox 360 or Modern.** A new **Look** setting at the top of the **Graphics** page. **Original** is the game as it was on the console: 720p at 30 FPS, the console's own antialiasing and texture filtering, the 69° field of view, motion blur and fog, no ambient occlusion and no upscaler. **Modern** gives you every setting, and switching back to it puts your own settings back.
- **Graphics presets.** **Low**, **Medium** (the default settings), **High**, **Ultra** and **Steam Deck** set the upscaler, render quality, antialiasing, texture filtering and ambient occlusion in one click. Change any of them yourself and it shows **Custom**.
- **Distance fog on or off.** **Off** clears the haze over far scenery. It's on by default, as the fog is part of Skull Island's look and also hides the edges of each area, so some empty backdrops can show with it off.
- **Performance draws one step lower.** The game draws in steps of its original 720p, so on a 4K screen **Performance** used to come out the same as **Quality** (1440p). It now goes one step lower where there is one: 720p at 4K, 2160p at 8K.

### Graphics presets
- **Low:** AMD FSR 1 in Performance mode, FXAA, 2x texture filtering.
- **Medium** (the default settings): no upscaler, Native, FXAA, 4x texture filtering.
- **High:** Native, FXAA, 8x texture filtering, ambient occlusion.
- **Ultra:** Supersample, FXAA Extreme, 16x texture filtering, ambient occlusion.
- **Steam Deck:** Native, FXAA, 2x texture filtering.

### Full changelog
- Ambient occlusion: built into the GPU plugin (`rexgpu-xenos.dll`, patches 0003 to 0007 in `tools/rexglue-patches`), set with `ao_mode` (0 off, 1 on) and `ao_strength` (1 to 3). Once a frame, at half resolution, it reads the scene depth the game copies out before lighting, works out the occlusion from 10 samples, blurs it and scales it up to full resolution. It's applied to the scene before the game's post effects, so the HUD isn't touched, and it uses the game's own fog values so only the part of the scene in front of the fog is darkened. Measured at 0.7 to 1.0 ms a frame at 3840 x 2160 on an RTX 4070 Ti. It needs the host render target path that NVIDIA and AMD graphics use; Intel graphics use a different path that isn't supported yet.
- Distance fog: `kk_fog` (on by default). Off skips the game's fog pass.
- Look: `kk_original_look`. While it's on, your Modern settings are kept in `kk_modern_settings`, and the Original values are set again each time the game starts in case the settings file was edited.
- Upscalers: `present_effect` (`bilinear` for Off, `fsr`, `nis`) and `present_nis_sharpness` (0.5). `rexruntime.dll` is now built from the ReXGlue SDK source with patch 0008: AMD FSR 1 and CAS without the AMD SDK, and the NVIDIA Image Scaling SDK v1.0.3 as a compute pass, run more than once for scale factors over 2x like FSR's own passes. The patch also fixes a heap in the presenter that only had room for two in between images, too few for FSR from 720p to 8K. Their MIT licences are in `THIRD-PARTY-NOTICES.txt`.
- Upscaler mode: sets `kk_render_quality` (`native`, `quality`, `balanced`, `performance`). While an upscaler is on, it replaces the **Render quality** setting. `performance` now goes one 720p step below `quality` where there is one, with or without an upscaler.
- Presets: the Graphics page shows which preset your settings match. Motion blur and fog aren't part of them.

## v1.8.0 (8 October 2026)

### What's new
- **Shader freezes cut from 2 seconds to under half a second.** A new **Balanced** choice for **Shader preparing** on the **Graphics** page, now the default for everyone. New effects are prepared on many threads at once in the background, and each frame waits a moment for them, instead of the game stopping while they're prepared one at a time. Once in a while an object can appear a moment late the first time it's seen. **Wait** and **Background** are still there if you prefer them. The numbers are below.
- **The shader pack looks after itself.** Each time the launcher opens, it downloads the newest shader pack if you don't have it yet. There's no button to click and nothing to remember. The Play page shows how it went, and if you press **PLAY** while a download is still going, the game starts as soon as it finishes.
- **Prepared effects are kept after a crash.** If the game closed while saving a newly prepared effect, everything prepared after that point was ignored and had to be prepared again. Now only the damaged entry is dropped, and the rest is kept.
- **New defaults.** FXAA is on, motion blur is off, **Controller sensitivity** is 150% and the **Stick deadzone** is 5%. The achievement sound is the Xbox 360's when you have `Xbox_360.wav` in the `sounds` folder; otherwise the built in chime plays as before. Settings you had changed from the old defaults stay as you set them.
- The "share your shaders" popup and the **Share my shaders** button are gone.

### Performance
Tested on the V-Rex chapter at 30 FPS on its first visit, with the graphics driver's own shader cache bypassed so every effect really was new.
- **Longest freeze:** 2.08 seconds with Wait (the old default), 0.45 seconds with Balanced.
- **All the freezes added up:** 3.9 seconds with Wait, 1.3 seconds with Balanced.
- **With shader pack 2:** the longest freeze was 0.33 seconds with Wait and 0.16 seconds with Balanced.

The freezes came as the chapter started, when the game meets the most new effects at once: up to about 240 in a second, each taking the graphics driver about 20 ms to prepare. In the minute of play after that, neither mode froze. Without a shader pack, Balanced drew about 80 objects a frame late in that minute; with shader pack 2, none.

### Full changelog
- Shader preparing: `async_shader_compilation` is now on by default with a new `async_shader_wait_ms`. A draw whose effect is still being prepared waits for it within a budget for each frame of half a frame at your frame rate cap (16 ms at 30 FPS, 8 at 60, 4 from 120 or unlimited), and is skipped only once that is used up. **Background** sets the wait to 0, and **Wait** turns preparing in the background off.
- GPU plugin: `rexgpu-xenos.dll` is now built from the ReXGlue SDK 0.10.0 source with the port's patch for that wait. The patch and build steps are in `tools/rexglue-patches`.
- Shader cache: at startup the port checks the cache's records and drops any that were only partly written, instead of the runtime ignoring everything after the first one.
- Shader pack: downloaded automatically each time the launcher opens, whether or not update checks are on. The **Shader pack update** popup and the **Download shader pack** button are gone. Shader pack 2 is still the latest; it covers about 40% of the game's effects, and Balanced handles the rest smoothly.
- Defaults: `swap_post_effect` fxaa, `kk_motion_blur` false, `kk_camera_sensitivity` 150, `kk_deadzone` 5, `kk_achievement_sound_file` `Xbox_360.wav` (the built in chime if that file isn't in the `sounds` folder). VSync off (`d3d12_allow_variable_refresh_rate_and_tearing`), 4x texture filtering (`anisotropic_override` 3) and the Modern camera response were already the defaults; the port now sets the first two itself.
- Launcher: the share shaders poster (`kk_share_poster`) and the **Share my shaders** button are removed.
- `tools/shader_coverage.py`: measures how many of the game's own effects (listed in its `Shaders/xeshaders.bin`) a shader cache has prepared.

## v1.7.3 (8 October 2026)

### What's new
- **Controller sensitivity speeds up the camera itself.** Before, the slider could only make smaller pushes of the stick reach the game's top turning speed: a full push turned just as fast whatever you chose. Now 200% turns twice as fast at every push, a full push included, both looking around and while aiming (#19).
- **Modern camera response, on by default.** A new **Camera response** setting on the **Controls** page. **Modern** turns the camera the same way in every direction, so circles and diagonals feel even. **Original** is the Xbox 360's own response, where small pushes up and down turn much more slowly than small pushes sideways. A full push straight across or straight up turns at the game's own top speed either way.
- **90 FPS.** The frame rate choices now include 90 FPS. They're a dropdown now, so every choice fits even in a small launcher window. As with any rate above 30, the launcher explains the animation issue when you press Play.

### Full changelog
- Controller sensitivity: scales the camera's own turn each frame (the turn the game's camera code applies left and right and up and down) instead of the stick, which the game caps at a full push. Keyboard and mouse still uses **Mouse sensitivity**.
- Camera response: `kk_camera_modern`, on by default. The game reads the right stick one direction at a time, with a 15% deadzone on each, then turns by the square of the push left and right and the cube of the push up and down. Modern reshapes the stick the camera reads so both follow the square of how far you push, in the direction you push. A half push now gives a quarter of the top speed in any direction; before, it was 17% left and right and 7% up and down.
- Frame rate: 90 added to `kk_frame_rate`'s choices, and the setting is a dropdown.

## v1.7.2 (7 October 2026)

### What's new
- **999 bullets lasts the whole chapter.** The **999 bullets** cheat was undone as soon as a chapter started: the game hands out each chapter's own weapons and ammo, so in V-Rex you got the machine gun's usual 30 rounds and nothing more. The port now gives the cheat again right after a chapter or checkpoint starts, so you really have 999 rounds. The weapon cheats (revolver, machine gun, shotgun and sniper rifle) had the same problem and are fixed too.

### Full changelog
- Cheats: about a second after a chapter or checkpoint hands out its loadout, the port gives the chosen weapon and ammo cheats again. Before, they were only given at the main menu.

## v1.7.1 (7 October 2026)

### What's new
- **The About page is always reachable.** Since the Cheats page was added in v1.6.0, the launcher's last page, **About**, was cut off the bottom of the page list when the launcher window was small (for example 1280 x 720), with no way to scroll to it. The list now shrinks to fit, so every page is always there.

### Full changelog
- Launcher: the page list's entries get shorter when the window doesn't have room for all of them at full height.

## v1.7.0 (7 October 2026)

### What's new
- **Field of view slider.** A new **Field of view** setting on the launcher's **Gameplay** page widens the view from 69° (the original) up to 110°. The game draws the wider area too, so nothing pops in at the edges. Jack's gun keeps its usual size, the menus keep their original look, and cutscenes widen by the same amount (#3).
- **Controller sensitivity works up and down too.** The **Controller sensitivity** slider on the **Controls** page (it was called Camera speed) now changes how fast you look up and down, not just left and right. A full push of the stick still turns at the game's own top speed. **Mouse sensitivity** and **Mouse camera** are now always shown on the Controls page.
- **Motion blur on or off.** A new **Motion blur** setting on the **Graphics** page. Off removes the ghost trail the game blends over fast moments, mostly in Kong's sequences and some transitions.
- **Skip the startup logos.** A new **Startup logos** setting on the **Gameplay** page skips the Ubisoft, Universal and WingNut movies, so the title screen appears within a few seconds. The story movies still play.
- **Reset a page.** Every settings page has a **Reset page** button that puts just that page's settings back to their defaults.
- **What's new and the changelog.** After an update, the launcher shows what changed the first time it opens. The **About** page now lists the notes for every version.

### Full changelog
- Field of view: `kk_fov`, in degrees for Jack's camera (69 is the original). Every camera is widened by the same amount, so close ups stay closer than wide shots. The area the game draws widens with it. Jack's gun is drawn with its own fixed angle and doesn't change. Nothing is widened while the menus show.
- Controller sensitivity: `kk_camera_sensitivity` now scales the right stick as the game reads it, instead of the raw stick, so both directions change together and it is no longer limited below a full push. It no longer applies to keyboard and mouse, which has its own **Mouse sensitivity**.
- Motion blur: `kk_motion_blur`, on by default as on the console. Off runs the game's motion blur effect at zero strength, the game's own way of having no blur.
- Startup logos: `kk_skip_intros`, off by default.
- Reset page: the Display, Graphics, Gameplay, Controls (including button remapping and keyboard keys), Cheats and Achievements pages each reset only their own settings. Press **Save** to keep them.
- Changelog: `CHANGELOG.md` is built into the launcher, and the launcher remembers the last version it ran (`kk_last_version`) to know what's new.

## v1.6.0 (7 October 2026)

### What's new
- **No more freezes with PlayStation, Xbox Series or keyboard button prompts.** If you picked any button prompts other than the original Xbox 360 ones, the game could freeze for 1 to 5 seconds at a time, again and again, even during the intro videos. The port was searching the game's memory for the button picture in a way that held up the graphics. It now finds it instantly, and the freezes are gone (#11).
- **Cheats page.** All ten of the game's cheats are now switches on the launcher's new **Cheats** page: all chapters, all bonus content, fast healing, one hit kills, 999 bullets, unlimited spears, and the revolver, machine gun, shotgun and sniper rifle. Switch them on, and they're active as soon as the game reaches its main menu. No more typing codes every time you play (#29).
- **Shader pack 2, and shader pack updates in the launcher.** A new shader pack covers more of the game, including the V-Rex fight and the damage effects. The launcher now checks for a newer shader pack when it opens, as well as for a newer version of the port, and offers whichever is out: they're released separately, so you're always on the latest of both.

### Full changelog
- Button prompts: the port looked for the game's button picture every second by asking Windows about each 64 KB of the game's 512 MB of memory. Each search took about 5 seconds and held the memory lock the graphics need. It now reads the emulator's own record of which memory is in use, so a search takes a few milliseconds.
- Cheats: the launcher's **Cheats** page (`kk_cheats`, `kk_cheat_*`). The port makes the same changes the game makes when a code is typed on its Cheat screen, when the main menu appears, and again each time you return to the main menu.
- Updates: the startup check (and **Check for updates now** on the About page) also checks the shader pack. Players with an older pack get a **Shader pack update** popup with **Download now** and **Later**; it waits until after a port update, since that restarts the launcher.
- Shader pack 2: 4,931 pipelines and 2,438 shaders (pack 1 had 4,652 and 2,362).
- For bug reports: a hidden setting, `kk_hitch_report_ms`, writes a report to the `logs` folder when a frame takes longer than that many milliseconds, showing what the game was waiting on. Off by default.

## v1.5.1 (7 October 2026)

### What's new
- **No more antivirus warnings.** Some antivirus programs, including Windows Defender, wrongly flagged a file that comes with the port (`rexruntime.dll`, part of the ReXGlue toolkit the port is built with) as a virus. It was a false alarm, but it could block the download. This release ships that file rebuilt from the exact same ReXGlue source, and antivirus programs no longer flag it.
- **Saving always works.** Some players were told to sign in before they could save, and the game never offered their save slots. This happened when the PC saw more than one controller, which is common with Steam Input, DS4Windows, or a handheld's built in controls. The game then treated the player as player 2, and only player 1 can save. Every controller, plus keyboard and mouse, now counts as player 1, so saving works whichever one you use (#9).
- **Rumble reaches your controller.** The game shakes the controller, for example when you fire a gun, but that rumble only went to the first controller Windows listed. With Steam Input, a wireless receiver or more than one pad, that wasn't always the one in your hands. It now reaches every connected controller, so you feel it whichever one you use.
- **Freeze reports.** If the game ever stops responding for 20 seconds, the port now writes a freeze report to the `logs` folder. If the game recovers, the log notes how long it froze. Attaching those files to a bug report lets us find what caused the freeze (#11).

### Full changelog
- Ships `rexruntime.dll` rebuilt from the same ReXGlue v0.10.0 source, unchanged. The official build was an antivirus false positive (rexglue/rexglue-sdk#485). `rexgpu-xenos.dll` is the official one, which was never flagged.
- Controllers: all controllers and keyboard & mouse now count as player 1. Before, each controller got its own player number in connection order, and the game refuses to save for players 2 to 4 because they have no profile. The game's rumble for player 1 now goes to every connected controller too.
- Freeze reports: a watchdog writes `logs\hang-<date-time>.txt` (where every part of the game was at that moment) and `logs\hang-<date-time>.dmp` when no frame has been drawn for 20 seconds. Time the PC spends asleep isn't counted.
- Crash and freeze reports can now be traced to the exact part of the game's code, so problems players report are quicker to fix.

## v1.5.0 (7 October 2026)

### What's new
- **The Language setting works.** Picking Deutsch, Español, Français or Italiano in the launcher now changes the game's language. Before, the game always came up in English whatever you chose (#6).
- **Toggle aim.** A new **Aim** setting on the launcher's **Controls** page: **Hold** (the default, as on the console) or **Toggle**. On Toggle, press the left trigger once to raise the gun and again to lower it, without holding it down. Pausing (Start or Back) lowers it. It works with keyboard and mouse too (#21).

### Full changelog
- Game language: the game asks the system which language to use, and the runtime always answered English. It now answers with the launcher's Language setting (English, German, French, Spanish or Italian, the languages on the disc).
- Controls: **Aim: Hold / Toggle** (`kk_toggle_aim`). Each press of the left trigger flips between released and fully held. Start, Back or unplugging the controller releases it.

## v1.4.0 (7 October 2026)

### What's new
- **Updates from the launcher.** When the launcher opens it checks whether a newer version of the port is out. If one is, it offers to update: **Update now** downloads it, replaces the port's own files and restarts the launcher, with your installed game, saves and settings left as they are. **What's new** opens the release notes. You can turn the check off, or check by hand, on the **About** page. From this version on, you won't need to download releases yourself.
- **Share my shaders.** The shader pack only covers what has been played so far, and you can help fill in the rest. Once you have played a good part of the game, open the **Play** page and click **Share my shaders**. It packs your shaders into one small file, shows it to you and opens a GitHub form to drop it into. The file holds only shader data: nothing personal, no saves or settings. Shaders that at least two players have sent go into the next shader pack, so everyone after you gets a smoother first playthrough.
- **Your country needs your shaders.** A popup when the launcher opens explains the above, with a poster to match. Tick **Don't show this message again** and it won't appear again.
- **MSAA is no longer a setting.** 2x MSAA is how the Xbox 360 drew those surfaces, so it now always matches the console, and the shader pack covers everyone.

### Full changelog
- Launcher: automatic update check at startup (About page: Updates, Off / At startup, plus **Check for updates now**), with **Update now**, **What's new** and **Later**. Updates are staged next to the old files and swapped in, so a failed download never leaves a half updated game.
- Launcher: **Share my shaders** on the Play page, and the **Share shaders** issue form on GitHub.
- Launcher: the "Players wants your shaders" popup at startup, with **Don't show this message again**.
- Launcher: the Multisampling setting is removed (2x MSAA stays on, as on the Xbox 360). The About page shows the port's version.
- New `tools/collect_shader_shares.py` builds the next shader pack from players' submissions, taking only records that at least two players sent.
- The poster is Alfred Leete's 1914 "Britons wants you" (public domain), relettered.

## v1.3.0 (6 October 2026)

### What's new
- **Shader pack: no more pauses for new effects.** The game prepares each effect (a shader) the first time it appears, which caused the short pauses many of you reported on a first playthrough. A test build has now played every chapter on its own and collected those effects into a shader pack. On the launcher's **Play** page, click **Download shader pack**: it is added to your shader cache, and every time the game starts it prepares everything in the cache before you play. Anything your own game has already prepared is kept.
  - Pack 1 holds 4,652 prepared effects from all 42 chapters. In a hand played test of a chapter, it already covered about 97% of what the game needed, and the rest showed up only as a few tiny pauses (under 75 ms, about two frames). Bigger packs will follow, and **Check for a newer pack** on the Play page fetches them without needing a new release of the port.
- **A warning before playing above 30 FPS.** Pressing Play with a frame rate above 30 (or unlimited) now explains that some animations can look wrong, and offers to play at 30 or 60 FPS instead. 30 FPS plays every animation correctly. At 60 the issues are still there but much less noticeable. This will be fixed in a future update.

### Full changelog
- Launcher: **Download shader pack** on the Play page. It downloads from the `shader-packs` release on GitHub and merges into `Documents\king_kong\cache`. The launcher only contacts GitHub when you click.
- Launcher: frame rate popup when pressing Play above 30 FPS, with Play at 30, Play at 60, Keep and Back.
- New `tools/make_shader_pack.py` builds a pack from one or more shader caches.
- Developer test build: the automatic tour that plays every chapter (cheats, chapter select, skip videos, walk around), used to build the pack. It is never in release builds.
- README: a section on the shader pack.

## v1.2.0 (5 October 2026)

### What's new
- **No more vanishing objects, silhouettes or bright flashes the first time you play.** While the game prepared a new effect in the background, it skipped drawing everything that needed it, so characters could turn into silhouettes for a moment and some frames flashed bright. Effects are now prepared before they are drawn. The first time you see something new there can be a short pause instead, and only once, because prepared effects are saved for next time. If you prefer the old behaviour, set **Graphics > Shader preparing** to Background.
- **Steadier frame pacing on more PCs.** The game now asks Windows for precise 1 ms timers and opts out of power throttling. Without this, Windows 10 and 11 can stretch the game's short waits to about 16 ms, and on laptops with Intel's newer mixed performance core and efficiency core processors Windows 11 can move the game onto the slower cores. Both cause stutter.
- **Better bug reports.** Every 10 seconds the log notes the average frame rate, the 1% low, the worst frame and how many frames took over 50 or 100 ms. If the game ever crashes, it writes `crash-<date>.txt` and `crash-<date>.dmp` into the `logs` folder. The README explains what to send.

### Full changelog
- Shaders now finish preparing before they are used (no skipped draws). The new launcher setting **Graphics > Shader preparing** switches between Wait (default) and Background.
- 1 ms timer resolution and Windows power throttling opt out at startup. The log records how long a 1 ms sleep really takes.
- Frame time summary in the log every 10 seconds.
- Crash reports (`.txt` and minidump) in the `logs` folder.
- New developer test build (`kk-dev` preset, never in releases). It can skip straight into gameplay and walk around on its own, for testing performance.
- README: a new "Reporting a problem" section, and updated known issues.

## v1.1.0 (5 October 2026)

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

## v1.0.1 (5 October 2026)

### Fixed
- **Render quality presets now change the resolution** ([#1](https://github.com/TekRantGaming/king-kong-recompiled/issues/1)). Every preset (Native, Quality, Supersample and so on) was rendering at the original 720p. Only Custom worked. Presets now render at the resolution shown next to them in the launcher.

## v1.0.0 (5 October 2026)

### What's new
- The game plays past the "VENTURE" opening, which freezes on Xenia, with working music and voice lines
- A launcher that installs the game from your disc, with art from your copy of the game
- Up to 8K resolution, FXAA, 2x MSAA and 16x texture filtering
- 30 fps like the console, or up to 240 and unlimited (some animations are not right above 30 yet)
- Xbox 360 style achievement popups, with your own choice of sound
- Camera inversion and speed, deadzone, vibration, button remapping, keyboard and mouse
