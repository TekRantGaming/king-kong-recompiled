# Writes one KKTX file per fetch constant of a texture census (tex_census.py --csv), with random texel data, so
# kknr_sdkref can check the layout and conversion of every texture the game binds against the SDK even where
# no memory dump of it exists (render-target resolves, textures the game makes at run time).
# Usage: python -I census_synth.py <tex_census.csv> <out dir> [seed]
import csv
import os
import random
import struct
import sys

# Block width, height and bytes for the formats a census can hold (index = format number); 0 = not handled.
BLOCKS = {2: (1, 1, 1), 6: (1, 1, 4), 10: (1, 1, 2), 18: (4, 4, 8), 19: (4, 4, 16), 20: (4, 4, 16), 22: (1, 1, 4),
          24: (1, 1, 2), 25: (1, 1, 4), 26: (1, 1, 8), 30: (1, 1, 2), 31: (1, 1, 4), 32: (1, 1, 8), 36: (1, 1, 4),
          37: (1, 1, 8), 38: (1, 1, 16), 49: (4, 4, 16), 59: (4, 4, 8)}


def align(v, a):
    return (v + a - 1) // a * a


def region_bound(w):
    """Upper bound of the base region (and of the mip region): whole 32x32-block tiles, every layer."""
    d0, d1, d2, d3, d4, d5 = w
    bw, bh, bpb = BLOCKS[d1 & 63]
    dim = (d5 >> 9) & 3
    if dim == 0:
        width, height, layers = (d2 & 0xFFFFFF) + 1, 1, 1
    elif dim == 2:
        width, height, layers = (d2 & 0x7FF) + 1, ((d2 >> 11) & 0x7FF) + 1, align((d2 >> 22) + 1, 4)
    else:
        width, height = (d2 & 0x1FFF) + 1, ((d2 >> 13) & 0x1FFF) + 1
        layers = 6 if dim == 3 else ((d2 >> 26) + 1 if (d1 >> 10) & 1 else 1)
    pitch = max(((d0 >> 22) & 0x1FF) * 32, width)
    per_layer = align(align((pitch + bw - 1) // bw, 32) * bpb * align((height + bh - 1) // bh, 32), 4096)
    return per_layer * layers


def main():
    rows = list(csv.DictReader(open(sys.argv[1])))
    out = sys.argv[2]
    rng = random.Random(int(sys.argv[3]) if len(sys.argv) > 3 else 1)
    os.makedirs(out, exist_ok=True)
    written = skipped = 0
    for i, r in enumerate(rows):
        w = [int(x, 16) for x in r['fc'].split()]
        if (w[1] & 63) not in BLOCKS:
            skipped += 1
            continue
        bound = region_bound(w)
        base = bound if (w[1] >> 12) else 0
        mips = bound if (w[5] >> 12) else 0
        header = struct.pack('<10I', 0x58544B4B, 2, *w, base, mips)
        name = 'syn_%03d_%s_%s.bin' % (i, r['format'], r['size'])
        with open(os.path.join(out, name), 'wb') as f:
            f.write(header)
            f.write(rng.randbytes(base + mips))
        written += 1
    print('%d written, %d skipped (format not handled)' % (written, skipped))


if __name__ == '__main__':
    main()
