# Making mods for King Kong Recompiled

King Kong Recompiled has three kinds of mods: **texture packs**, **settings mods** and **cutscene mods**.
Players switch them on and off in the launcher's **Workshop**: texture packs in its **Texture packs** row
(only one texture pack is on at a time), settings and cutscene mods in its **Mods** row (many at once, in load
order). Nothing in a mod changes the game's own files, and switching a mod off brings everything back
exactly as it was.

Guides and the modding tools (texture extractor, pack checker) are on the game's website:
https://kong.tekrantgaming.com/mods (also **Build your own mods and texture packs** in the Workshop).

Not supported (for now): replacing records inside the game's archives (the .bf files), new models, levels
or sounds, and code mods (scripts, DLLs, programs). A mod folder that contains anything else is simply
ignored apart from the files described here.

## Where mods go

```
<user folder>/mods/<ModName>/mod.toml      your mods (one folder each)
<game folder>/mods-builtin/<ModName>/      the mods that ship with the game ("Built-in")
```

The user folder is `Documents\king_kong` on Windows and the game's user data folder on Linux
(the launcher shows the full path under the Kong mods row, and **Open mods folder** in Workshop > Mods
opens it). The launcher creates the `mods` folder the first time the Workshop opens.

The folder name is the mod's id. It must not start with a dot or contain `|` or `:`.

### Installing from a ZIP file

The Workshop's **Import texture pack** and **Import mod** buttons install a ZIP file for the player, after
checking it. The ZIP holds one of these:

```
MyPack.zip                                Import texture pack
  My Pack/pack.txt, *.dds                 a texture pack: pack.txt and its images in one folder
MyPackMod.zip                             Import texture pack
  My Pack/mod.toml, picture.png           a texture pack mod (type = "texture-pack")
  My Pack/textures/pack.txt, *.dds
MyMod.zip                                 Import mod
  My Mod/mod.toml, ...                    a settings or cutscenes mod
```

One mod per ZIP; no folders inside `textures`. A ZIP that does not fit (no mod.toml, no pack.txt, a mod given
to the texture pack button or the other way round) is refused with the reason, and nothing is installed. A
texture pack without a mod.toml gets one, named after its folder (or the ZIP). Installing the same mod again
(same folder name and same `name`) replaces it.

## mod.toml

A plain text file in [TOML](https://toml.io) format, at most 64 KB.

| Key | Needed | What it is |
|---|---|---|
| `name` | yes | The name on the tile, up to 80 characters |
| `type` | yes | `texture-pack`, `settings` or `cutscenes` |
| `author` | no | Who made it |
| `version` | no | The mod's own version, shown on the tile ("1.0") |
| `description` | no | Shown on the mod's page (up to 2000 characters) |
| `picture` | no | A PNG or JPEG file in the mod folder (16:9 looks best, at most 8 MB). Without one the tile shows the name on a plain panel |
| `min_game_version` | no | The oldest game version the mod works with, for example `"2.0.0-beta.7"` |
| `max_game_version` | no | The newest game version the mod works with (inclusive) |
| `[settings]` | settings mods | A table of `setting = value` lines |

Game versions compare by their numbers; a beta comes before the release of the same number
(`2.0.0-beta.7` is older than `2.0.0`). Outside the range, the mod shows a problem and cannot be switched on.

## Texture pack mods

A texture pack in a mod folder. The pack itself is exactly the format in
[native-renderer/docs/texture-packs.md](native-renderer/docs/texture-packs.md): a `pack.txt` manifest
and the replacement images beside it. Put them in a `textures` subfolder (or straight in the mod folder).

```
mods/Sharper Jungle/
  mod.toml
  picture.png
  textures/
    pack.txt
    0123456789ABCDEF__bf_12345678_0000000C.png
    ...
```

```toml
name = "Sharper Jungle"
author = "You"
version = "1.0"
description = "Hand painted leaves and rocks for the jungle chapters."
picture = "picture.png"
type = "texture-pack"
min_game_version = "2.0.0-beta.7"
```

Make the pack with the extractor and check it with `kknr_packcheck` as texture-packs.md explains. Texture
packs only work with the native renderer (the default).

Several packs at once: the player's own texture pack (Settings > Graphics > Texture pack, for example the
HD pack) loads first, then each enabled texture pack mod in load order. When two packs replace the same
texture, the one later in the load order is used, and the Workshop shows a conflict on both mods.

