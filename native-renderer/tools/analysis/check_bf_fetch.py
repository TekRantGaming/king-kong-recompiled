# Checks the fetch constants kknr_bfscan synthesizes from KKTextures.bf records against the ones the engine
# actually binds (the census CSV of tex_census.py): for each bound texture whose (format word bits, size,
# levels) shape has a dumped record, every field that does not depend on where memory was allocated or on
# sampler state must be equal, and the mip address must follow the base the same way (after the base's
# 4 KB aligned size, or 0).
# Usage: python -I check_bf_fetch.py <tex_census.csv> <bfdump dir>
import csv
import os
import struct
import sys
import collections

# Fields compared (mask per word): word 0 type, signs, pitch, tiled (clamps out); word 1 format, endian, request
# size, stacked (the nearest clamp policy and the address out); word 2 size; word 3 number format, swizzle,
# exp adjust (filters out); word 4 mip levels (filters and LOD bias out); word 5 dimension, packed (border and
# address out).
MASKS = [0xFFC003FF, 0x000007FF, 0xFFFFFFFF, 0x0007FFFF, 0x000003FC, 0x00000E00]


def shape(w):
    d2 = w[2]
    return (w[1] & 0x7FF, d2, (w[4] >> 6) & 15)


def main():
    census = [tuple(int(x, 16) for x in r['fc'].split()) for r in csv.DictReader(open(sys.argv[1]))]
    bf = {}
    for n in sorted(os.listdir(sys.argv[2])):
        h = struct.unpack('<10I', open(os.path.join(sys.argv[2], n), 'rb').read(40))
        bf.setdefault(shape(h[2:8]), (n, h[2:8], h[8]))
    stats = collections.Counter()
    for w in census:
        k = shape(w)
        if k not in bf:
            stats['no record of that shape'] += 1
            continue
        name, s, base_bytes = bf[k]
        diffs = [i for i in range(6) if (w[i] & MASKS[i]) != (s[i] & MASKS[i])]
        cb, cm = w[1] >> 12, w[5] >> 12
        sb, sm = s[1] >> 12, s[5] >> 12
        mip_rel_census = 'none' if cm == 0 else ('after base' if cm != cb else 'same')
        mip_rel_bf = 'none' if sm == 0 else ('after base' if sm != sb else 'same')
        # The engine's mip address relative to the base (in the 0xE0000000 view both are CPU addresses).
        gap = (cm - cb) * 4096 if cm else None
        exp_gap = (sm - sb) * 4096 if sm else None
        if diffs:
            stats['field differs'] += 1
            print('DIFF words %s census %s bf %s (%s)' % (diffs, ' '.join('%08X' % x for x in w),
                                                         ' '.join('%08X' % x for x in s), name))
        elif mip_rel_census != mip_rel_bf:
            stats['mip presence differs'] += 1
            print('MIPS census %s bf %s: %s' % (mip_rel_census, mip_rel_bf, ' '.join('%08X' % x for x in w)))
        elif gap is not None and gap != exp_gap:
            stats['fields equal, mips elsewhere'] += 1
        else:
            stats['equal'] += 1
    for k, n in stats.most_common():
        print('%5d  %s' % (n, k))


if __name__ == '__main__':
    main()
