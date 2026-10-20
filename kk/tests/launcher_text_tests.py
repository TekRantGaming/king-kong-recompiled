"""Task 144 source checks for the launcher, no game or window needed:
- every settings heading and row in settings_layout.h, and the launcher's new page and shader texts, has a
  German, Spanish, French and Italian translation (placeholders kept, no em dashes);
- the online player count reminder can never open by itself (owner, 16 Oct): no code opens it;
- the shader screen shows one bar with one label: no step text is drawn;
- every bonus is unlocked (owner, 18 Oct): the bonus video modes are never locked, Extras reads no save and has
  no save row, and the Artbook is its first item;
- job J2: setup steps 3 and 4 (texture pack, PS2 cutscenes), their pop-ups and the Settings rows;
- job J3: the notification switch (Settings > Advanced > Downloads) and the ding/notification calls;
- job J8: the Workshop page's text and its Home entry; task 209 (owner, 18 Oct, replacing the menu of three): the
  Workshop is one page with three rows (Texture packs, Mods, Other TRG projects and games), the two import buttons,
  the "Build your own" link, the UHD note before its download, and one texture pack on at a time;
- job J6: the look step gone, new installs Remastered, the New in this update screen;
- Settings' tabs are Display, Graphics, Audio, Controls, Gameplay, Cheats, Advanced, About (owner, 16 Oct
  evening), the pause menu's the same without About; Home's About entry is Benchmark (owner, 18 Oct);
- the frame rate warning pop-up is gone (owner, 18 Oct).
Usage: launcher_text_tests.py <kk folder>"""
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
src = root / 'src'
failures = []


def fail(message):
    failures.append(message)


# All translation tables the launcher includes.
table_text = ''
for name in ('settings_help_text_table.inc', 'launcher_text_table.inc', 'debug_overlay_text_table.inc', 'benchmark_text_table.inc',
             'hdr_text_table.inc', 'launcher_layout_text_table.inc', 'workshop_text_table.inc',
             'artbook_text_table.inc', 'mods_text_table.inc', 'picture_text_table.inc', 'texture_packs_text_table.inc',
             'setup_pictures_text_table.inc', 'photo_text_table.inc', 'pause_text_table.inc'):
    table_text += (src / name).read_text(encoding='utf-8') + '\n'
STRING = r'"((?:[^"\\]|\\.)*)"'
entries = {}
for m in re.finditer(r'\{\s*' + r'\s*,\s*'.join([STRING] * 5) + r'\s*\}', table_text):
    en, *rest = (bytes(g, 'utf-8').decode('unicode_escape').encode('latin-1').decode('utf-8') for g in m.groups())
    entries.setdefault(en, rest)

