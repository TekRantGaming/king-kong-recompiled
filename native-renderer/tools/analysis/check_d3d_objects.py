# Checks the D3D object layouts in docs/d3d-structs.md against the object dumps of a KK_DEV_D3D_TRACE log
# ("KK d3d obj:" / "KK d3d ret:" lines, written by kk/src/dev_d3d_trace.cpp). Prints, per object kind, how
# many dumps passed each check and a summary of the decoded fields.
# Usage: python -I check_d3d_objects.py <log> [<log> ...]
import re
import struct
import sys
from collections import Counter, defaultdict

line_re = re.compile(r'KK d3d (obj|ret): (sub_[0-9A-F]{8}) (r\d+)=([0-9A-F]{8}) n=(\d+) ([0-9A-F ]+)')

# (function, register) -> object kind
KIND = {
    ('sub_82118F78', 'r5'): 'texture', ('sub_82116178', 'r6'): 'texture', ('sub_82118A68', 'r3'): 'texture',
    ('sub_8210BD38', 'r5'): 'vertex_buffer', ('sub_821092B0', 'r3'): 'vertex_buffer',
    ('sub_8210BE38', 'r4'): 'index_buffer', ('sub_821093E0', 'r3'): 'index_buffer',
    ('sub_8210C378', 'r5'): 'surface', ('sub_8210C6E0', 'r4'): 'surface', ('sub_82118B88', 'r3'): 'surface',
    ('sub_82118838', 'r3'): 'surface', ('sub_82118900', 'r3'): 'surface', ('sub_8210BEB8', 'r3'): 'surface',
    ('sub_8210BF00', 'r3'): 'surface',
    ('sub_82111E68', 'r4'): 'declaration', ('sub_82110C90', 'r3'): 'declaration',
    ('sub_821108B8', 'r4'): 'vertex_shader', ('sub_82111CA0', 'r3'): 'vertex_shader',
    ('sub_82110C28', 'r4'): 'pixel_shader', ('sub_82111D90', 'r3'): 'pixel_shader',
    ('sub_8210BAC8', 'r4'): 'viewport',
}


