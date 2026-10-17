"""Xvfb's framebuffer file (Xvfb -fbdir <dir>: <dir>/Xvfb_screen0, XWD format) -> PNG, optionally scaled.
Usage: python3 -I xwd2png.py <Xvfb_screen0> <out.png> [scale]"""
import struct
import sys

from PIL import Image

src, out = sys.argv[1], sys.argv[2]
scale = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
with open(src, 'rb') as f:
    data = f.read()
h = struct.unpack('>25I', data[:100])
header_size, width, height = h[0], h[4], h[5]
byte_order, bpp, bpl, ncolors = h[7], h[11], h[12], h[19]
pix = header_size + ncolors * 12
if bpp != 32:
    sys.exit('unsupported bits per pixel %d' % bpp)
mode = 'BGRX' if byte_order == 0 else 'XRGB'
img = Image.frombuffer('RGB', (width, height), data[pix:pix + bpl * height], 'raw', mode, bpl, 1)
if scale != 1.0:
    img = img.resize((int(width * scale), int(height * scale)), Image.BILINEAR)
img.save(out)