layout = (src / 'settings_layout.h').read_text(encoding='utf-8')
needed = set(re.findall(r'\{"([^"]+)", "([^"*][^"]*)"\}', layout))
strings = {s for pair in needed for s in pair}
layout_pages = re.findall(r'"([^"]+)"', re.search(r'kPageNames\[kPageCount\] = \{([^}]*)\}', layout).group(1))
strings |= set(layout_pages)
strings |= {
    # Home and its pages, the Play menu's Extras
    'Settings', 'Achievements', 'About', 'Benchmark', 'Extras', 'Quit', 'Every bonus is unlocked.', 'Bonuses',
    'No save yet', 'Reinstall game', 'Install game',
    'Copies the game\'s files again from your disc image. Your saves and settings stay as they are.',
    # the shader screen's one bar, Rebuild shaders
    'Preparing the shaders', 'Preparing shaders', 'Rebuilding the shaders', 'Checking', 'Ready', 'Preparing {0}%',
    'Not prepared', 'Not prepared (see the log)', 'Rebuild', 'Rebuild shaders', 'The shaders are ready.',
    # task 168: Rebuild says it prepares everything again, even what the setup kept
    'Prepares all of the game\'s shaders again from the start, for Vulkan and DirectX 12, even the ones already '
    'prepared. It takes a few minutes, longer than the setup, which keeps shaders that are already prepared. It '
    'can help after a graphics driver update, or if the game shows graphics problems.',
    'Prepare all of the game\'s shaders again from the start, even the ones already prepared? This takes a few '
    'minutes. Your PC may run slower until this finishes, because preparing the shaders uses its processor.',
    'Continue in the background', 'Menu music', 'Menu music volume', 'Switch Menu music on to set its volume.',
}
# Old movie filter / Horizontal flip (Graphics > Extras): always unlocked since every bonus is (owner, 18 Oct;
# task 159 had the help text follow the save's unlock state).
LOCKED_HELP = 'An original bonus video mode. Unlock it in Extras before changing it.'
UNLOCKED_HELP = 'An original bonus video mode, unlocked in Extras.'
strings |= {UNLOCKED_HELP}
# Content downloads (task J1): the Settings rows, their values, help, errors and status lines.
strings |= {
    'HD texture pack',
    'Import texture pack ZIP',
    'Remove HD texture pack',
    'Restored PS2 cutscenes',
    'Remove PS2 cutscenes',
    'Download',
    'Waiting',
    'Starting',
    'Paused',
    'Installed',
    'Try again',
    'Removing',
    'Choose ZIP',
    'Cancel download',
    'Remove',
    'Downloads the HD texture pack ({0} GB) and turns it on. It needs {1} GB of free space while it installs.',
    'Downloads the upscaled PS2 cutscenes that the Xbox 360 version left out ({0} GB) and turns them on. It needs {1} GB of free space while it installs.',
    'Downloading. Select to pause. It continues where it stopped, also after closing the launcher.',
    '{0} MB/s',
    'Less than a minute left',
    'About {0} min left',
    'Paused. Select to continue the download.',
    'Checking the download.',
    'Unpacking the files. This takes a few minutes.',
    'Installed: {0}. Turn it on or off with Texture pack above.',
    'Installed. Turn them on or off with Play PS2 cutscenes above.',
    'Removing.',
    'Already have a texture pack as a ZIP file? Choose it and the launcher unpacks it, checks it and turns it on. Your ZIP file is kept.',
    'A download or install is running.',
    'Stops the download and deletes what was downloaded so far.',
    'Deletes the installed texture pack and turns Texture pack off.',
    'Deletes the restored cutscenes and turns Play PS2 cutscenes off.',
    'Wait until it has finished.',
    'Not installed.',
    'The connection was lost. Select to continue the download.',
    'The download server did not answer as expected. Try again later.',
    'Not enough free space. It needs {0} GB and the drive has {1} GB free.',
    'The download did not pass its check and was deleted. Select to download it again.',
    'This is not a texture pack the game can use.',
    'The download is not usable.',
    'The files could not be unpacked. Select to try again.',
    'Your user folder could not be written to. Check that the drive is not full.',
    'Another download is running. Try again when it has finished.',
    'It is not a ZIP file.',
    'It has file names that are not safe to unpack.',
    'It has no pack.txt.',
    'It has more than one pack.txt.',
    'The pack folder has folders inside it.',
    'A file in it is too large.',
    'Its pack.txt does not start with KKTP 1, or a line in it is broken.',
    'A file listed in its pack.txt is missing.',
    'It has no cutscene files.',
    'The HD texture pack is installed and turned on.',
    'The PS2 cutscenes are restored and turned on.',
    'Removed.',
}
# Content downloads (task J2): setup steps 3 and 4, the pop-ups, the strip and the Settings rows.
strings |= {
    # Settings > Graphics > Texture pack, Settings > Advanced > Cutscenes
    'Texture pack',
    'Replaces the game\'s textures with the sharper ones from the installed texture pack. You can switch it at any time, also from the pause menu, and it changes straight away.',
    'No texture pack is installed. Install one in the launcher\'s Settings > Graphics.',
    'No texture pack is installed. Download the HD texture pack or import a ZIP below.',
    'Installed',
    'The texture pack in your user folder that Texture pack uses.',
    'Your texture pack ({0})',
    'Your own folder',
    'None',
    'Download HD texture pack',
    'Import ZIP',
    'Remove texture pack',
    'PS2 cutscenes',
    'Download PS2 cutscenes',
    'Plays the restored PS2 cutscenes in the game, between the missions where the Xbox 360 version has none. Download them from the main menu\'s Settings > Advanced. You can switch them at any time.',
    'Plays the restored PS2 cutscenes in the game, between the missions where the Xbox 360 version has none. Download them below, or choose a folder with your own BK_xxxx.mp4 files. You can switch them at any time.',
    'Downloading. Select to see the progress, pause or cancel. It continues where it stopped, also after closing the launcher.',
    'Paused. Select to continue or cancel the download.',
    'Installed. Turn them on or off with PS2 cutscenes above.',
    'Deletes the restored cutscenes and turns PS2 cutscenes off.',
    # setup steps 3 and 4
    'HD texture pack',
    'Sharper versions of the game\'s textures, made at four times their original size. It is a separate download of {0} GB and takes {1} GB once installed. You can turn it on or off at any time in Settings > Graphics.',
    'Restore the PS2 cutscenes?',
    'The PS2 version of King Kong has cutscenes between some missions that the Xbox 360 version left out. These are upscaled versions of them, and the game plays them where they belong. It is a separate download of {0} GB. You can turn them off at any time in Settings > Advanced.',
    'It downloads in the background: the setup goes on, and you can use the launcher meanwhile.',
    'I already have a texture pack',
    'Skip',
    'Yes, restore them',
    'No thanks',
    'Download',
    'Installed size',
    'Space needed',
    '{0} GB while it installs',
    'Free space',
    '{0} GB',
    'Unknown',
    'It goes into your user folder. Remove it at any time in Settings.',
    # Settings > Advanced > Diagnostics: what happens to crash reports (task 318: the text report is sent with
    # Online player count on; the minidump stays on the PC)
    'Each crash saves a text report and a minidump on this PC. With Online player count on, the text report is sent, with your user name and personal folders removed. The minidump stays on this PC; it may contain private process memory, so review it before sharing.',
    # Progress, finished and error pop-ups, the strip (no "This can take a while" question, owner 19 Oct)
    'Watch progress',
    'Pause',
    'Continue',
    'Run in background',
    'Cancel',
    'Installing your texture pack',
    'Downloading the HD texture pack',
    'Downloading the PS2 cutscenes',
    'Your ZIP file is checked and unpacked. It carries on in the background if you leave this pop-up.',
    'It carries on in the background if you leave this pop-up. If the connection drops or you close the launcher, it continues from where it stopped.',
    'Waiting for the other download to finish',
    'Starting',
    'Connecting',
    'Downloading',
    '{0} of {1} GB',
    'Paused at {0} of {1} GB',
    'Paused',
    'Checking the download',
    'Unpacking the files',
    'Removing',
    'Stopped',
    'Cancel the download?',
    'What was downloaded so far is deleted. You can download it again at any time.',
    'Cancel download',
    'Keep downloading',
    'Download cancelled.',
    'Remove the texture pack?',
    'Remove the PS2 cutscenes?',
    'This deletes the installed texture pack from your user folder and turns Texture pack off. You can download or import it again at any time.',
    'This deletes the restored cutscenes from your user folder and turns PS2 cutscenes off. You can download them again at any time.',
    'Remove',
    'Keep it',
    'Texture pack installed',
    'HD texture pack installed',
    'PS2 cutscenes restored',
    'Your texture pack is installed and turned on. You can turn it off at any time in Settings > Graphics.',
    'The HD texture pack is installed and turned on. You can turn it off at any time in Settings > Graphics.',
    'The PS2 cutscenes are restored and turned on. They play in the game where the Xbox 360 version has none. You can turn them off at any time in Settings > Advanced.',
    'OK',
    'Download interrupted',
    'The connection was lost. What was downloaded so far is kept, so the download continues from where it stopped.',
    'Continue download',
    'Download check failed',
    'The downloaded file did not match the original, so it was deleted. This can happen when a connection damages the download. Download it again?',
    'Download again',
    'Not enough free space',
    'It needs {0} GB of free space on the drive with your user folder while it installs, and the drive has {1} GB free. Free some space, then try again.',
    'Texture pack not installed',
    'Download failed',
    'Try again',
    'Later',
    '{0} more waiting',
    'Select',
}
# Job J3: Settings > Advanced > Downloads (the section and row come from settings_layout.h), and the
# pop-ups for Play while a download runs or an install finishes.
strings |= {
    'Your download will pause while you play', 'Play now', 'Stay here', 'Pausing the download',
    'The game starts in a moment.', 'Finishing the install', 'Finishing',
    'The HD texture pack carries on downloading when you come back to the launcher. What was downloaded so far is kept.',
    'The PS2 cutscenes carry on downloading when you come back to the launcher. What was downloaded so far is kept.',
    'The HD texture pack and the PS2 cutscenes carry on downloading when you come back to the launcher. What was downloaded so far is kept.',
    'The download has finished and is being unpacked. That cannot be paused safely, so the game starts as soon as it is done.',
    'Shows a system notification when the HD texture pack or the PS2 cutscenes finish downloading, or stop, while the launcher is minimised or behind other windows. The sound that plays with it follows Menu sounds.',
}
# Job J4: the graphics setup pages (setup steps 5 to 8): headings, sentences, cards, the close-up panel.
strings |= {
    'Upscaling and sharpness',
    'Lighting and effects',
    'Choose how the game fills your screen and how smoothly it runs.',
    'Choose how the picture is scaled up to your screen and how sharp it looks.',
    'New lighting effects made for this PC port.',
    'Keep the new look, or play the game as it looked on the Xbox 360.',
    'You can change this any time in Settings.',
    'King Kong was made to run at 30 FPS, and above that some character animations can look wrong, such as the crew rowing the boat at the start.',
    'Match my screen ({0} FPS)',
    'The most pictures the game draws each second.',
    'How the game fills your screen: in a window, borderless or fullscreen.',
    'The size your screen switches to while the game runs.',
    'The game is drawn at up to this size.',
    'How many times a second your screen refreshes while the game runs.',
    "Borderless uses your desktop's resolution. Choose Fullscreen to pick another one.",
    'Desktop',
    "Borderless uses your desktop's refresh rate.",
    "The size of the game's window.",
    'Scales the picture up to your screen and sharpens it.',
    'How many pixels the game draws before they are scaled up to your screen.',
    'How many pixels the game draws before the upscaler rebuilds the picture.',
    '{0} to {1}',
    'How sharply the game is drawn compared with your screen.',
    'Smooths jagged edges after the picture is drawn.',
    'The upscaler smooths edges itself, so SMAA is not used with it.',
    'Makes fine detail crisper.',
    '16x texture filtering',
    'Keeps the ground and distant textures sharp at steep angles.',
    'Brighter highlights and deeper colour on your HDR screen.',
    'Sunlight bounces off surfaces and softly lights the shade beside them.',
    'Soft shading where surfaces meet, in corners and under rocks and grass.',
    "Sharper, softer-edged shadows than the game's own.",
    'The water reflects the rocks, trees and the ship around it.',
    'Sunlight shines through the mist and fog in visible shafts.',
    'Original look',
    'Plays the game exactly as it looked on the Xbox 360.',
    'Use recommended for my PC',
    'Sets every graphics option to what suits your PC, with the new look.',
    'Apply',
    'Use recommended',
    'Sets the options on this page to what suits your PC.',
    'Next',
    'Finishes the graphics setup.',
    'The next page.',
    'The page before.',
    '{0} of {1}',
    'The recommended settings for your PC are set.',
    'The recommended settings for this page are set.',
    'The recommended graphics API. It works on every graphics card the game supports.',
    'Needed for AMD FSR 4',
    'Also works well, and AMD FSR 4 needs it.',
    'Not on this graphics card',
    'Supported',
    'Rebuilds a sharp picture from fewer pixels on NVIDIA RTX graphics cards.',
    'Rebuilds a sharp picture from fewer pixels and runs on most graphics cards.',
    'Rebuilds a sharp picture from fewer pixels, best on Intel graphics cards.',
    'Plain scaling',
    "No upscaler: the game is drawn at up to your screen's size and scaled plainly.",
    'Nothing to compare',
    'This option changes how the game runs, not how it looks.',
    'New',
    # Task 219: the pictures are made from the player's game files (no setup-art screenshots ship any more).
    'Fewer pixels', 'More pixels', 'Upscaler', 'Off', 'On', 'From your game', 'No picture yet',
    'Pictures from your copy of the game show here once its files are installed.',
    'Made from a still of your game: drawn with fewer pixels on the left, more on the right.',
    'Made from a still of your game: half the pixels scaled up plainly on the left, the full picture an upscaler '
    'rebuilds on the right.',
    'Made from a still of your game: as drawn on the left, sharpened on the right.',
    'Made from a still of your game, enlarged: jagged edges on the left, smoothed edges on the right.',
    'Made from a still of your game laid on a floor: the far floor blurs on the left and stays sharp with 16x '
    'filtering on the right.',
    "A picture from your copy of the game. This option's effect only shows in the game itself, as described below.",
    # Task 219: HDR follows Windows (Display > HDR output).
    'HDR output', 'HDR', 'Follow Windows',
    'Follow Windows uses HDR when Windows HDR is on for the screen the game is on. On always asks for HDR, which also '
    'needs Windows HDR. Off always uses SDR.',
    'Windows HDR could not be detected, so Follow Windows uses SDR.', 'Windows HDR is on for this screen.',
    'Windows HDR is off for this screen. It can be turned on in Windows Settings > System > Display.',
    'Windows HDR is off for this screen.', 'HDR brightness',
    'Automatic takes Peak brightness from Windows HDR Calibration and Paper white from the SDR content brightness in '
    'Windows. Manual uses the sliders below.',
    'Automatic (from Windows)', 'Manual', 'Windows HDR Calibration: {0} nits peak brightness.',
    'No Windows HDR Calibration found. The screen reports {0} nits peak brightness.',
    'No peak brightness found, so {0} nits is used.', 'SDR content brightness in Windows: {0} nits.',
    'SDR content brightness in Windows not found, so {0} nits is used.',
    'Set from Windows. Choose Manual under HDR brightness to change it.', 'Peak brightness (nits)',
    'Paper white (nits)', 'Open Windows HDR Calibration',
    'Opens the Windows HDR Calibration app to measure your screen. The values here are read again when you come back.',
    'Opens the free Windows HDR Calibration app in the Microsoft Store. Install it, then select this again to measure '
    'your screen.',
    'Graphics {0} of {1}',
    'Applies when the game starts.',
    'Your graphics card does not run this upscaler.',
    'Compare',
    'Checking which other upscalers your graphics card runs...',
}
# Job J8: the Workshop page (workshop.cpp): every string it draws through T() / F(), and the Home entry.
workshop_cpp = (src / 'workshop.cpp').read_text(encoding='utf-8')
workshop_strings = set(re.findall(r'(?<![A-Za-z_])[TF]\("([^"]+)"', workshop_cpp))
workshop_strings |= set(re.findall(r'DrawCaption\([^;]*?"([^"]+)"\);', workshop_cpp))
workshop_strings |= {'Workshop', 'Download', 'View on GitHub', 'Scroll', 'Select', 'Back',
                     "Opens the project's page on GitHub in your web browser.",
                     'Opens the latest release on GitHub in your web browser. The launcher does not download or run anything.',
                     # owner, 18 Oct: Workshop > Mods, its guide and the mods folder
                     'Making mods', 'Modding guide', 'Open mods folder',
                     'Opens the folder your mods go in, so you can add or remove them.',
                     'How to make texture packs, settings mods and cutscene mods for the game.',
                     'The modding guide (MODDING.md) was not found beside the game.'}
