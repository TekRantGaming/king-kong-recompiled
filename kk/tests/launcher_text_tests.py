"""Task 144 source checks for the launcher, no game or window needed:
- every settings heading and row in settings_layout.h, and the launcher's new page and shader texts, has a
  German, Spanish, French and Italian translation (placeholders kept, no em dashes);
- the online player count reminder can never open by itself (owner, 16 Oct): no code opens it;
- the shader screen shows one bar with one label: no step text is drawn;
- the bonus video modes' help text says unlocked or locked to match the save.
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
strings |= set(re.search(r'kPageNames\[kPageCount\] = \{([^}]*)\}', layout).group(1).replace('"', '').replace(' ', '').split(','))
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

if failures:
    print('\n'.join('FAIL: ' + f for f in failures))
    sys.exit(1)
print(f'{len(strings)} launcher strings translated; no online reminder; one shader bar')