## Settings mods

A list of game settings that are used while the mod is on. They go **over** the player's own settings for
that run only: they are never written to the player's settings file, so switching the mod off gives back
exactly what the player had. If the player changes one of those settings in the game while the mod is on,
the player's new choice is kept. Settings given on the command line always win over mods. The Original look
(Settings > Graphics) still wins over graphics settings from a mod.

```toml
name = "Wide view"
author = "You"
version = "1.0"
description = "A wider field of view and no camera shake."
type = "settings"

[settings]
kk_fov = 85
kk_camera_shake = false
```

Values: `true` / `false`, whole numbers, decimal numbers or "text", matching the setting's type. A setting
the game does not know, a value of the wrong type or out of range, or a setting mods may not change is
skipped (the mod's page lists it); the rest of the mod still works.

Useful settings:

| Setting | Values | What it does |
|---|---|---|
| `kk_hide_hud` | true / false | Hides the game's HUD all the time (the weapon stays). F10 still hides everything for screenshots |
| `kk_no_healing` | true / false | Jack's health does not come back on its own (not active yet in this version) |
| `kk_fov` | 50 to 110 | Field of view |
| `kk_frame_rate` | 30, 60, 90, 120, 144, 165, 240, 0 (no limit) | Frame rate limit |
| `kk_motion_blur` | true / false | The game's motion blur |
| `kk_fog` | true / false | The distance fog |
| `kk_big_blur` | true / false | The full screen blur some levels use |
| `kk_camera_shake` | true / false | Camera shake |
| `kk_camera_shake_strength` | 10 to 200 | Camera shake strength in percent |
| `kk_vibration` | true / false | Controller vibration |
| `kk_ps2_cutscenes` | true / false | The restored PS2 cutscenes (when they are installed) |
| `native_cel_shading` | true / false | Cel shading (ink outlines and flat light bands) |
| `native_cel_outline` | 0.5 to 4 | Cel shading: outline thickness |
| `native_cel_darkness` | 0 to 1 | Cel shading: outline darkness (1 is black) |
| `native_cel_bands` | 2 to 4 | Cel shading: number of light bands |
| `native_cel_strength` | 0 to 1 | Cel shading: how strong the bands are |
| `native_sharpness` | off, low, medium, high | Sharpening after the upscaler |
| `native_gi` | off, low, medium, high | Global illumination |
| `native_ao_samples` | 1 to 3 | Ambient occlusion quality |

Any other setting in `king_kong.toml` works the same way. Settings mods may not change where files are,
the online and update switches, the setup's own records or the mod list: `kk_mods`, `kk_install_id`,
`kk_online_count`, `kk_online_asked`, `kk_check_updates`, `kk_update_channel`, `kk_discord_status`,
`kk_discord_app_id`, `kk_setup_version`, `kk_settings_version`, `kk_hardware_tier`, `kk_ps2_movie_folder`,
`native_texture_pack_folder`, `game_data_root`, `user_data_root`, `cache_root`, `log_file`,
`kk_achievement_sound_file`, `gpu_plugin`, `kk_launcher`, `kk_skip_launcher`, `kk_start`, `kk_renderer`,
`kk_launcher_open` and the benchmark settings.

The built-in mod is a settings mod: **Cel shading** (`native_cel_shading`; the fine controls are in the F8 panel).

## Cutscene mods

Video clips that play instead of the cutscenes between chapters (the restored PS2 cutscenes). Put them in a
`movies` subfolder with the same file names as the PS2 clips:

```
mods/My Cutscenes/
  mod.toml
  movies/
    BK_1B17.mp4
    BK_1683.mp4
```

```toml
name = "My Cutscenes"
author = "You"
version = "1.0"
description = "New clips before V-Rex and Ann."
type = "cutscenes"
```

Clips are H.264 video with AAC sound in an MP4 file, at most 4096x4096 and 60 seconds (as the PS2 clips).
A clip from a mod plays even when the PS2 cutscenes are switched off; the clips the mod does not have come
from the PS2 cutscenes only when those are on. File names are matched ignoring upper and lower case.

| Clip | Plays between |
|---|---|
| BK_8943.mp4 | the story start and The venture (not shown at the very first start) |
| BK_9BB9.mp4 | Skull Island and Necropolis |
| BK_1976.mp4 | Necropolis and Scorpions |
| BK_45B1.mp4 | Scorpions and The wall |
| BK_975D.mp4 | The wall and Sacrifice |
| BK_34E8.mp4 | Sacrifice and On Kong's tracks |
| BK_8887.mp4 | On Kong's tracks and Hayes |
| BK_1B17.mp4 | Hayes and V-Rex |
| BK_1683.mp4 | V-Rex and Ann |
| BK_EBEF.mp4 | Ann and Kong |
| BK_2001.mp4 | Kong and The canyon |
| BK_5EFA.mp4 | The canyon and Millipedes |
| BK_1793.mp4 | Millipedes and Brontosaurus |
| BK_2144.mp4 | Brontosaurus and Jimmy |
| BK_B3B3.mp4 | Jimmy and On the raft |
| BK_BECA.mp4 | On the raft and Rapids |
| BK_EABC.mp4 | Rapids and Fight |
| BK_1E3B.mp4 | Fight and Swamps |
| BK_F858.mp4 | Swamps and Chased by V-Rex |
| BK_4D69.mp4 | Chased by V-Rex and The log |
| BK_018C.mp4 | The log and The Skull islanders |
| BK_54C2.mp4 | The Skull islanders and To save Ann |
| BK_99AC.mp4 | To save Ann and The cave |
| BK_373A.mp4 | The cave and Venatosaurus |
| BK_47D3.mp4 | Venatosaurus and In the mud |
| BK_9D63.mp4 | In the mud and Call Kong |
| BK_0213.mp4 | Call Kong and Kong to the rescue |
| BK_2CF8.mp4 | Kong to the rescue and To the plane |
| BK_03EB.mp4 | To the plane and To the lair |
| BK_16AF.mp4 | To the lair and Kong's lair |
| BK_0C5A.mp4 | Kong's lair and Fight in the lair |
| BK_1B6F.mp4 | Fight in the lair and Free! |
| BK_57D6.mp4 | Free! and Chased by Kong |
| BK_60E9.mp4 | Chased by Kong and Heading back |
| BK_4F21.mp4 | Heading back and Back to the village |
| BK_48EF.mp4 | Back to the village and Kong's capture |
| BK_C697.mp4 | Kong's capture and Kong struck down |
| BK_FEDB.mp4 | Kong struck down and In the streets of New York |
| BK_0105.mp4 | In the streets of New York and The Empire State Building |
| BK_CD24.mp4 | The Empire State Building and Kong's Death |

Clips play when the story moves on to the next chapter, not when a chapter is started from the chapter
select or replayed.

## Load order and conflicts

Mods that are switched on form the **load order**: a mod you switch on goes to the end, and **Load earlier**
/ **Load later** on its page move it. When two mods of the same kind change the same thing, the one later in
the load order wins:

- two texture packs that replace the same texture,
- two settings mods that set the same setting,
- two cutscene mods with a clip of the same name.

Both mods then show **Conflict** on their tiles, and their pages say what they share. Mods of different
kinds never conflict.

Changes in the Workshop apply the next time the game starts.

## When something is wrong

A mod with a problem shows **Problem** on its tile and cannot be switched on; its page says what is wrong
(no mod.toml, a TOML mistake with its line, no name, an unknown type, no `textures/pack.txt`, no
`movies/*.mp4`, an empty `[settings]` table, or the wrong game version). A mod that was switched on and later
breaks (or is deleted) is skipped when the game starts; the game always starts. The log (`logs/king_kong.log`)
has a `KK mods:` line for every mod found and every setting applied.