# Task 209: the Workshop's rows and texture pack cards (workshop.cpp, workshop_texpacks.cpp) and the launcher's
# texture pack row, import pop-ups and setup lines: every string drawn through T() / F(), in five languages.
texpacks_cpp = (src / 'workshop_texpacks.cpp').read_text(encoding='utf-8')
# String literals split over lines ("..." "...") count as one.
_tp_flat = re.sub(r'"\s*\n\s*"', '', texpacks_cpp)
texpack_strings = set(re.findall(r'(?<![A-Za-z_])[TF]\("((?:[^"\\]|\\.)*)"', _tp_flat))
texpack_strings |= set(re.findall(r'\bAsk\("((?:[^"\\]|\\.)*)"', _tp_flat))
texpack_strings |= {'Remove', 'Keep it', 'Download', 'Not now'}
# Task 212: the downloads' cards and pages (texture_packs_downloads.h): every sentence in its table and the UHD note.
_downloads_h = re.sub(r'"\s*\n\s*"', '', (src / 'texture_packs_downloads.h').read_text(encoding='utf-8'))
_download_texts = {x for x in re.findall(r'"((?:[^"\\]|\\.)*)"', _downloads_h) if ' ' in x}
if len(_download_texts) < 4:
    fail(f'task 212: only {len(_download_texts)} texts found in texture_packs_downloads.h')
texpack_strings |= _download_texts
texpack_strings = {bytes(x, 'utf-8').decode('unicode_escape').encode('latin-1').decode('utf-8') for x in texpack_strings}
if len(texpack_strings) < 30:
    fail(f'task 209: only {len(texpack_strings)} texture pack strings found')
strings |= texpack_strings
_ws_flat = re.sub(r'"\s*\n\s*"', '', workshop_cpp)
for m in re.finditer(r'action\("[a-z]+", "([^"]+)", "([^"]+)",\s*"((?:[^"\\]|\\.)*)"\);', _ws_flat):
    workshop_strings |= {m.group(1), m.group(2), m.group(3)}
workshop_strings |= set(re.findall(r'(?<![A-Za-z_])[TF]\("((?:[^"\\]|\\.)*)"', _ws_flat))
strings |= {'Mods', 'Texture packs', 'Other TRG projects and games', 'Not installed', 'Installed', 'Downloading',
            'Looking for mods', 'Texture pack', 'Texture packs in the Workshop', 'Open Workshop', 'Texture pack folder',
            'Choose folder', 'Original textures', 'HD texture pack', 'Kong UHD Textures', 'Mod', 'Installing your mod',
            'Downloading Kong UHD Textures', 'Mod installed', 'Mod not installed', 'Kong UHD Textures installed',
            'Cancel the install?', 'Cancel install', 'Keep going', 'Install cancelled.',
            'Another import is running. Try again when it has finished.', 'Continue',
            "Which texture pack the game uses. Only one is on at a time: choosing one switches the other off. Original "
            "textures are the game's own. You can switch at any time, also from the pause menu, and it changes straight away.",
            'Download the HD and UHD texture packs, import your own from a ZIP file, see what each one is and remove the '
            'ones you no longer want.',
            'A texture pack in a folder of your own (with its pack.txt). Choosing one switches it on.',
            "No texture pack is on. Choose one in the Workshop from the main menu.",
            'Nothing is installed and everything you had stays as it was. Your ZIP file is kept.',
            'Your ZIP file is checked and unpacked. It carries on in the background if you leave this pop-up, and Cancel '
            'stops it without changing anything you have installed.',
            'Comic Book Texture Pack', 'Downloading Comic Book Texture Pack', 'Comic Book Texture Pack installed', 'Switch on {0}', '{0} is on',
            "Kong's Tropical Holiday Texture Pack", "Downloading Kong's Tropical Holiday Texture Pack",
            "Kong's Tropical Holiday Texture Pack installed", 'Kong Cartoon Texture Pack',
            'Downloading Kong Cartoon Texture Pack', 'Kong Cartoon Texture Pack installed',
            'Your downloads carry on when you come back to the launcher. What was downloaded so far is kept.',
            'Unpacking: {0} of {1} GB',
            "{0} is in your mods folder. Switch it on in the Workshop's Mods row.",
            '{0} is installed and turned on. It is the only texture pack on: the one before it was switched off. Choose '
            'another at any time in the Workshop or in Settings > Graphics.',
            'The HD texture pack is installed and turned on. It is the only texture pack on: the one before it was '
            'switched off. You can turn it off at any time in Settings > Graphics or the Workshop.',
            'Nothing was installed and everything you had stays as it was.',
            'It has no mod.toml. A mod is one folder with its mod.toml.',
            'It has more than one mod.toml. Put one mod in each ZIP file.',
            'It is a texture pack, not a mod. Use Import texture pack.',
            'It is a mod, not a texture pack. Use Import mod.',
            'It is a texture pack mod without textures/pack.txt.',
            'Its mod.toml has no type the game knows (texture-pack, settings or cutscenes).',
            'Already installed. Turn it on or off at any time in the Workshop or Settings > Graphics.',
            'Already installed. Turn them on or off at any time in Settings > Advanced.',
            'Already downloading. It carries on in the background.',
            'Enabled {0}', 'Disabled', 'Problem', 'Built-in', 'Conflict', 'Version {0}', 'Import mod', 'From a ZIP file'}
if len(workshop_strings) < 14:
    fail(f'J8: only {len(workshop_strings)} Workshop strings found')
