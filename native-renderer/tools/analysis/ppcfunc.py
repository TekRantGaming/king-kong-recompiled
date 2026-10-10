# Prints the PowerPC of recompiled functions (the instruction comments in kk/generated/default), one
# instruction per line with its address, so a library function can be read without the C++ around it.
# The first run builds an index (function -> file, line) and caches it next to the output of --index.
# Usage: python ppcfunc.py <kk/generated/default> <index.json> sub_X [sub_Y ...]
#        python ppcfunc.py <kk/generated/default> <index.json> --grep <regex> sub_X ...   (matching lines only)
import glob
import json
import os
import re
import sys

gen, index_path = sys.argv[1], sys.argv[2]
args = sys.argv[3:]
pattern = None
if args and args[0] == '--grep':
    pattern = re.compile(args[1])
    args = args[2:]

func_re = re.compile(r'^DEFINE_REX_FUNC\((\w+)\)')
if os.path.exists(index_path):
    index = json.load(open(index_path))
else:
    index = {}
    for path in sorted(glob.glob(os.path.join(gen, 'king_kong_recomp.*.cpp'))):
        with open(path, encoding='utf-8', errors='replace') as f:
            for n, line in enumerate(f):
                m = func_re.match(line)
                if m:
                    index[m.group(1)] = [os.path.basename(path), n]
    json.dump(index, open(index_path, 'w'))

label_re = re.compile(r'^loc_([0-9A-F]{8}):')
for name in args:
    if name not in index:
        print('==', name, 'not found')
        continue
    fname, start = index[name]
    addr = int(name[4:], 16) if re.fullmatch(r'sub_[0-9A-F]{8}', name) else 0
    print('==', name)
    with open(os.path.join(gen, fname), encoding='utf-8', errors='replace') as f:
        for n, line in enumerate(f):
            if n <= start:
                continue
            if line.startswith('}'):
                break
            s = line.strip()
            m = label_re.match(s)
            if m:
                addr = int(m.group(1), 16)
                if not pattern:
                    print(f'  loc_{m.group(1)}:')
                continue
            if s.startswith('// '):
                text = s[3:]
                if pattern is None or pattern.search(text):
                    print(f'  {addr:08X}  {text}')
                addr += 4