def words(b):
    return struct.unpack('>%dI' % (len(b) // 4), b[:len(b) // 4 * 4])


checks = defaultdict(Counter)
info = defaultdict(Counter)
fails = defaultdict(list)


def check(kind, name, ok, detail=''):
    checks[kind][(name, ok)] += 1
    if not ok and len(fails[kind]) < 5:
        fails[kind].append(f'{name}: {detail}')


def texture(w):
    common = w[0]
    check('texture', 'type 3 in Common bits 16-18', (common >> 16) & 7 == 3, f'{common:08X}')
    check('texture', 'reference count > 0', common & 0xFF > 0, f'{common:08X}')
    check('texture', 'fetch word 0 type 2', w[4] & 3 == 2, f'{w[4]:08X}')
    dim = (w[9] >> 9) & 3
    check('texture', 'dimension 1-3', dim in (1, 2, 3), f'{w[9]:08X}')
    check('texture', 'base address set', w[5] >> 12 != 0, f'{w[5]:08X}')
    fmt = w[5] & 0x3F
    if dim == 1:
        size = ((w[6] & 0x1FFF) + 1, ((w[6] >> 13) & 0x1FFF) + 1)
    elif dim == 2:
        size = ((w[6] & 0x7FF) + 1, ((w[6] >> 11) & 0x7FF) + 1, (w[6] >> 22) + 1)
    else:
        size = ((w[6] & 0x1FFF) + 1, ((w[6] >> 13) & 0x1FFF) + 1)
    levels = ((w[8] >> 6) & 0xF) + 1
    info['texture'][f'format {fmt} dim {dim} levels {levels}'] += 1
    info['texture size'][str(size)] += 1


def vertex_buffer(w):
    check('vertex_buffer', 'type 1', (w[0] >> 16) & 7 == 1, f'{w[0]:08X}')
    check('vertex_buffer', 'word +12 type bits = 3', w[3] & 3 == 3, f'{w[3]:08X}')
    check('vertex_buffer', 'word +16 endian = 2', w[4] & 3 == 2, f'{w[4]:08X}')
    info['vertex_buffer'][f'size {((w[4] >> 2) & 0xFFFFFF) * 4} bytes'] += 1


def index_buffer(w):
    check('index_buffer', 'type 2', (w[0] >> 16) & 7 == 2, f'{w[0]:08X}')
    check('index_buffer', 'address set', w[3] != 0, f'{w[3]:08X}')
    info['index_buffer']['32-bit' if w[0] >> 31 else '16-bit'] += 1


def surface(w):
    check('surface', 'type 4', (w[0] >> 16) & 7 == 4, f'{w[0]:08X}')
    level = (w[0] >> 25) & 1
    if level:
        check('surface', 'texture level has a parent at +44', w[11] != 0, f'{w[11]:08X}')
        info['surface']['texture level'] += 1
    else:
        width = (w[6] & 0x1FFF) + 1
        height = ((w[6] >> 13) & 0x1FFF) + 1
        pitch = w[12] & 0x3FFF
        check('surface', 'EDRAM: pitch >= width', pitch >= width, f'pitch {pitch} width {width}')
        check('surface', 'EDRAM: size bytes = tiles * 5120', w[10] % 5120 == 0, f'{w[10]}')
        info['surface'][f'EDRAM {width}x{height} fmt {w[5] & 0x3F} base {w[13] & 0xFFF} msaa {(w[12] >> 16) & 3}'] += 1


def declaration(b, w):
    count = w[2]
    check('declaration', 'count < 16', count < 16, str(count))
    els = []
    for i in range(min(count, (len(b) - 36) // 12)):
        e = b[36 + 12 * i: 48 + 12 * i]
        stream, offset, typ = struct.unpack('>HHI', e[:8])
        method, usage, index = e[8], e[9], e[10]
        check('declaration', 'stream <= highest stream (+12)', stream <= w[3], f'{stream} > {w[3]}')
        els.append(f'{stream}:{offset}:{typ:X}:{usage}.{index}')
    info['declaration'][' '.join(els)] += 1


def shader(kind, w, header):
    check(kind, 'reference count > 0', w[1] > 0, f'{w[1]:08X}')
    c = header // 4
    if len(w) > c + 3:
        virt, phys = w[c + 1], w[c + 2]
        check(kind, 'container copy: virtual size > 0', 0 < virt < 0x100000, f'{virt:08X}')
        check(kind, 'container copy: physical size > 0', 0 < phys < 0x100000, f'{phys:08X}')
        info[kind][f'container flags {w[c]:08X}'] += 1
    addr = w[3] if kind == 'vertex_shader' else w[10]
    check(kind, 'microcode address set', addr != 0, f'{addr:08X}')


for path in sys.argv[1:]:
    for line in open(path, encoding='utf-8', errors='replace'):
        m = line_re.search(line)
        if not m:
            continue
        _, fn, reg, addr, n, hexs = m.groups()
        kind = KIND.get((fn, reg))
        b = bytes.fromhex(hexs.replace(' ', ''))
        w = words(b)
        if kind == 'texture':
            texture(w)
        elif kind == 'vertex_buffer':
            vertex_buffer(w)
        elif kind == 'index_buffer':
            index_buffer(w)
        elif kind == 'surface':
            surface(w)
        elif kind == 'declaration':
            declaration(b, w)
        elif kind == 'vertex_shader':
            shader(kind, w, 52)
        elif kind == 'pixel_shader':
            shader(kind, w, 592)
        elif kind == 'viewport':
            info['viewport'][f'{w[0]} {w[1]} {w[2]} {w[3]} {struct.unpack(">f", b[16:20])[0]} {struct.unpack(">f", b[20:24])[0]}'] += 1

for kind in sorted(checks):
    print('==', kind)
    names = sorted({k[0] for k in checks[kind]})
    for nm in names:
        print(f'   {nm}: {checks[kind][(nm, True)]} pass, {checks[kind][(nm, False)]} fail')
    for f in fails[kind]:
        print('   FAIL', f)
for kind in sorted(info):
    print('--', kind, len(info[kind]), 'distinct')
    for k, v in info[kind].most_common(12):
        print(f'   {v:5d}  {k}')