strings |= workshop_strings
# Task 200: the Kong mods row (workshop_mods.cpp): every string it draws, in five languages, no dashes; the row
# replaced "Coming soon"; the two built-in mods ship (mods-builtin) and MODDING.md exists.
mods_cpp = (src / 'workshop_mods.cpp').read_text(encoding='utf-8')
mods_strings = set(re.findall(r'(?<![A-Za-z_])[TF]\("([^"]+)"', mods_cpp))
mods_strings |= set(re.findall(r'\{Action::k[A-Za-z]+, "([^"]+)"\}', mods_cpp))
if len(mods_strings) < 35:
    fail(f'task 200: only {len(mods_strings)} Kong mods strings found')
strings |= mods_strings
if 'Coming soon' in workshop_cpp or 'KongMods' not in workshop_cpp:
    fail('task 200: the Workshop still shows Coming soon instead of the Kong mods row')
for builtin in ('cel-shading',):  # Hardcore held back until no healing works (owner, Beta 7)
    if not (root / 'mods-builtin' / builtin / 'mod.toml').exists():
        fail(f'task 200: built-in mod {builtin} missing')
if (root / 'mods-builtin' / 'hardcore').exists():
    fail('task 200: the Hardcore mod is held back and must not ship as a built-in mod yet')
if not (root.parent / 'MODDING.md').exists():
    fail('task 200: MODDING.md missing')
else:
    guide = (root.parent / 'MODDING.md').read_text(encoding='utf-8')
    if '—' in guide or '–' in guide:
        fail('task 200: dash in MODDING.md')
if 'e.push_back({"Workshop", true, [this] { GoTo(kWorkshop); }});' not in (src / 'launcher.cpp').read_text(encoding='utf-8'):
    fail('J8: no Workshop entry on Home')
# Task 209 (owner, 18 Oct): the Workshop is ONE page with three rows in this order; Home's Workshop opens it
# directly; Back closes an inner page (its card keeps the focus), then goes Home; Settings > Graphics keeps its
# texture pack rows; the two import buttons and the website link are on the page; the mods row leaves the texture
# pack mods to the texture packs row; the UHD pack's note is asked before its download.
_launcher = (src / 'launcher.cpp').read_text(encoding='utf-8')
for needle, why in (
        ('if (page == kWorkshop) EnsureWorkshop();', 'Home does not open the Workshop page directly'),
        ('if (!workshop_->Back()) GoTo(kHome);', 'Back on the Workshop does not close the inner page, then go Home'),
        ('    OriginalVideoItems(items);\n    TexturePackItems(items);', 'Settings > Graphics lost its texture pack rows'),
        ('texture_pack_enable_(keys[size_t(i)]);', 'Settings > Graphics does not switch packs with the Workshop rule'),
        ('if (c.name == "@texture-pack-mod") workshop_->OnTexturePackModInstalled(c.value);',
         'a newly imported texture pack is not switched on as the only one'),
        ('if (c.name == "native_texture_pack" && c.value == "true") workshop_->OnFolderPackOn();',
         'the HD texture pack turned on does not switch the texture pack mods off'),
):
    if needle not in _launcher:
        fail(why)
for banned in ('Workshop::View', 'WorkshopEntries', 'DrawWorkshopHub', 'tex_row_focus_'):
    if banned in _launcher or banned in workshop_cpp:
        fail(f'the menu of three is back: {banned}')
if 'const char* captions[kRows] = {"Texture packs", "Mods", "Other TRG projects and games"};' not in workshop_cpp:
    fail('the Workshop rows are not Texture packs, Mods, Other TRG projects and games (in that order)')
for needle, why in (
        ('action("import", "Import mod",', 'no Import mod button on the Mods row'),
        ('action("link", "Build your own mods and texture packs",', 'no Build your own mods and texture packs link'),
        ('kModsUrl = "https://kong.tekrantgaming.com/mods"', 'the Build your own link does not open the website Mods page'),
        ('kong->SetFilter(KongMods::Filter::kOthers);', 'the Mods row shows texture pack mods'),
        ('if (m.texture_pack) continue;', 'the Mods row shows texture pack mods'),
        ('void OpenGuide()', 'the Workshop has no modding guide'),
        ('mods::BuiltinDir().parent_path() / "MODDING.md"', 'the Workshop has no modding guide'),
):
    if needle not in workshop_cpp:
        fail(why)
UHD_NOTE = ('This pack replaces many surfaces with high-detail materials that were not made for this game, so the game '
            'will no longer look exactly as its developers intended. Every effort was made to keep the original style: '
            'where no good match was found, the original texture is used, upscaled with AI.')
_flat = re.sub(r'"\s*\n\s*"', '', texpacks_cpp)
# Task 212: the downloads' texts are one table (texture_packs_downloads.h); the UHD row carries the note, the Comic
# row none. The page draws a row's note, and the download asks it first and starts only after Download.
_uhd_row = _downloads_h[_downloads_h.find('{content::Kind::kUhd,'):_downloads_h.find('{content::Kind::kComic,')]
_comic_row = _downloads_h[_downloads_h.find('{content::Kind::kComic,'):]
if f'kUhdNote =\n    "{UHD_NOTE}";' not in _downloads_h or 'kUhdNote,' not in _uhd_row or 'kUhdNote' in _comic_row:
    fail('the UHD note is not exactly the owner\'s words in the UHD row of texture_packs_downloads.h')
if 'const std::string note = T(p.page->note);' not in _flat:
    fail('the UHD note is not on its page')
m = re.search(r'void Download\(content::Kind kind\) \{(.*?)\n  \}\n', _flat, re.S)
if (not m or 'if (!page || !page->note) {' not in m.group(1) or
        'Ask("Before you download", page->note, {"Download", "Not now"}, 1, [this, kind](int i) {\n      if (i != 0) return;'
        not in m.group(1) or m.group(1).count('host->download(kind)') != 2):
    fail('the UHD download does not start only after the player confirms the note')
if 'Download(kind); }});' not in _flat or 'DownloadUhd' in _flat:
    fail('the Download button does not go through the note question')
for needle, why in (
        ('imp.name = T("Import texture pack");', 'no Import texture pack button on the texture packs row'),
        ('texpacks::EnableMod(', 'the texture pack row does not use the one-pack rule'),
        ('texpacks::EnableFolder(', 'the texture pack row does not use the one-pack rule'),
        ('texpacks::Normalize(', 'old settings with several packs on are not reduced to one'),
):
    if needle not in texpacks_cpp:
        fail(why)
if 'texpacks::EnableMod(' not in mods_cpp:
    fail('switching a texture pack mod on in its mod page does not switch the others off')
if 'texpacks::Normalize(' not in (src / 'mods.cpp').read_text(encoding='utf-8'):
    fail('the game start does not keep only one texture pack')
# Beta 7: Extras > Artbook (artbook.cpp, its Extras row and the quiet download in launcher.cpp): every string it
# draws, its prompts, no dashes; the artbook never asks before Play and has no download strip.
artbook_cpp = (src / 'artbook.cpp').read_text(encoding='utf-8')
artbook_strings = set(re.findall(r'(?<![A-Za-z_])[TF]\("([^"]+)"\s*[,)]', artbook_cpp))
artbook_strings |= set(re.findall(r'\{Action::k[A-Za-z]+, "([^"]+)"\}', artbook_cpp))
artbook_strings |= {
    'Artbook', 'Bonus disc', '{0} paintings', 'Downloading', 'Not downloaded', 'Fit to screen', 'Zoom in',
    'The artbook is still downloading', 'The artbook is not downloaded yet',
    "The paintings come from the game's PS2 bonus disc, which the launcher downloads from archive.org ({0} MB). "
    "Only the pictures are kept.",
    "Concept paintings by Weta from the game's bonus disc. Look at them up close and save them to your PC.",
    'Artbook download failed', 'Try again', 'Later',
    'The artbook could not be downloaded from archive.org after several tries. You can try again now, or later in '
    'Play > Extras > Artbook.',
}
if len(artbook_strings) < 24:
    fail(f'Artbook: only {len(artbook_strings)} strings found')
strings |= artbook_strings
for text, name in ((artbook_cpp, 'artbook.cpp'), ((src / 'artbook_text_table.inc').read_text(encoding='utf-8'),
                                                  'artbook_text_table.inc')):
    if '—' in text or '–' in text:
        fail(f'dash in {name}')
launcher_src = (src / 'launcher.cpp').read_text(encoding='utf-8')
for needle, what in (('if (!st.Busy() || content::Quiet(kind)) continue;', 'Play does not ask about the artbook'),
                     ('if (Quiet(Kind(i))) continue;  // the artbook shows no strip', 'no download strip for it'),
                     ('artbook_want_ = "first start after an update";', 'it downloads after an update'),
                     ('it.label = "Artbook";', 'Extras has an Artbook row')):
    if needle not in launcher_src:
        fail('Artbook: ' + what)
