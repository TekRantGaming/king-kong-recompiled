"""Audit every panel label, choice, description and translated template."""
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
source = (root / 'src/debug_overlay.cpp').read_text(encoding='utf-8')
table = (root / 'src/launcher_text_table.inc').read_text(encoding='utf-8')
table += (root / 'src/debug_overlay_text_table.inc').read_text(encoding='utf-8')
table += (root / 'src/hdr_text_table.inc').read_text(encoding='utf-8')
literal = r'"((?:[^"\\]|\\.)*)"'
entries = {}
for match in re.finditer(r'\{\s*' + literal + r'\s*,\s*' + literal + r'\s*,\s*' + literal +
                         r'\s*,\s*' + literal + r'\s*,\s*' + literal + r'\s*\}', table):
    entries.setdefault(match[1], match.groups())

keys = set(re.findall(r'\b[TF]\("([^"\n]*)"', source))
settings = source[source.index('const std::vector<Setting>& Settings()'):source.index('std::string Get(')]
for section, label in re.findall(r'\{"([^"]+)", "([^"]+)", "[^"]+", Kind::', settings):
    keys.update((section, label))
keys.update(re.findall(r'\{"([^"]+)", "[^"\n]+"\}', settings))
keys.update(re.findall(r'Show::\w+,\s*"([^"]+)"', settings))
# Variable or conditional arguments at the draw sites (identifiers and units
# such as DLSS, AO, GI, FXAA, GPU and ms are deliberately language invariant).
keys.update([
    'Shadows: their shader is being prepared.', "Shadows: nothing in view draws the sun's shadows here.",
    'Water reflections: their shader is being prepared.', 'Water reflections: no water in view.',
    'Original Xbox 360', 'Remastered', 'Your saved settings are back.',
    'Saved: these are now your settings, in the launcher too.', 'Could not save the settings.',
    'Shadows', 'Water', 'Shafts', 'Tessellation', 'Lighting', 'Shaders', 'Off', 'Low', 'Medium', 'High', 'Auto',
])
invariant = {'AO', 'GI', 'FXAA', 'J', 'K', 'L', 'M', '2x', '4x', '8x', '16x'}
missing = sorted(keys - entries.keys() - invariant)
assert not missing, 'Missing panel translations: ' + repr(missing)
for key in keys - invariant:
    row = entries[key]
    assert all(row), 'Empty translation: ' + key
    # Both named arguments and printf formats must preserve order and types.
    tokens = re.findall(r'\{\d+\}|%(?:\.\d+)?[suf]', row[0])
    for value in row[1:]:
        assert tokens == re.findall(r'\{\d+\}|%(?:\.\d+)?[suf]', value), 'Placeholder mismatch: ' + key
        assert '\u2014' not in value, 'Em dash in panel translation: ' + key
print(f'{len(keys)} panel text keys covered in all five languages')
