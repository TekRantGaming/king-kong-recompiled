"""Inspect profile settings and check that UI edits preserve unrelated save bytes."""
import argparse
import struct
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('save', type=Path)
parser.add_argument('--universe', type=Path)
parser.add_argument('--before', type=Path)
parser.add_argument('--offsets', default='')
args = parser.parse_args()
data = args.save.read_bytes()
assert len(data) == 0x8040
assert struct.unpack_from('>I', data, 0x28)[0] != 0
if args.universe:
    universe = args.universe.read_bytes()
    for lo, hi in [(18680, 18732), (18684, 18716), (18700, 18716), (19848, 19876)]:
        needle = universe[lo:hi]
        hits = []
        at = data.find(needle, 0x100)
        while at >= 0:
            hits.append(hex(at)); at = data.find(needle, at+1)
        print(f'G[{lo:#x}:{hi:#x}] matches:', ', '.join(hits))
    for offset in (18684, 18692, 18700, 18704, 18708, 18712, 19856, 20456):
        word = struct.unpack_from('>I', universe, offset)[0]
        print(f'G+{offset:#x}: {word:#010x}')
    for offset in range(0xa40, 0xb20, 4):
        print(f'file+{offset:#x}: {struct.unpack_from(">I", data, offset)[0]:#010x}')
if args.offsets:
    offsets = [int(s, 0) for s in args.offsets.split(',')]
    assert len(offsets) == 5
    print('master, voices, music, effects:', [struct.unpack_from('>f', data, o)[0] for o in offsets[:4]])
    print('subtitles:', struct.unpack_from('>I', data, offsets[4])[0])
    if args.before:
        before = args.before.read_bytes()
        assert len(before) == len(data)
        allowed = {byte for o in offsets for byte in range(o, o+4)}
        changed = {i for i, (a, b) in enumerate(zip(before, data)) if a != b}
        assert changed <= allowed, f'unrelated save bytes changed: {sorted(changed-allowed)}'
        print('PASS: only the five option words changed; header and all other save data preserved')