if 'artbook_want_ = "end of setup";' not in (src / 'launcher_setup_end.inc').read_text(encoding='utf-8'):
    fail('Artbook: it does not download at the end of the setup')
# Job J5: setup step 9 (button prompts).
strings |= {
    'Choose your button prompts',
    'Pick which buttons the game and this launcher show in menus and hints.',
    'Keyboard and mouse', 'Mouse camera', 'Now: {0}', 'Automatic ({0})', 'Preview',
    'Press {0} to select and {1} to go back.',
    'In play, Kong grabs with {0} and jumps with {1}.',
    'Shows the buttons of the controller you are using, and changes when you switch controllers.',
    "Plays with a keyboard and mouse: the game's hints show the keys and the mouse turns the camera.",
    'Always shows the {0} buttons.',
    'Controller buttons and keyboard keys can be remapped any time in Settings > Controls.',
    # step 10, "You're all set"
    "You're all set", 'Thanks for installing King Kong Recompiled.',
    'More options, such as field of view, audio, subtitles, controls and much more, are in Settings on the main '
    'menu. Enjoy the game.',
    'Join our Discord', 'Finish setup', 'The Discord invite opened in your web browser.',
    'King Kong is in your Steam library.', 'Checking for Steam...',
    'You started King Kong from Steam, so it is already in your library.', 'Steam was not found on this PC.',
    'Add to Steam', 'Added to Steam', 'Play it from your Steam library, with its artwork.',
    'Chat with players, share feedback, shape the project.',
    "Steam wasn't found on this computer, or nobody has signed in to it yet.",
}
# Job J6: New in this update (players who had the setup on an older build) and the graphics pages opened from it.
strings |= {
    'New in this update',
    'This update adds a few things you can set up now. Everything here is also in Settings, so you can leave it for later.',
    "Sharper versions of the game's textures, made at four times their original size. A separate download of {0} GB "
    'that you can turn on or off at any time in Settings > Graphics.',
    'Installed: your texture pack ({0})', 'You use a texture pack folder of your own.',
    'Not now. You can get it later in Settings > Graphics.', 'I already have one', 'Not now',
    'The PS2 version has cutscenes between some missions that the Xbox 360 version left out. Restore them as upscaled '
    'videos? A separate download of {0} GB.',
    'Restored', 'No thanks. You can restore them later in Settings > Advanced.',
    'New graphics options: display and frame rate, upscaling and sharpness, lighting and effects, and the Original '
    'look. Your settings stay as they are until you change them.',
    'See the new graphics options', 'Back to New in this update.', 'Done', 'Update', 'New', 'Graphics',
    'PS2 cutscenes', 'Download', 'Watch progress', 'Yes, restore them', 'No thanks', 'Continue', 'Installed',
}
# Task 206 (owner, 18 Oct): brightness and gamma, Performance metrics (the old Frame counter), the pause menu's
# greyed rows: every string in five languages, picked up from the code that draws them.
_l206 = (src / 'launcher.cpp').read_text(encoding='utf-8')
_m = re.search(r'void PictureItems\(std::vector<Item>& items\) \{(.*?)\n  \}\n', _l206, re.S)
_picture = _m.group(1) if _m else ''
if not _picture:
    fail('206: Display > Picture (PictureItems) is missing')
_texts206 = set()
for _src in (re.sub(r'//[^\n]*', '', _picture), _l206[_l206.find('items.push_back(Section("Overlay"));'):].split('return items;')[0]):
    for _lit in re.findall(r'((?:"(?:[^"\\]|\\.)*"\s*)+)', _src):
        _t = ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', _lit))
        if len(_t) > 3 and re.search('[A-Za-z]{3}', _t) and '_' not in _t and '%' not in _t and _t not in ('true', 'false', 'off', 'fps', 'full'):
            _texts206.add(_t.replace("\\'", "'"))
strings |= _texts206
if len(_texts206) < 6:
    fail(f'206: only {len(_texts206)} texts found in PictureItems and the Overlay row')
strings |= {'Brightness', 'Gamma', 'Picture', 'Overlay', 'Performance metrics', 'Off', 'FPS only', 'Full', 'game',
            'No upscaler',
            "This can't be changed while you play. Change it from the main menu.",
            'While you play, the change is made at once. FSR 1 and NIS are chosen from the main menu.'}
for _needle, _why in (
        ('static constexpr const char* kInLauncherOnly =\n      "This can\'t be changed while you play.', 'the pause menu note'),
        ('if (it.type != ItemType::kSection && help::RestartOnly(it.label)) Disable(it, true, kInLauncherOnly);',
         'restart-only rows are greyed in the pause menu, not left out'),
        ('if (in_game_ && spatial_now) Disable(it, true, kInLauncherOnly);', 'the upscaler greys out with FSR 1 / NIS in game'),
        ('"kk_performance_metrics", {{"Off", "off"}, {"FPS only", "fps"}, {"Full", "full"}}', 'Performance metrics levels'),
        ('"kk_brightness", -50, 50, 1,', 'the brightness slider'), ('"kk_gamma", 0.5f, 1.5f, 0.05f,', 'the gamma slider')):
    if _needle not in _l206:
        fail(f'206: {_why}')
# Task 222 (owner, 18 Oct, option A): the pause menu is a slim list (task 248: Resume, Photo mode, Controls, Gameplay,
# Settings, then Load last checkpoint and Quit to main menu), each settings entry opening its own page; the old
# layout (tabs across the top, a Settings entry, the logo) is gone; in game nothing says "launcher".
_pm = (src / 'pause_menu.cpp').read_text(encoding='utf-8')
_pl = (src / 'pause_menu_logic.h').read_text(encoding='utf-8')
_pt222 = (src / 'pause_text_table.inc').read_text(encoding='utf-8')
_m = re.search(r'inline const char\* Label\(Entry e\) \{(.*?)\n\}', _pl, re.S)
_labels = dict(re.findall(r'case Entry::(\w+): return "([^"]+)";', _m.group(1))) if _m else {}
_m = re.search(r'inline const char\* What\(Entry e\) \{(.*?)\n\}', _pl, re.S)
_whats = dict(re.findall(r'case Entry::(\w+): return "([^"]+)";', _m.group(1))) if _m else {}
for _dev in ('kGameOptions',):  # developer builds only
    _labels.pop(_dev, None)
    _whats.pop(_dev, None)
if set(_whats) != {'kResume', 'kPhotoMode', 'kControls', 'kGameplay', 'kSettings', 'kDisplay', 'kGraphics', 'kAudio',
                   'kCheats', 'kLoadCheckpoint', 'kScore', 'kSave', 'kQuitToMainMenu', 'kQuitGame'}:
    fail(f'222: the strip sentences cover {sorted(_whats)}')
strings |= set(_labels.values()) | set(_whats.values())
strings |= {'Change from the main menu', 'Photo mode', 'Go back to the last checkpoint? Your progress since then is lost.',
            'Leave the game and go back to the main menu?', 'Leave the game and close it?', 'Leave', 'Quit', 'Cancel',
            # Beta 12: the Quit confirmations in a story run say when the game saves
            re.search(r'kQuitToMainMenuSaves =\s*"([^"]+)"', _pl).group(1), re.search(r'kQuitGameSaves = "([^"]+)"', _pl).group(1),
            'Paused', 'Select', 'Back', 'Change', 'Default', 'Resume', 'Reset page', 'Default: {0}'}
# Task 237 (owner 18 Oct): "Load last checkpoint" replaces "Restart chapter". It does what the pause page's own
# item 6 does (the reload request a death makes, without the death counters) and never the chapter's Start again.
if _labels.get('kLoadCheckpoint') != 'Load last checkpoint' or 'Restart chapter' in _pm + _pl:
    fail('237: the pause list has no "Load last checkpoint" or still says "Restart chapter"')
if 'play::RunPauseAction(PauseAction::kLoadCheckpoint)' not in _pm or 'PauseAction::kStartAgain' in _pm:
    fail('237: the pause menu does not run Load last checkpoint (or still runs Start again)')
_ph237 = (src / 'pause_hooks.cpp').read_text(encoding='utf-8')
for _needle, _why in (('Store32(base, kReloadRequest, 1);', 'no reload request'),
                      ('Store32(base, kReloadFrames, 30);', 'no reload frame count'),
                      ('__imp__sub_825E66D0(call, base);', "the pause is not ended the game's way"),
                      ('g_can_checkpoint = !BonusLevelSession();', 'offered in the Extras galleries and credits')):
    if _needle not in _ph237:
        fail(f'237: {_why}')
