"""Inspect shader-pack instructions for the unexpected depth-comparison VUID."""
import json
from pathlib import Path
import struct
import sys

data = Path(sys.argv[1]).read_bytes()
u32 = lambda at: struct.unpack_from("<I", data, at)[0]
u64 = lambda at: struct.unpack_from("<Q", data, at)[0]
assert u32(0) == 0x50534B4B and u32(4) == 1
count, index = u32(16), u64(24)
dref_ops = {89, 90, 93, 94, 97, 307, 308, 311, 312, 315}
matches = []
modules = 0
for i in range(count):
    rec = index + i * 32
    start, size = u64(rec + 16), u32(rec + 24)
    assert u32(start) == 0x45534B4B
    nb, nd, ns, nh = (u32(start + n) for n in (40, 44, 48, 52))
    assert 64 + nb + nd + ns + nh == size
    if not ns:
        continue
    words = struct.unpack_from(f"<{ns // 4}I", data, start + 64 + nb + nd)
    assert words[0] == 0x07230203
    pos = 5
    while pos < len(words):
        length, op = words[pos] >> 16, words[pos] & 65535
        assert length and pos + length <= len(words)
        if op in dref_ops:
            matches.append({"entry": i, "ucode": f"{u64(rec):016X}", "opcode": op, "word": pos})
        pos += length
    assert pos == len(words)
    modules += 1
print(json.dumps({"entries": count, "spirv_modules": modules, "dref_instructions": matches}, indent=2))
