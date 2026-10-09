# Tables of function pointers in the game image (vtables and driver tables): runs of consecutive big-endian
# words that are entry addresses of known functions. Writes fptables.json next to this script.
# Usage: python fptables.py <kk/image.bin> <callgraph.json>
import json
import struct
import sys

IMAGE_BASE = 0x82000000
MIN_RUN = 6

image = open(sys.argv[1], 'rb').read()
graph = json.load(open(sys.argv[2]))
funcs = {int(n[4:], 16) for n in graph if n.startswith('sub_') and len(n) == 12}
words = struct.unpack('>%dI' % (len(image) // 4), image[:len(image) // 4 * 4])

tables = []
i = 0
while i < len(words):
    if words[i] in funcs:
        j = i
        while j < len(words) and (words[j] in funcs or words[j] == 0):
            j += 1
        # trim trailing zeros
        k = j
        while k > i and words[k - 1] == 0:
            k -= 1
        n = k - i
        if n >= MIN_RUN and sum(1 for w in words[i:k] if w in funcs) >= MIN_RUN:
            addr = IMAGE_BASE + i * 4
            entries = ['sub_%08X' % w if w else None for w in words[i:k]]
            lib = sum(1 for w in words[i:k] if 0x82108000 <= w < 0x82128000)
            tables.append({'addr': '%08X' % addr, 'count': n, 'in_d3d_lib': lib, 'entries': entries})
        i = j
    else:
        i += 1
json.dump(tables, open('fptables.json', 'w'), indent=0)
print(len(tables), 'tables')
for t in sorted(tables, key=lambda t: -t['count'])[:40]:
    print(t['addr'], 'count', t['count'], 'd3d-lib entries', t['in_d3d_lib'])