for _bad in ('19900', '19904', '0x4DBC', '0x4DC0'):
    if re.search(r'Store32\([^;]*' + re.escape(_bad), _ph237):
        fail('237: Load last checkpoint touches a death counter')
if _labels.get('kQuitToMainMenu') != 'Quit to main menu':
    fail('222: no "Quit to main menu" entry')
# Task 248 (owner 19 Oct): the main list is Resume, Photo mode, Controls, Gameplay, Settings, then the game's items;
# Settings opens a second list (Display, Graphics, Audio, Cheats, Options in developer builds). Beta 12: Cheats is
# always listed, so the main Cheats switch can be switched on again in game.
if _labels.get('kSettings') != 'Settings' or _whats.get('kSettings') != 'Display, graphics and audio settings.':
    fail('248: no Settings entry with its strip sentence')
_m = re.search(r'inline std::vector<Listed> Entries\(const Available& a\) \{(.*?)\n\}', _pl, re.S)
if not _m or 'Entry::kDisplay' in _m.group(1) or 'Entry::kCheats' in _m.group(1) or \
        '{Entry::kControls, Entry::kGameplay, Entry::kSettings}' not in _m.group(1):
    fail('248: the main list is not Resume, Photo mode, Controls, Gameplay, Settings')
_m = re.search(r'inline std::vector<Listed> SettingsEntries\(const Available& a\) \{(.*?)\n\}', _pl, re.S)
if not _m or '{Entry::kDisplay, Entry::kGraphics, Entry::kAudio, Entry::kCheats}' not in _m.group(1) or \
        'a.cheats' in _m.group(1) or 'a.game_options' not in _m.group(1):
    fail('248: the Settings list is not Display, Graphics, Audio, Cheats (+ Options)')
for _needle, _why in (('DrawEntryList(dl, view_ == View::kSettings)', 'no Settings list view'),
                      ('pause::SettingsEntries(', 'the Settings list is not drawn from SettingsEntries'),
                      ('pause::ListWithPage(a, int(page_))', 'LB / RB do not stay in the page\'s own list'),
                      ('OpenMainList(Entry::kSettings)', 'Back from Settings does not return to the main list on Settings'),
                      ('pause::Label(Entry::kSettings), InGameSettings::PageName(page)', 'no "PAUSED / SETTINGS / page" breadcrumb'),
                      ('t == "list:settings"', 'no list:settings developer step')):
    if _needle not in _pm:
        fail(f'248: {_why}')
for _needle, _why in (('ui::DrawTabs(', 'the old tab bar is still drawn'),
                      ('LoadLogo', 'the old logo is still drawn'), ('"Return to main menu"', 'the old "Return to main menu" entry')):
    if _needle in _pm:
        fail(f'222: {_why}')
for _needle, _why in (('pause::PageOrder(kinds)', 'pages are not ordered live rows first, then the quiet group'),
                      ('caption.label = pause::kQuietCaption;', 'no "Change from the main menu" group'),
                      ('DrawStrip(dl, focused,', 'a page has no description strip'),
                      ('DrawStrip(dl, nullptr, T(pause::What(focus_entry))', 'the list has no description strip'),
                      ('pause::NextPage(', 'LB / RB do not move between pages'),
                      ('BackToList()', 'Back does not return to the list on the same entry')):
    if _needle not in _pm:
        fail(f'222: {_why}')
for _name, _text in (('pause_menu.cpp', _pm), ('pause_menu_logic.h', _pl), ('pause_text_table.inc', _pt222)):
    _code = '\n'.join(l.split('//')[0] for l in _text.splitlines() if not l.lstrip().startswith('#include'))
    for _s in re.findall(r'"([^"\n]*)"', _code):
        if 'launcher' in _s.lower() and not _s.startswith('kk_launcher'):
            fail(f'222: {_name} says "launcher" in game: {_s!r}')
    if '\u2014' in _text or '\u2013' in _text:
        fail(f'222: dash in {_name}')
if '"kk_show_fps"' in _l206 or '"Frame counter"' in _l206:
    fail('206: the old Frame counter row is still in the launcher')
_settings206 = (src / 'settings.cpp').read_text(encoding='utf-8')
if 'void MigrateFrameCounter()' not in _settings206 or 'rex::cvar::SetFlagByName("kk_performance_metrics", "fps");' not in _settings206:
    fail('206: an old Show FPS is not moved to Performance metrics')
if 'kk::MigrateFrameCounter();' not in (src / 'king_kong_app.h').read_text(encoding='utf-8'):
    fail('206: the Show FPS move is not run at start')
_pt = (src / 'picture_text_table.inc').read_text(encoding='utf-8')
if '\u2014' in _pt or '\u2013' in _pt:
    fail('dash in picture_text_table.inc')
# Task 331: the Returning to the main menu loop's lines: every tip and the saved line in launcher.cpp are
# translated (checked above with the rest), and none has a dash (owner: plain words, no dashes).
_l331 = (src / 'launcher.cpp').read_text(encoding='utf-8')
_tips331 = re.search(r'kTips\[kTipCount\] = \{(.*?)\};', _l331, re.S)
if not _tips331:
    fail('331: the tips list is missing')
else:
    _t331 = re.findall(r'"((?:[^"\\]|\\.)*)"', _tips331.group(1))
    if len(_t331) != 12:
        fail(f'331: {len(_t331)} tips, not 12')
    strings |= set(_t331)
    for _s in _t331 + [x for e in _t331 for x in entries.get(e, [])]:
        if '-' in _s or '\u2014' in _s or '\u2013' in _s:
            fail(f'331: dash in a tip: {_s!r}')
strings |= {'Game saved at your last checkpoint.'}
for s in sorted(strings):
    if s == 'Home':
        continue  # never drawn as text
    t = entries.get(s)
    if not t:
        fail(f'no translation: {s!r}')
        continue
    for lang, text in zip(('de', 'es', 'fr', 'it'), t):
        if not text.strip():
            fail(f'empty {lang} translation: {s!r}')
        if sorted(re.findall(r'\{\d\}', text)) != sorted(re.findall(r'\{\d\}', s)):
            fail(f'{lang} placeholders differ: {s!r}')
for en, t in entries.items():
    for text in [en, *t]:
        if '—' in text:
            fail(f'em dash in {en!r}')
new_table = (src / 'launcher_layout_text_table.inc').read_text(encoding='utf-8')
if '—' in new_table or '–' in new_table:
    fail('dash in launcher_layout_text_table.inc')

# The online player count reminder: nothing opens a pop-up about it; it is in the setup and on About only.
launcher = (src / 'launcher.cpp').read_text(encoding='utf-8')
for banned in ('MaybeAskOnline', '"Keep it on"', '"Switch it off"', 'OpenModal("Online player count"'):
    if banned in launcher:
        fail(f'the online reminder is back: {banned}')
if 'CvarToggle("Online player count"' not in launcher:
    fail('the About page lost its Online player count switch')
if '"Online players"' not in launcher:
    fail('the setup lost its Online players entry')

# Task 194 (Beta 7 uploads, owner 17 Oct): both descriptions of the switch (setup and About) say it also helps
# reduce stutter by sharing graphics pipeline data and sends anonymous performance summaries and crash reports,
# in five languages, with no dashes.
# Only the descriptions the launcher draws (the source holds each split over several literals).
flat = re.sub(r'"\s*\n\s*"', '', launcher)
drawn = [en for en in entries if en.startswith('Shows how many players are online') and en.replace('"', '\\"') in flat]
if len(drawn) != 2:
    fail(f'expected the setup and About descriptions of Online players, found {len(drawn)}')
for en in drawn:
    for needed in ('Help reduce stutter: share graphics pipeline data', 'anonymous performance summary',
                   'crash report', 'user name and personal folders removed'):
        if needed not in en:
            fail(f'the Online players description lacks "{needed}": {en[:60]!r}')
    for lang, text in zip(('de', 'es', 'fr', 'it'), entries[en]):
        if not text.strip() or text == en:
            fail(f'no {lang} translation of the Online players description')
        if '—' in text or '–' in text:
            fail(f'dash in the {lang} Online players description')
    if '—' in en or '–' in en:
        fail('dash in the Online players description')
