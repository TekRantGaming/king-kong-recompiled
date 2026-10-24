"""Task 220 source checks for the description panel's structured help (kk/src/settings_help.h), no game needed:
- every settings row in settings_layout.h has a help entry (HDR output's too, task 227), and every entry is
  a row, a help_key the launcher uses, or one of About's switches;
- every entry that has a "when it applies" label also has a performance label;
- every English string of the help (how it shows, the option notes, the impact labels) and the rows' rewritten
  first paragraphs has a German, Spanish, French and Italian translation, none with an em dash;
- the rows that can't change while playing (kRestartOnlyRows) are settings rows, and LiveOnly uses that list;
- a rough estimate of each panel's height (Segoe UI widths at 1280 x 720, the panel's 431 x 502 pixels) fits in
  every language with at most the panel's smaller text. The developer build's "desc-fit" step measures the real
  fonts (launcher.cpp, LogDescriptionFit).
Usage: settings_help_tests.py <kk folder>"""
import math
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
src = root / 'src'
failures = []


def fail(message):
    failures.append(message)


def dec(g):
    return bytes(g, 'utf-8').decode('unicode_escape').encode('latin-1').decode('utf-8')


STRING = r'"((?:[^"\\]|\\.)*)"'
table_text = ''.join((src / f).read_text(encoding='utf-8') + '\n' for f in sorted(p.name for p in src.glob('*_text_table.inc')))
entries = {}
for m in re.finditer(r'\{\s*' + r'\s*,\s*'.join([STRING] * 5) + r'\s*\}', table_text):
    en, *rest = (dec(g) for g in m.groups())
    entries.setdefault(en, rest)

help_src = (src / 'settings_help.h').read_text(encoding='utf-8')
body = help_src[help_src.index('kHelp[] = {'):help_src.index('inline const Help* Find')]
body = re.sub(r'"\s*\n\s*"', '', body)  # adjacent literals
if '—' in help_src:
    fail('em dash in settings_help.h')

# The entries: key, perf, vram, applies, how, notes.
helps = {}
for m in re.finditer(r'\{"([^"]+)", C::(\w+), C::(\w+), A::(\w+),\s*(nullptr|' + STRING + r'),\s*\{(.*?)\}\},?\s*(?://[^\n]*)?\n',
                     body, re.S):
    key, perf, vram, applies = m.group(1), m.group(2), m.group(3), m.group(4)
    how = dec(m.group(6)) if m.group(6) is not None else None
    notes = [(dec(a), dec(b)) for a, b in re.findall(r'\{' + STRING + r', ' + STRING + r'\}', m.group(7))]
    if key in helps:
        fail(f'help entry twice: {key}')
    helps[key] = (perf, vram, applies, how, notes)
declared = len(re.findall(r'\{"[^"]+", C::\w+, C::\w+, A::\w+,', body))
if declared != len(helps):
    fail(f'parsed {len(helps)} of {declared} help entries')

layout = (src / 'settings_layout.h').read_text(encoding='utf-8')
rows = [label for _, label in re.findall(r'\{"([^"]+)", "([^"*][^"]*)"\}', layout)]
for label in rows:
    if label not in helps:
        fail(f'no help for the settings row "{label}"')

launcher = (src / 'launcher.cpp').read_text(encoding='utf-8')
help_keys = set(re.findall(r'help_key = "([^"]+)"', launcher))
ABOUT = {'Show this launcher', 'Check for updates', 'Online player count', 'Discord status'}
for key in helps:
    if key not in rows and key not in help_keys and key not in ABOUT:
        fail(f'help entry "{key}" is neither a settings row nor a help_key the launcher uses')
for key in help_keys:
    if key not in helps:
        fail(f'help_key "{key}" has no help entry')
for key, (perf, vram, applies, how, notes) in helps.items():
    if applies != 'kNone' and perf == 'kUnset':
        fail(f'"{key}" says when it applies but has no performance label')
    if len(notes) > 7:
        fail(f'"{key}" has more than seven option notes')

# kRestartOnlyRows: settings rows (Renderer is the old renderer row), and LiveOnly greys by that list.
restart = re.findall(r'"([^"]+)"', help_src[help_src.index('kRestartOnlyRows[] = {'):help_src.index('inline bool RestartOnly')])
for label in restart:
    if label not in rows and label != 'Renderer':
        fail(f'kRestartOnlyRows names "{label}", not a settings row')
if 'help::RestartOnly(it.label)) Disable(it, true, kInLauncherOnly);' not in launcher:
    fail('LiveOnly does not grey the rows of kRestartOnlyRows')
if 'AttachHelp(items);' not in launcher or launcher.count('AttachHelp(items);') < 3:
    fail('the launcher Settings, the pause menu and the desc-fit check must attach the help')

# The strings to translate.
strings = set()
for key, (perf, vram, applies, how, notes) in helps.items():
    if how:
        strings.add(how)
    for _, note in notes:
        strings.add(note)
