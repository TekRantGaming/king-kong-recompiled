# Summarises a resource census (KK_DEV_TEX_CENSUS, see gen_d3d_trace.py) into the format combinations the
# game uses: textures bound (fetch constants), render targets, resolves, index and vertex buffers.
# Usage: python census_report.py <census.txt> [more census files...]
import collections
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


def fetch(words):
    w = [int(x, 16) for x in words]
    d0, d1, d2, d3, d4, d5 = w
    dim = (d5 >> 9) & 3
    if dim == 0:
        size = ((d2 & 0xFFFFFF) + 1, 1, 1)
    elif dim == 2:
        size = ((d2 & 0x7FF) + 1, ((d2 >> 11) & 0x7FF) + 1, (d2 >> 22) + 1)
    else:
        size = ((d2 & 0x1FFF) + 1, ((d2 >> 13) & 0x1FFF) + 1, ((d2 >> 26) + 1) if (d1 >> 10) & 1 else 1)
    return {
        'type': d0 & 3,
        'signs': ''.join(SIGN[(d0 >> (2 + 2 * i)) & 3] for i in range(4)),
        'pitch': ((d0 >> 22) & 0x1FF) * 32,
        'tiled': d0 >> 31,
        'format': FORMATS[d1 & 63],
        'endian': ENDIAN[(d1 >> 6) & 3],
        'stacked': (d1 >> 10) & 1,
        'base': ((d1 >> 12) & 0x1FFFF) << 12,
        'size': size,
        'numformat': d3 & 1,
        'swizzle': ''.join(SWZ[(d3 >> (1 + 3 * i)) & 7] for i in range(4)),
        'exp_adjust': ((d3 >> 13) & 63) - (64 if (d3 >> 18) & 1 else 0),
        'mip_min': (d4 >> 2) & 15,
        'mip_max': (d4 >> 6) & 15,
        'dim': DIM[dim],
        'packed': (d5 >> 11) & 1,
        'mip': ((d5 >> 12) & 0x1FFFF) << 12,
    }


def d3dformat(v):
    """The 360 D3DFORMAT word (CreateRenderTarget / CreateTexture argument)."""
    return {
        'format': FORMATS[v & 63], 'endian': ENDIAN[(v >> 6) & 3], 'tiled': (v >> 8) & 1,
        'signs': ''.join(SIGN[(v >> (9 + 2 * i)) & 3] for i in range(4)), 'numformat': (v >> 17) & 1,
        'swizzle': ''.join(SWZ[(v >> (18 + 3 * i)) & 7] for i in range(4)),
    }


def fields(line):
    return dict(re.findall(r'(\w+)=((?:[0-9A-Fa-f.]+)(?: [0-9A-F]{8})*)', line))


def main():
    tex = collections.Counter()
    tex_sizes = collections.defaultdict(set)
    rts = collections.Counter()
    rsv = collections.Counter()
    ibs = collections.Counter()
    vbs = collections.Counter()
    locks = []
    first_t = {}
    for path in sys.argv[1:]:
        for line in open(path):
            kind = line.split(' ', 1)[0]
            f = fields(line)
            if kind == 'TEX':
                fc = fetch(f['fc'].split())
                key = (fc['format'], fc['endian'], 'tiled' if fc['tiled'] else 'linear', fc['dim'], fc['signs'],
                       fc['swizzle'], 'packed' if fc['packed'] else '-', 'mips' if fc['mip'] else 'nomips',
                       fc['numformat'], fc['exp_adjust'])
                tex[key] += 1
                tex_sizes[key].add(fc['size'])
                first_t.setdefault(key, f['t'])
            elif kind == 'RT':
                a = f['args'].split()
                w, h, fmt, ms = (int(x, 16) for x in a[:4])
                d = d3dformat(fmt)
                rts[(w, h, '%08X' % fmt, d['format'], d['endian'], d['tiled'], d['swizzle'], ms)] += 1
            elif kind == 'RSV':
                fc = fetch(f['fc'].split())
                rsv[(f['flags'], fc['format'], fc['endian'], fc['tiled'], fc['size'], fc['swizzle'])] += 1
            elif kind == 'IB':
                w = [int(x, 16) for x in f['words'].split()]
                ibs[('%08X' % (w[0] & 0xFFFF0000), 'size=%d' % w[4] if w[4] < 64 else 'size>=64')] += 1
            elif kind == 'VB':
                w = [int(x, 16) for x in f['words'].split()]
                vbs[('%08X' % (w[0] & 0xFFFF0000), 'type=%d' % (w[3] & 3), ENDIAN[w[4] & 3])] += 1
            elif kind in ('LOCK', 'UNLOCK'):
                locks.append((kind, f['lr'], f['words'].split()[0]))
    print('== Textures bound: (format, endian, tiling, dim, signs XYZW, swizzle, packed mips, mips, numformat, '
          'exp_adjust): distinct fetch constants, first seen (s), sizes')
    for k, n in sorted(tex.items(), key=lambda kv: -kv[1]):
        sizes = sorted(tex_sizes[k])
        shown = ' '.join('%dx%dx%d' % s for s in sizes[:6]) + (' ...' if len(sizes) > 6 else '')
        print('%5d  %-60s t=%s  %s' % (n, ' '.join(map(str, k)), first_t[k], shown))
    print('\n== Render targets created: (w, h, D3DFORMAT, format, endian, tiled, swizzle, msaa)')
    for k, n in sorted(rts.items(), key=lambda kv: -kv[1]):
        print('%5d  %s' % (n, k))
    print('\n== Resolves: (flags, dest format, endian, tiled, size, swizzle)')
    for k, n in sorted(rsv.items(), key=lambda kv: -kv[1]):
        print('%5d  %s' % (n, k))
    print('\n== Index buffers: (Common high half, size)')
    for k, n in sorted(ibs.items(), key=lambda kv: -kv[1]):
        print('%5d  %s' % (n, k))
    print('\n== Vertex buffers: (Common high half, fetch type, endian)')
    for k, n in sorted(vbs.items(), key=lambda kv: -kv[1]):
        print('%5d  %s' % (n, k))
    print('\n== Lock / Unlock sites: (kind, lr, Common)')
    for k in sorted(set(locks)):
        print('       %s' % (k,))


if __name__ == '__main__':
    main()
