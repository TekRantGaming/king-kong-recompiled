"""Task 208 source checks for photo mode, no game or window needed:
- every player string in photo_mode.cpp (rows, options, descriptions, prompts, the hint and the saved message)
  has German, Spanish, French and Italian translations, placeholders kept, no em or en dashes, no emoji;
- the pause menu offers Photo mode (only when photo mode is available);
- photo mode never writes a setting (no cvar is set from photo_mode.cpp: nothing is saved over the player's);
- the renderer leaves out only Jack's arms (no weapon or shader-only matching).
Usage: photo_text_tests.py <kk folder>"""
import re
import sys
from pathlib import Path

root = Path(sys.argv[1]).resolve()
src = root / 'src'
failures = []

STRING = r'"((?:[^"\\]|\\.)*)"'
tables = ''
for f in sorted(src.glob('*_text_table.inc')):
    tables += f.read_text(encoding='utf-8') + '\n'
entries = {}
for m in re.finditer(r'\{\s*' + r'\s*,\s*'.join([STRING] * 5) + r'\s*\}', tables):
    en, *rest = (bytes(g, 'utf-8').decode('unicode_escape').encode('latin-1').decode('utf-8') for g in m.groups())
    entries.setdefault(en, rest)

code = (src / 'photo_mode.cpp').read_text(encoding='utf-8')
panel = code[code.index('class Panel'):code.index('#if defined(KK_DEV_TOOLS)\n  // KK_DEV_PHOTO_STEPS')]
panel = re.sub(r'"\s*\n\s*"', '', panel)  # adjacent literals
needed = set()
for m in re.finditer(STRING, panel):
    s = m.group(1)
    if len(s) < 2 or s.startswith('##') or s.startswith('KK ') or re.fullmatch(r'[a-z_0-9%.*:/ -]+', s) or re.fullmatch(r'[0-9.:]+', s):
        continue
    if s in ('keyboard', 'auto', 'xbox360', 'xbox_series', 'ps5', 'ps2', 'steam_deck', 'kk_button_prompts', 'glyphs'):
        continue
    needed.add(s)
needed.add('Photo mode')
for s in sorted(needed):
    if s not in entries:
        failures.append(f'no translation: {s!r}')
        continue
    for lang, t in zip(('de', 'es', 'fr', 'it'), entries[s]):
        if not t.strip():
            failures.append(f'empty {lang}: {s!r}')
        if set(re.findall(r'\{\d\}', s)) != set(re.findall(r'\{\d\}', t)):
            failures.append(f'placeholders differ ({lang}): {s!r}')
photo_table = (src / 'photo_text_table.inc').read_text(encoding='utf-8')
for bad in ('—', '–'):
    if bad in photo_table or bad in code:
        failures.append('em or en dash in photo mode text')
if re.search('[\U0001F300-\U0001FAFF☀-➿]', photo_table + code):
    failures.append('emoji in photo mode text')

menu = (src / 'pause_menu.cpp').read_text(encoding='utf-8')
logic = (src / 'pause_menu_logic.h').read_text(encoding='utf-8')
# Task 222: the list comes from pause_menu_logic.h; Photo mode is listed when photo::Available().
if ('a.photo = photo::Available();' not in menu or 'if (a.photo) add(Entry::kPhotoMode);' not in logic or
        'case Entry::kPhotoMode: return photo::Enter();' not in menu):
    failures.append('the pause menu does not offer Photo mode when available')
if re.search(r'SetFlagByName|REXCVAR_SET|\bSet\(', code):
    failures.append('photo_mode.cpp sets a setting (photo mode must never save over the player\'s settings)')

renderer = (root.parent / 'native-renderer' / 'backend' / 'photo_config.cpp').read_text(encoding='utf-8')
table = renderer[renderer.index('kHidden[]'):renderer.index('};', renderer.index('kHidden[]'))]
if len(re.findall(r'0x[0-9A-F]{16}ull', table)) != 2 or 'arms' not in table:
    failures.append('the renderer must leave out only the two arm meshes')

for f in failures:
    print('FAIL', f)
print('PASS' if not failures else f'{len(failures)} failures', f'({len(needed)} strings checked)')
sys.exit(1 if failures else 0)