strings |= set(re.findall(r'return "([^"]+)";', help_src))
strings |= set(re.findall(r'inline constexpr const char\* k\w+ = "([^"]+)";', help_src))
FIRST = [
    "Which display the game opens on.", "What the game's 16:9 picture does on a screen of another shape.",
    "Makes each frame wait for your screen's next refresh.",
    "Readings in the top left corner while you play. F2 switches between them.",
    "How many pictures the game draws a second. Your screen's refresh rate is separate.",
    "Original keeps the Xbox 360 look. Remastered unlocks the graphics settings below.",
    "How the renderer talks to your graphics card.",
    "How many pixels the game draws before the upscaler rebuilds the picture at your screen's size.",
    "How many pixels the game draws before the upscaler scales them to your screen.",
    "How dark the shading gets and how far it spreads from each corner.", "How many samples the shading uses.",
    "How far from the camera bounced light is worked out.", "How sharp and detailed the game's shadows are.",
    "How the water reflects its surroundings.",
    "Some levels (Necropolis, Brontosaurus) blur the whole picture slightly.", "The haze over far away scenery.",
]
for s in FIRST:
    if s not in launcher:
        fail(f'first paragraph not in launcher.cpp: {s}')
strings |= set(FIRST)
for s in sorted(strings):
    t = entries.get(s)
    if not t:
        fail(f'no translation: {s}')
        continue
    for lang, text in zip(('de', 'es', 'fr', 'it'), t):
        if not text:
            fail(f'empty {lang} translation: {s}')
        if '—' in text:
            fail(f'em dash in the {lang} translation of: {s}')
        if sorted(re.findall(r'\{\d\}', text)) != sorted(re.findall(r'\{\d\}', s)):
            fail(f'{lang} placeholders differ: {s}')
    if '—' in s:
        fail(f'em dash in: {s}')

# The rows' own first paragraphs where the launcher writes them in one literal (for the estimate).
first = {}
for m in re.finditer(r'Cvar(?:Choice|Toggle|Slider)\(\s*"([^"]+)",\s*((?:"(?:[^"\\]|\\.)*"\s*)+),', launcher):
    first.setdefault(m.group(1), ''.join(dec(x) for x in re.findall(STRING, m.group(2))))
for m in re.finditer(r'it\.label = "([^"]+)";\s*(?:[^\n]*\n){0,3}?\s*it\.description\s*=\s*((?:"(?:[^"\\]|\\.)*"\s*)+);', launcher):
    first.setdefault(m.group(1), ''.join(dec(x) for x in re.findall(STRING, m.group(2))))


# A rough height in pixels: average Segoe UI widths, word wrapping, the panel's sizes and gaps (launcher_ui.cpp).
def lines(text, size, width, em=0.5):
    per_line = max(1, int(width / (size * em)))
    n, cur = 1, 0
    for word in text.split():
        if cur and cur + 1 + len(word) > per_line:
            n, cur = n + 1, len(word)
        else:
            cur += (1 if cur else 0) + len(word)
    return n


def height(label, desc, how, notes, chips, k):
    W, lh = 431, 1.33
    y = lines(label, 25, W, 0.52) * 25 * lh + 12
    bs = 15.5 * k
    if desc:
        y += lines(desc, bs, W) * bs * lh + 10 * k
    if how:
        y += lines(how, bs, W) * bs * lh + 10 * k
    y += 2 * k + 2 * bs * lh  # room for two facts lines
    if notes:
        os_ = 14 * k
        col = min(max(len(n) for n, _ in notes) * os_ * 0.55 + 14 * k, W * 0.36)
        for name, note in notes:
            y += max(lines(name, os_, col - 10 * k, 0.55), lines(note, os_, W - col)) * os_ * lh + 6 * k
        y += 6 * k
    if chips:
        width = sum(16 + 16 + len(c) * 13 * k * 0.5 for c in chips) + 6 * (len(chips) - 1)
        y += math.ceil(width / W) * (22 * k + 6) + 12 * k
    return y + 14 * k * lh


AVAIL = 502
langs = ('en', 'de', 'es', 'fr', 'it')
worst = (0, '')
for key, (perf, vram, applies, how, notes) in helps.items():
    label = key.split(' (')[0]
    desc = first.get(label, 'x' * 120)  # unknown: about two lines
    chips = [c for c in (f'Performance impact: {perf[1:]}' if perf != 'kUnset' else '',
                         f'VRAM: {vram[1:]}' if vram != 'kUnset' else '', 'Applies when the game starts') if c]
    for i, lang in enumerate(langs):
        tr = (lambda s: s) if i == 0 else (lambda s, i=i: (entries.get(s) or [s] * 4)[i - 1] or s)
        args = (tr(label), tr(desc), tr(how) if how else None, [(tr(n), tr(t)) for n, t in notes], [tr(c) for c in chips])
        h = height(*args, 1.0)
        if h > worst[0]:
            worst = (h, f'{key} ({lang})')
        if height(*args, 0.87) > AVAIL:
            fail(f'"{key}" in {lang} would not fit the panel even with smaller text (about {int(h)} px of {AVAIL})')

if failures:
    print('\n'.join('FAIL: ' + f for f in failures))
    sys.exit(1)
print(f'{len(helps)} help entries, {len(strings)} strings translated; tallest panel about {int(worst[0])} px: {worst[1]}')
