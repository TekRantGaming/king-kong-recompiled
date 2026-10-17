"""Task 144 source checks for the launcher, no game or window needed:
- every settings heading and row in settings_layout.h, and the launcher's new page and shader texts, has a
  German, Spanish, French and Italian translation (placeholders kept, no em dashes);
- the online player count reminder can never open by itself (owner, 16 Oct): no code opens it;
- the shader screen shows one bar with one label: no step text is drawn;
- the bonus video modes' help text says unlocked or locked to match the save;
- job J2: setup steps 3 and 4 (texture pack, PS2 cutscenes), their pop-ups and the Settings rows;
- job J3: the notification switch (Settings > Advanced > Downloads) and the ding/notification calls;
- job J8: the Workshop page's text and its Home entry;
- job J6: the look step gone, new installs Remastered, the New in this update screen;
- Settings' tabs are Display, Graphics, Audio, Controls, Gameplay, Cheats, Advanced, About (owner, 16 Oct
  evening), the pause menu's the same without About, and About stays on Home too.
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
for name in ('launcher_text_table.inc', 'debug_overlay_text_table.inc', 'benchmark_text_table.inc',
             'hdr_text_table.inc', 'launcher_layout_text_table.inc', 'workshop_text_table.inc',
             'artbook_text_table.inc', 'mods_text_table.inc'):
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
    'Settings', 'Achievements', 'About', 'Extras', 'Quit', 'Bonuses use this save\'s progress and scores.',
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
# Old movie filter / Horizontal flip (Graphics > Extras): the help text follows the unlock state (task 159).
LOCKED_HELP = 'An original bonus video mode. Unlock it in Extras before changing it.'
UNLOCKED_HELP = 'An original bonus video mode, unlocked in Extras.'
strings |= {LOCKED_HELP, UNLOCKED_HELP}
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
    'Plays the restored PS2 cutscenes in the game, between the missions where the Xbox 360 version has none. Download them in the launcher\'s Settings > Advanced. You can switch them at any time.',
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
    # This can take a while, progress, finished and error pop-ups, the strip
    'This can take a while',
    'The HD texture pack is a {0} GB download. After that it is checked and unpacked. Depending on your connection this takes from a few minutes to an hour or more.',
    'The restored PS2 cutscenes are a {0} GB download. After that they are checked and unpacked. Depending on your connection this takes from a few minutes to an hour or more.',
    'It runs in the background, so you can carry on meanwhile. If the connection drops or you close the launcher, it continues from where it stopped.',
    'It starts when the download before it has finished.',
    'Minimise and notify me',
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
    'Comparison picture not included',
    'Release builds show a close-up from the game here: Original on the left, New on the right.',
    'New',
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
                     'Opens the latest release on GitHub in your web browser. The launcher does not download or run anything.'}
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
for builtin in ('hardcore', 'cel-shading'):
    if not (root / 'mods-builtin' / builtin / 'mod.toml').exists():
        fail(f'task 200: built-in mod {builtin} missing')
if not (root.parent / 'MODDING.md').exists():
    fail('task 200: MODDING.md missing')
else:
    guide = (root.parent / 'MODDING.md').read_text(encoding='utf-8')
    if '—' in guide or '–' in guide:
        fail('task 200: dash in MODDING.md')
if 'e.push_back({"Workshop", true, [this] { GoTo(kWorkshop); }});' not in (src / 'launcher.cpp').read_text(encoding='utf-8'):
    fail('J8: no Workshop entry on Home')
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

if not re.search(r'unlocked \? "' + re.escape(UNLOCKED_HELP) + r'"\s*:\s*"' + re.escape(LOCKED_HELP) + '"', launcher):
    fail('the bonus video help text does not follow the unlock state')

# One bar, one label: no step text on the shader screen or in the pack's status.
shaders_cpp = (src / 'native_shaders.cpp').read_text(encoding='utf-8')
for text, name in ((launcher, 'launcher.cpp'), (shaders_cpp, 'native_shaders.cpp')):
    for banned in ('Step {0} of {1}', 'StepText()', 'Compiling the {0} shaders', 'Preparing the {0} pipelines'):
        if banned in text:
            fail(f'{name} still shows steps: {banned}')
if 'native_pack_.ProgressLabel()' not in launcher or 'DrawnFraction(' not in launcher:
    fail('the shader screen does not draw the single eased bar')

# Settings' tabs (task 169): the settings pages in the owner's order, then About; the pause menu shares the
# settings pages (not About); Home keeps its About entry.
if layout_pages != ['Display', 'Graphics', 'Audio', 'Controls', 'Gameplay', 'Cheats', 'Advanced']:
    fail(f'settings_layout.h pages are {layout_pages}')
m = re.search(r'enum Page \{([^}]*)\};', launcher)
launcher_pages = [x.strip() for x in m.group(1).split(',')] if m else []
tabs = launcher_pages[launcher_pages.index('kDisplay'):launcher_pages.index('kAbout') + 1] if 'kAbout' in launcher_pages else []
if tabs != ['kDisplay', 'kGraphics', 'kAudio', 'kControls', 'kGameplay', 'kCheats', 'kAdvanced', 'kAbout']:
    fail(f'the launcher Settings tabs are {tabs}')
if 'constexpr Page kLastSettingsTab = kAbout;' not in launcher:
    fail('About is not the last Settings tab')
if 'e.push_back({"About", true, [this] { GoTo(kAbout); }});' not in launcher:
    fail('Home lost its About entry')
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
        ('{"Minimise and notify me", "Watch progress"}', 'the This can take a while buttons differ'),
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
if 'if (in_game_) return;' not in body or body.find('if (in_game_) return;') > body.find('"Installed"'):
    fail('the pause menu shows more than the texture pack switch')

# Job J4: the graphics pages are setup stages 8 to 11 after the cutscene step, drawn by launcher_setup_graphics.inc
# with the Settings rows (one source of truth), "Use recommended" and "change any time in Settings" on every page,
# and the close-ups loaded from setup-art beside the program (never in git).
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
        ('/ "setup-art"', 'close-ups beside the program'), ('"-original.png"', 'Original close-up'), ('"-new.png"', 'New close-up'),
        ('DisplayItems()', 'Display rows from Settings'), ('GraphicsItems()', 'Graphics rows from Settings'),
):
    if needle not in gfx:
        fail(f'{why} ({needle})')
if '—' in gfx or '–' in gfx:
    fail('dash in launcher_setup_graphics.inc')

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
        ('AskContentDownload(Kind::kTexturePack, 0);', 'the download asks first, as from Settings'),
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
