"""Task 144 source checks for the launcher, no game or window needed:
- every settings heading and row in settings_layout.h, and the launcher's new page and shader texts, has a
  German, Spanish, French and Italian translation (placeholders kept, no em dashes);
- the online player count reminder can never open by itself (owner, 16 Oct): no code opens it;
- the shader screen shows one bar with one label: no step text is drawn;
- the bonus video modes' help text says unlocked or locked to match the save;
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
             'hdr_text_table.inc', 'launcher_layout_text_table.inc'):
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

if failures:
    print('\n'.join('FAIL: ' + f for f in failures))
    sys.exit(1)
print(f'{len(strings)} launcher strings translated; no online reminder; one shader bar')
