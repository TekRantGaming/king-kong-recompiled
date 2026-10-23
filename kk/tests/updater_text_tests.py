# Stable 1.9.13 (task 371): the new updater's screen and messages. Each is in the launcher's table in five
# languages, none has a dash of any kind or a hyphenated word (owner rule), update.cpp gives the messages exactly as
# the table has them (the launcher translates them as it draws), and the launcher shows the first start check.
# Usage: updater_text_tests.py <kk folder>
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
src = root / 'src'
failures = []

table_text = (src / 'launcher_text_table.inc').read_text(encoding='utf-8')
STRING = r'"((?:[^"\\]|\\.)*)"'
entries = {}
for m in re.finditer(r'\{\s*' + r'\s*,\s*'.join([STRING] * 5) + r'\s*\}', table_text):
    en, *rest = (bytes(g, 'utf-8').decode('unicode_escape').encode('latin-1').decode('utf-8') for g in m.groups())
    entries.setdefault(en, rest)

update_cpp = (src / 'update.cpp').read_text(encoding='utf-8')
launcher = (src / 'launcher.cpp').read_text(encoding='utf-8')
strings = [
    'Finishing the update',
    'Some files of version {0} are missing, so the launcher is downloading them again. Your installed game, saves and settings stay as they are.',
    'Installing the files.',
    'Every file of version {0} is now installed.',
    'The download did not pass its check, so nothing was changed. Try again later.',
    '{0} is in use by another program, so the update was undone and nothing was changed. Close that program and try again.',
    "The update's file list is damaged, so nothing was changed.",
    'The download failed. Check your internet connection and try again.',
    'Could not unpack the update.',
    "The update did not contain the game's program.",
    'Could not write the update into the game folder. Is it read only?',
    'Checking the download.', 'Connecting', 'Continue in the background', 'Update failed',
]
dash = re.compile(r'[‐-―−]|\w-\w| - ')
for s in strings:
    if len(entries.get(s, [])) != 4 or not all(entries[s]):
        failures.append(f'not translated: {s!r}')
    for tr in [s] + entries.get(s, []):
        if dash.search(tr):
            failures.append(f'dash or hyphen in {tr!r}')
# The messages update.cpp gives (C++ string literals may be split over lines: compare without the joins).
flat = re.sub(r'"\s*\n\s*"', '', update_cpp)
for s in strings[4:11]:
    if '"' + s.replace('"', '\\"') + '"' not in flat:
        failures.append(f'update.cpp does not give {s!r}')
for needle, why in (
    ('heading("Finishing the update");', 'the first start check has its screen'),
    ('entries.push_back({"Continue in the background", true, [this] { finish_hidden_ = true; }});', 'it can be left'),
    ('F(update_->message.c_str(), {update_->detail})', 'the failure names the file in use'),
    ('StartFinishCheck();', 'the first start check runs'),
    ('SetProtectedFolders({paths_.user_dir, paths_.game_dir});', 'user and game data are protected'),
    ('TickFinish();', 'the result of the first start check is reported'),
):
    if needle not in launcher:
        failures.append(f'launcher.cpp: {why} ({needle})')
# Asset pick: only a name ending in windows-x64.zip, and SHA256SUMS.txt.
for needle in ('EndsWith(name, "windows-x64.zip")', 'name == "SHA256SUMS.txt"'):
    if needle not in update_cpp:
        failures.append(f'update.cpp: {needle}')

for f in failures:
    print('FAIL', f)
print(f'updater text: {len(strings)} strings, {"FAILED" if failures else "all ok"}')
sys.exit(1 if failures else 0)