if 'REXCVAR_DEFINE_BOOL(kk_online_count, true,' not in (src / 'settings.cpp').read_text(encoding='utf-8'):
    fail('the Online player count default changed')

# Every bonus is unlocked (owner, 18 Oct): nothing in the launcher reads a save to lock a bonus, the bonus video
# modes are never shown locked, Extras has no save row and starts with the Artbook.
for banned in (LOCKED_HELP, 'ReadExtraProgress', 'ExtraUnlocked(', 'save.label = "Save";',
               "Bonuses use this save's progress and scores."):
    if banned in launcher:
        fail(f'a bonus can still be locked or tied to a save in the launcher: {banned}')
if LOCKED_HELP in entries:
    fail('the locked bonus help text is still translated')
m = re.search(r'std::vector<Item> ExtrasItems\(\) \{(.*?)\n  \}\n', launcher, re.S)
extras = m.group(1) if m else ''
if 'it.label = "Artbook";' not in extras or extras.find('it.label = "Artbook";') > extras.find('for (const auto& extra : play::kExtras)'):
    fail('the Artbook is not the first item in Extras')
if '"Unlocked"' not in extras or '"Locked"' in extras:
    fail('Extras still shows locked bonuses')

# The frame rate warning pop-up is gone (owner, 18 Oct): Play starts the game.
for banned in ('OpenFrameRateWarning', 'Frame rate above 30 FPS', 'Play at 30 FPS', 'Keep {0} FPS'):
    if banned in launcher or banned in entries:
        fail(f'the frame rate warning is still there: {banned}')

# One bar, one label: no step text on the shader screen or in the pack's status.
shaders_cpp = (src / 'native_shaders.cpp').read_text(encoding='utf-8')
for text, name in ((launcher, 'launcher.cpp'), (shaders_cpp, 'native_shaders.cpp')):
    for banned in ('Step {0} of {1}', 'StepText()', 'Compiling the {0} shaders', 'Preparing the {0} pipelines'):
        if banned in text:
            fail(f'{name} still shows steps: {banned}')
if 'native_pack_.ProgressLabel()' not in launcher or 'DrawnFraction(' not in launcher:
    fail('the shader screen does not draw the single eased bar')

# Settings' tabs (task 169): the settings pages in the owner's order, then About; the pause menu shares the
# settings pages (not About). Owner, 18 Oct: Home's About entry is Benchmark, which opens the benchmark.
if layout_pages != ['Display', 'Graphics', 'Audio', 'Controls', 'Gameplay', 'Cheats', 'Advanced']:
    fail(f'settings_layout.h pages are {layout_pages}')
m = re.search(r'enum Page \{([^}]*)\};', launcher)
launcher_pages = [x.strip() for x in m.group(1).split(',')] if m else []
tabs = launcher_pages[launcher_pages.index('kDisplay'):launcher_pages.index('kAbout') + 1] if 'kAbout' in launcher_pages else []
if tabs != ['kDisplay', 'kGraphics', 'kAudio', 'kControls', 'kGameplay', 'kCheats', 'kAdvanced', 'kAbout']:
    fail(f'the launcher Settings tabs are {tabs}')
if 'constexpr Page kLastSettingsTab = kAbout;' not in launcher:
    fail('About is not the last Settings tab')
if '{"About", true, [this] { GoTo(kAbout); }}' in launcher:
    fail('Home still has an About entry')
if 'e.push_back({"Benchmark", CanPlay(), [this] { OpenBenchmark(); }});' not in launcher:
    fail('Home has no Benchmark entry')
m = re.search(r'std::vector<HomeEntry> HomeEntries\(\) \{(.*?)\n  \}\n', launcher, re.S)
home = re.findall(r'e\.push_back\(\{"([^"]+)"', m.group(1)) if m else []
if home != ['Play', 'Settings', 'Workshop', 'Achievements', 'Benchmark', 'Quit']:
    fail(f'Home entries are {home}')
ingame = (src / 'ingame_settings.h').read_text(encoding='utf-8')
if 'enum Page { kDisplay, kGraphics, kAudio, kControls, kGameplay, kCheats, kAdvanced, kPageCount };' not in ingame:
    fail('the pause menu Settings tabs differ from the launcher ones')
if 'case kCheats: items = CheatItems(); break;' not in launcher:
    fail('no page builds the cheats')
for name in ('Cheats', 'About', 'Settings'):
    if name not in entries:
        fail(f'no translation for the tab {name!r}')

# Job J2: setup steps 3 and 4 after the shaders, the cutscene page only where PS2 movies play, the pop-ups'
# buttons as specified, the J1 temporary rows gone, the pause menu with the switches only, and no log line
# from the UI thread while a download unpacks (REPORT-181).
for needle, why in (
        ('setup_shaders_ ? 4 : setup_textures_ ? 6', 'the texture pack page does not follow the shaders'),
        ('setup_cutscenes_ ? 7 : setup_graphics_ ? kGfxFirstStage', 'the cutscene page does not follow the texture pack page (the graphics pages after it)'),
        ('setup_cutscenes_ = movies::PlaybackAvailable();', 'the cutscene page is not tied to PS2 movie playback'),
        ('if (movies::PlaybackAvailable()) {', 'Advanced > Cutscenes is not tied to PS2 movie playback (Linux too, task 187)'),
        ('{"Download HD texture pack", true,', 'the texture pack page lost Download'),
        ('{"I already have a texture pack", true,', 'the texture pack page lost I already have a texture pack'),
        ('{"Skip", true,', 'the texture pack page lost Skip'),
        ('{"Yes, restore them", true,', 'the cutscene page lost Yes'),
        ('{"No thanks", true,', 'the cutscene page lost No thanks'),
        ('    content::Service::Get().Download(kind);\n    REXLOG_INFO("KK content: {} download started ({}); watching the progress"',
         'a download does not go straight to its progress pop-up (owner 19 Oct)'),
        ('{"Pause", "Run in background", "Cancel"}', 'the progress pop-up buttons differ'),
        ('if (st.stage != content::Stage::kUnpacking)', 'the UI thread logs while unpacking'),
        ('void NotifyContentDone(', 'no notification hook for job J3'),
        # Job J3: the decision (notify_logic.h), the switch, the ding and the system notification.
        ('const notify::Plan plan = notify::Decide(s);', 'J3: the download end does not decide what to notify'),
        ('s.setting_on = GetBool("kk_notify_downloads");', 'J3: the Settings switch is not respected'),
        ('if (plan.ding) PlayDing();', 'J3: no ding'),
        ('if (plan.system) notify::Show(title, body,', 'J3: no system notification'),
        ('items.push_back(CvarToggle("Notify me when downloads finish",', 'J3: no Settings switch'),
        ('if (!ContentReadyForPlay()) return;', 'J3: Play does not ask while a download runs'),
        ('{"Play now", "Stay here"}', 'J3: the Play while downloading buttons differ'),
        ('content::Service::Get().PauseForPlay(kind);', 'J3: Play now does not pause the download'),
        ('if (service.ResumeAfterPlay())', 'J3: the launcher does not continue a download paused for the game')):
    if needle not in launcher:
        fail(why)
for banned in ('"Import texture pack ZIP"', '"Restored PS2 cutscenes"', '"Remove HD texture pack"', '"Play PS2 cutscenes"'):
    if banned in launcher or banned in layout:
        fail(f'a J1 temporary row is back: {banned}')
m = re.search(r'void TexturePackItems\(std::vector<Item>& items\) \{(.*?)\n  \}\n', launcher, re.S)
body = m.group(1) if m else ''
# Task 209: in the pause menu (in_game_) only the switch; the launcher's choice of packs comes after its return.
_game = body.find('if (in_game_ || !texture_pack_choices_) {')
if _game < 0 or body.find('return;', _game) > body.find('it.type = ItemType::kChoice;') or body.find('return;', _game) < 0:
    fail('the pause menu shows more than the texture pack switch')

# Job J4: the graphics pages are setup stages 8 to 11 after the cutscene step, drawn by launcher_setup_graphics.inc
# with the Settings rows (one source of truth), "Use recommended" and "change any time in Settings" on every page,
# and the pictures made at run time from the player's game files (task 219: no setup-art folder, nothing
# game-derived shipped).
gfx = (src / 'launcher_setup_graphics.inc').read_text(encoding='utf-8')
for needle, why in (
        ('#include "launcher_setup_graphics.inc"', 'launcher.cpp includes the graphics pages'),
        ('setup_graphics_ ? kGfxFirstStage', 'SetupStage has the graphics pages'),
        ('setup_graphics_ = true;', 'the setup asks the graphics pages'),
):
    if needle not in launcher:
        fail(f'{why} ({needle})')
