# The texture census (brief 03): every (format, endian, tiling, dimension, size, mips, signs, swizzle)
# combination of the textures the game binds, from any mix of
#   - resource census files (KK_DEV_TEX_CENSUS, "TEX ... fc=<6 words>" lines),
#   - stream 01 trace logs (KK_DEV_D3D_TRACE object dumps, "KK d3d obj: sub_82118F78 r5=... n=40 <10 words>":
#     the texture object passed to SetTexture, header then fetch constant),
# and, with --bf <listing>, the texture records a "kknr_bfscan KKTextures.bf" listing shows.
# Usage: python -I tex_census.py [--bf LISTING] [--csv FILE] LABEL=FILE [LABEL=FILE ...]
# A texture is one distinct fetch constant (all six words; the object's copy has no sampler state).
import collections
import csv
import re
import sys

FORMATS = ['k_1_REVERSE', 'k_1', 'k_8', 'k_1_5_5_5', 'k_5_6_5', 'k_6_5_5', 'k_8_8_8_8', 'k_2_10_10_10', 'k_8_A',
           'k_8_B', 'k_8_8', 'k_Cr_Y1_Cb_Y0_REP', 'k_Y1_Cr_Y0_Cb_REP', 'k_16_16_EDRAM', 'k_8_8_8_8_A', 'k_4_4_4_4',
           'k_10_11_11', 'k_11_11_10', 'k_DXT1', 'k_DXT2_3', 'k_DXT4_5', 'k_16_16_16_16_EDRAM', 'k_24_8',
           'k_24_8_FLOAT', 'k_16', 'k_16_16', 'k_16_16_16_16', 'k_16_EXPAND', 'k_16_16_EXPAND',
           'k_16_16_16_16_EXPAND', 'k_16_FLOAT', 'k_16_16_FLOAT', 'k_16_16_16_16_FLOAT', 'k_32', 'k_32_32',
           'k_32_32_32_32', 'k_32_FLOAT', 'k_32_32_FLOAT', 'k_32_32_32_32_FLOAT', 'k_32_AS_8', 'k_32_AS_8_8',
           'k_16_MPEG', 'k_16_16_MPEG', 'k_8_INTERLACED', 'k_32_AS_8_INTERLACED', 'k_32_AS_8_8_INTERLACED',
           'k_16_INTERLACED', 'k_16_MPEG_INTERLACED', 'k_16_16_MPEG_INTERLACED', 'k_DXN',
           'k_8_8_8_8_AS_16_16_16_16', 'k_DXT1_AS_16_16_16_16', 'k_DXT2_3_AS_16_16_16_16',
           'k_DXT4_5_AS_16_16_16_16', 'k_2_10_10_10_AS_16_16_16_16', 'k_10_11_11_AS_16_16_16_16',
           'k_11_11_10_AS_16_16_16_16', 'k_32_32_32_FLOAT', 'k_DXT3A', 'k_DXT5A', 'k_CTX1', 'k_DXT3A_AS_1_1_1_1',
           'k_8_8_8_8_GAMMA_EDRAM', 'k_2_10_10_10_FLOAT_EDRAM']
ENDIAN = ['none', '8in16', '8in32', '16in32']
DIM = ['1D', '2D', '3D', 'cube']
SIGN = 'uSbg'  # unsigned, signed, unsigned biased, gamma
SWZ = 'XYZW01??'


def decode(w):
    d0, d1, d2, d3, d4, d5 = w
    dim = (d5 >> 9) & 3
    stacked = (d1 >> 10) & 1
    if dim == 0:
        size = '%d' % ((d2 & 0xFFFFFF) + 1)
    elif dim == 2:
        size = '%dx%dx%d' % ((d2 & 0x7FF) + 1, ((d2 >> 11) & 0x7FF) + 1, (d2 >> 22) + 1)
    else:
        size = '%dx%d' % ((d2 & 0x1FFF) + 1, ((d2 >> 13) & 0x1FFF) + 1)
        if stacked:
            size += 'x%d' % ((d2 >> 26) + 1)
    base = (d1 >> 12) != 0
    mip = (d5 >> 12) != 0
    lo, hi = (d4 >> 2) & 15, (d4 >> 6) & 15
    return {
        'format': FORMATS[d1 & 63],
        'endian': ENDIAN[(d1 >> 6) & 3],
        'tiling': 'tiled' if d0 >> 31 else 'linear',
        'dim': DIM[dim] + (' stacked' if stacked and dim == 1 else ''),
        'size': size,
        'pitch': ((d0 >> 22) & 0x1FF) * 32,
        'levels': '%d-%d' % (lo, hi) if hi else '0',
        'packed': 'packed' if (d5 >> 11) & 1 else '-',
        'storage': ('base' if base else '') + ('+mips' if mip else ''),
        'signs': ''.join(SIGN[(d0 >> (2 + 2 * i)) & 3] for i in range(4)),
        'swizzle': ''.join(SWZ[(d3 >> (1 + 3 * i)) & 7] for i in range(4)),
        'numformat': 'int' if d3 & 1 else 'frac',
        'exp_adjust': ((d3 >> 13) & 63) - (64 if (d3 >> 18) & 1 else 0),
    }


