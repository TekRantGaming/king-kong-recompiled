# Finds image addresses (0x82xxxxxx) and known function entries in the dumped D3D device struct.
# Usage: python scan_device.py device_dump.bin callgraph.json
import json, struct, sys
d = open(sys.argv[1], 'rb').read()
g = json.load(open(sys.argv[2]))
funcs = {int(n[4:], 16): n for n in g if n.startswith('sub_') and len(n) == 12}
words = struct.unpack('>%dI' % (len(d) // 4), d[:len(d) // 4 * 4])
for i, w in enumerate(words):
    if 0x82000000 <= w < 0x830D0000:
        print(f'+{i*4:5d}: {w:08X} {funcs.get(w, "(image data)")}')
for off in (28, 308, 476, 944, 10376, 12532, 12536, 12552, 12808):
    print(f'field +{off}: {words[off//4]:08X}')