for needle, why in (
        ('std::string(T(kGfxIntros[page])) + " " + T(kChangeLater)', 'every page says it can be changed in Settings'),
        ('"Use recommended"', 'Use recommended row'), ('"Use recommended for my PC"', 'Use recommended for my PC row'),
        ('art::LoadLauncherArt(ArtDir()).backdrops', 'pictures from the game files'),
        ('compare::Ground(floor, kW, kH, true)', '16x filtering picture made at run time'),
        ('DisplayItems()', 'Display rows from Settings'), ('GraphicsItems()', 'Graphics rows from Settings'),
):
    if needle not in gfx:
        fail(f'{why} ({needle})')
if '—' in gfx or '–' in gfx:
    fail('dash in launcher_setup_graphics.inc')

for banned in ('setup-art', '-original.png', '-new.png'):
    if banned in gfx:
        fail(f'task 219: the setup pages still load shipped screenshots ({banned})')
# Task 219: HDR follows Windows: the Display rows, the calibration button, and present_hdr decided at start.
_l219 = (src / 'launcher.cpp').read_text(encoding='utf-8')
for needle, why in (
        ('"kk_hdr", {{"Follow Windows", "auto"}, {"On", "on"}, {"Off", "off"}}', 'HDR row: Follow Windows / On / Off'),
        ('"kk_hdr_levels", {{"Automatic (from Windows)", "auto"}, {"Manual", "manual"}}', 'HDR brightness row'),
        ('[] { hdr::OpenCalibrationApp(); }', 'Open Windows HDR Calibration button'),
        ('"kk_hdr", "kk_hdr_levels", "present_hdr"', 'Display reset includes the HDR modes')):
    if needle not in _l219:
        fail(f'219: {why}')
if 'kk::hdr::Apply("start");' not in (src / 'king_kong_app.h').read_text(encoding='utf-8'):
    fail('219: present_hdr is not decided at start')

# Job J5: setup steps 9 and 10 come after the graphics pages (launcher_setup_end.inc): the button prompt cards
# set kk_button_prompts (Keyboard and mouse through the Controls page's own input switch).
end = (src / 'launcher_setup_end.inc').read_text(encoding='utf-8')
for needle, why in (
        ('#include "launcher_setup_end.inc"', 'launcher.cpp includes the last setup pages'),
        ('setup_prompts_ ? kPromptsStage', 'SetupStage has the button prompts page'),
):
    if needle not in launcher:
        fail(f'{why} ({needle})')
if 'setup_prompts_ = true;' not in gfx:
    fail('the graphics pages do not go on to the button prompts')
for needle, why in (
        ('Set("kk_button_prompts", value)', 'the cards set kk_button_prompts'),
        ('SetInputMode(true)', 'Keyboard and mouse switches Input as Controls does'),
        ('"Controller buttons and keyboard keys can be remapped any time in Settings > Controls."', 'remap note'),
        ('{"auto", "Automatic"}', 'Automatic card'), ('{"xbox360", "Xbox 360"}', 'Xbox 360 card'),
        ('{"xbox_series", "Xbox Series"}', 'Xbox Series card'), ('{"ps5", "PlayStation 5"}', 'PlayStation 5 card'),
        ('{"ps2", "PlayStation 2"}', 'PlayStation 2 card'), ('{"steam_deck", "Steam Deck"}', 'Steam Deck card'),
        ('{"keyboard", "Keyboard and mouse"}', 'Keyboard and mouse card'),
):
    if needle not in end:
        fail(f'{why} ({needle})')
# Step 10: both big buttons work through the existing actions, with the marks Home already uses (no new brand
# artwork), and Finish setup opens What's new once on the main menu.
for needle, why in (
        ('OpenUrl(kDiscordUrl)', 'Join our Discord opens the invite'), ('OpenSteamPrompt()', 'Add to Steam as on Home'),
        ('kDiscordIcon', 'Discord mark'), ('kSteamIcon', 'Steam mark'), ('"Finish setup"', 'Finish setup'),
        ('open_whats_new_ = true;', "What's new after the setup"),
        ('"Steam wasn\'t found on this computer, or nobody has signed in to "', 'Steam missing message'),
):
    if needle not in end:
        fail(f'{why} ({needle})')
if 'setup_all_set_ ? kAllSetStage' not in launcher:
    fail("SetupStage has the You're all set page")
if '\u2014' in end or '\u2013' in end:
    fail('dash in launcher_setup_end.inc')

# Job J6: the Original or Remastered step is gone (new installs are Remastered from the setup's start; the Original
# look stays on graphics page 4 and in Settings), and players who updated get the one-time New in this update screen.
new_src = (src / 'launcher_setup_new.inc').read_text(encoding='utf-8')
for banned in ('setup_style_', 'DrawStyleCards', 'ApplyOriginal', '"Choose a look"', 'look:original'):
    if banned in launcher or banned in gfx or banned in end or banned in new_src:
        fail(f'J6: the old look step is back: {banned}')
for needle, why in (
        ('#include "launcher_setup_new.inc"', 'launcher.cpp includes the New screen'),
        ('update_new_ ? kNewStage : 0', 'SetupStage has the New screen last'),
        ('void StartFullSetup(', 'one entry to the full setup'),
        ('if (first_start) StartFullSetup(false);', 'a fresh start does not save before the update test (What' + chr(39) + 's new would wait as after an update)'),
        ('if (save) SaveSettings(paths_.config_path);', 'the full setup saves only when asked'),
        ('if (Get("kk_setup_version").empty()) Set("kk_setup_version", kSetupWaiting);', 'an update marks the New screen as waiting'),
        ('update_new_ = files_ok_ && !testing && Get("kk_setup_version") == kSetupWaiting;', 'the New screen only when waiting'),
        ('InSetup(), update_new_, ShaderScreen()', "What's new is held back by the New screen"),
):
    if needle not in launcher:
        fail(f'J6: {why} ({needle})')
m = re.search(r'void StartFullSetup\(bool save = true\) \{(.*?)\n  \}\n', launcher, re.S)
body = m.group(1) if m else ''
if 'if (Get("kk_setup_version").empty()) {\n      ApplyRemastered();' not in body:
    fail('J6: a new install does not start Remastered')
if 'Set("kk_setup_version", KK_VERSION);' not in body or 'update_new_ = false;' not in body:
    fail('J6: the full setup does not stand in for the New screen')
for needle, why in (
        ('{"Download", new_tex_answer_ == 0,', 'texture pack Download'), ('{"I already have one",', 'I already have one'),
        ('{"Not now",', 'Not now'), ('{"Yes, restore them",', 'cutscenes Yes'), ('{"No thanks",', 'cutscenes No thanks'),
        ('if (movies::PlaybackAvailable()) {', 'cutscenes only where PS2 movies play'),
        ('{"See the new graphics options", false, [this] { OpenNewGraphics(); }}', 'the graphics pages from the New screen'),
        ('{"Continue", false, [this] { DismissNewScreen(); }}', 'Continue'),
        ('Set("kk_setup_version", KK_VERSION);', 'dismissed for good'),
        ('StartContentDownload(Kind::kTexturePack, 0);', 'the download starts as from Settings'),
):
    if needle not in new_src:
        fail(f'J6: {why} ({needle})')
if 'if (gfx_from_new_) return BackToNewScreen();' not in gfx:
    fail('J6: the last graphics page does not go back to the New screen')
if 'ApplyRemastered();\n        setup_style_' in gfx:
    fail('J6: the graphics pages still set Remastered as they come up')
if 'Set("kk_setup_version", KK_VERSION);' not in end or '!whats_new_shown_' not in end:
    fail("J6: Finish setup does not record the setup or could open What's new twice")
if '—' in new_src or '–' in new_src:
    fail('dash in launcher_setup_new.inc')

# Job J5 part 3: the launcher's controllers press nothing while its window is not the active one (REPORT-188).
ui_src = (src / 'launcher_ui.cpp').read_text(encoding='utf-8')
if 'input_.set_pads_active(active);' not in launcher:
    fail('the launcher does not tell its input whether its window is active')
if 'if (!pads_active_)' not in ui_src or 'dir_stale_' not in ui_src:
    fail('the input does not ignore controllers while the window is not active')

if failures:
    print('\n'.join('FAIL: ' + f for f in failures))
    sys.exit(1)
print(f'{len(strings)} launcher strings translated; no online reminder; one shader bar')
