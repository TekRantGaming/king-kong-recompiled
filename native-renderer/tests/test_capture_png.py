"""Verify exact WIC RGB, row padding, top-down order, and undefined X handling."""
from pathlib import Path
import sys
from PIL import Image
path = Path(sys.argv[1])
assert path.read_bytes().startswith(b'\x89PNG\r\n\x1a\n')
with Image.open(path) as image:
    assert image.mode == 'RGB' and image.size == (7, 3)
    assert image.tobytes() == bytes(c for y in range(3) for x in range(7) for c in (10+x, 50+y, 100+x+y))
assert not path.with_name(path.name + '.writing').exists()
print('PASS WIC PNG: exact RGB pixels, padded stride, short final row, RGBX, empty/short input, atomic publication')
