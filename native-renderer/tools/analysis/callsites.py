# For given library functions: the engine's call sites and the argument registers set just before each call
# (constants only; a register loaded from memory shows as "mem", one copied from another as "=rN").
# Usage: python callsites.py <kk/generated/default> sub_X [sub_Y ...]
import glob
import os
import re
import sys

gen = sys.argv[1]
targets = sys.argv[2:]
func_re = re.compile(r'^DEFINE_REX_FUNC\((\w+)\)')
li_re = re.compile(r'ctx\.(r\d+)\.s64 = (-?\d+);')
addi_re = re.compile(r'ctx\.(r\d+)\.s64 = ctx\.(r\d+)\.s64 \+ (-?\d+);')
mov_re = re.compile(r'ctx\.(r\d+)\.u64 = ctx\.(r\d+)\.u64;')
load_re = re.compile(r'ctx\.(r\d+)\.u64 = REX_LOAD_U\d+\(ctx\.(r\d+)\.u32 \+ (-?\d+)\);')
asg_re = re.compile(r'^ctx\.(r\d+)\.')
sites = {t: [] for t in targets}
for path in sorted(glob.glob(os.path.join(gen, 'king_kong_recomp.*.cpp'))):
    cur = None
    regs = {}
    for l in open(path, encoding='utf-8', errors='replace'):
        m = func_re.match(l)
        if m:
            cur = m.group(1)
            regs = {}
            continue
        s = l.strip()
        if s.startswith('//'):
            continue
        for t in targets:
            if s == t + '(ctx, base);':
                args = []
                for r in ('r3', 'r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'r10'):
                    v = regs.get(r)
                    if isinstance(v, int):
                        args.append(f'{r}={v & 0xFFFFFFFF:X}')
                    elif isinstance(v, str):
                        args.append(f'{r}={v}')
                sites[t].append((cur, ' '.join(args)))
                regs = {}
                break
        else:
            m = li_re.search(s)
            if m:
                regs[m.group(1)] = int(m.group(2))
                continue
            m = addi_re.search(s)
            if m:
                v = regs.get(m.group(2))
                regs[m.group(1)] = (v + int(m.group(3))) if isinstance(v, int) else f'{m.group(2)}+{m.group(3)}'
                continue
            m = mov_re.search(s)
            if m:
                regs[m.group(1)] = regs.get(m.group(2), '=' + m.group(2))
                continue
            m = load_re.search(s)
            if m:
                regs[m.group(1)] = f'mem[{m.group(2)}+{m.group(3)}]'
                continue
            m = asg_re.match(s)
            if m:
                regs.pop(m.group(1), None)
for t in targets:
    print('==', t, len(sites[t]), 'sites')
    for cur, args in sites[t]:
        print('  ', cur, args)