TEX_RE = re.compile(r'^TEX .* fc=((?:[0-9A-F]{8} ?){6})')
OBJ_RE = re.compile(r'KK d3d obj: sub_82118F78 r5=[0-9A-F]{8} n=40 ((?:[0-9A-F]{8} ?){10})')


def read_textures(path):
    out = set()
    for line in open(path, errors='replace'):
        m = TEX_RE.match(line)
        if m:
            out.add(tuple(int(x, 16) for x in m.group(1).split()))
            continue
        m = OBJ_RE.search(line)
        if m:
            out.add(tuple(int(x, 16) for x in m.group(1).split()[4:10]))
    return out


def main():
    args = sys.argv[1:]
    bf = None
    csv_path = None
    sources = []
    i = 0
    while i < len(args):
        if args[i] == '--bf':
            bf = args[i + 1]
            i += 2
        elif args[i] == '--csv':
            csv_path = args[i + 1]
            i += 2
        else:
            label, _, path = args[i].partition('=')
            sources.append((label, path))
            i += 1
    seen = collections.defaultdict(set)  # fetch words -> labels
    for label, path in sources:
        for w in read_textures(path):
            seen[w].add(label)
    labels = [l for l, _ in sources]

    # Per texture rows.
    rows = []
    for w, ls in seen.items():
        d = decode(w)
        d['fc'] = ' '.join('%08X' % x for x in w)
        d['scenes'] = ' '.join(l for l in labels if l in ls)
        rows.append(d)
    if csv_path:
        with open(csv_path, 'w', newline='') as f:
            cols = ['format', 'endian', 'tiling', 'dim', 'size', 'pitch', 'levels', 'packed', 'storage', 'signs',
                    'swizzle', 'numformat', 'exp_adjust', 'scenes', 'fc']
            wr = csv.DictWriter(f, fieldnames=cols, extrasaction='ignore')
            wr.writeheader()
            for r in sorted(rows, key=lambda r: (r['format'], r['size'])):
                wr.writerow(r)

    print('%d distinct textures bound in %d sources (%s)\n' % (len(rows), len(sources), ', '.join(labels)))
    # Combination table: (format, endian, tiling, dimension) with what varies inside.
    combos = collections.defaultdict(list)
    for r in rows:
        combos[(r['format'], r['endian'], r['tiling'], r['dim'])].append(r)
    print('| Format | Endian | Tiling | Dim | Textures | Sizes | Levels | Packed | Storage | Signs | Swizzles | '
          'Scenes |')
    print('|---|---|---|---|---|---|---|---|---|---|---|---|')
    for k, rs in sorted(combos.items(), key=lambda kv: -len(kv[1])):
        def vals(field, limit=6):
            c = collections.Counter(r[field] for r in rs)
            items = [v for v, _ in c.most_common()]
            return ' '.join(str(v) for v in items[:limit]) + (' ...(%d)' % len(items) if len(items) > limit else '')
        sc = collections.Counter(s for r in rs for s in r['scenes'].split())
        print('| %s | %s | %s | %s | %d | %s | %s | %s | %s | %s | %s | %s |' % (
            k[0], k[1], k[2], k[3], len(rs), vals('size'), vals('levels'), vals('packed'), vals('storage'),
            vals('signs'), vals('swizzle'), ' '.join(l for l in labels if sc[l])))
    other = collections.Counter((r['numformat'], r['exp_adjust']) for r in rows)
    print('\nnumber format / exp_adjust: ' + ', '.join('%s %d: %d' % (k[0], k[1], n) for k, n in other.items()))

    if bf:
        rec = re.compile(r'^pack (\w+) \+(\w+)\s+(\d+)x(\d+)\s+fmt (\w+) (\S+)\s+(\S+)\s+(\S+)\s+extra (\w+) (\w+) (\w+) (\w+)')
        bfc = collections.defaultdict(lambda: [0, collections.Counter(), collections.Counter(), collections.Counter()])
        for line in open(bf):
            m = rec.match(line)
            if not m:
                continue
            k = (m.group(6), m.group(7), m.group(8), m.group(5))
            e = bfc[k]
            e[0] += 1
            e[1]['%sx%s' % (m.group(3), m.group(4))] += 1
            e[2][int(m.group(9), 16)] += 1
            e[3]['packed' if m.group(10) != '0000FFFF' else '-'] += 1
        print('\n== KKTextures.bf records: (format, endian, tiling, D3DFORMAT) count, sizes, level counts, packed')
        print('| Format | Endian | Tiling | D3DFORMAT | Records | Sizes (most common) | Levels | Packed |')
        print('|---|---|---|---|---|---|---|---|')
        for k, e in sorted(bfc.items(), key=lambda kv: -kv[1][0]):
            print('| %s | %s | %s | %s | %d | %s | %s | %s |' % (
                k[0], k[1], k[2], k[3], e[0], ' '.join(s for s, _ in e[1].most_common(8)),
                ' '.join('%d' % s for s, _ in sorted(e[2].items())), ' '.join('%s %d' % kv for kv in e[3].items())))


if __name__ == '__main__':
    main()
