"""Builds kk/src/<name>_icon.inc: a brand mark as a 64 x 64 alpha mask for the launcher.

The paths are the Discord and Steam icons from Simple Icons (https://simpleicons.org, CC0).
The brands' own rules still apply: each mark is drawn as it is, white on the brand's colour.
    python tools/make_brand_icon.py <discord.svg> discord
    python tools/make_brand_icon.py <steam.svg> steam
"""
import math
import os
import re
import sys

from PIL import Image, ImageChops, ImageDraw

SIZE, SCALE = 64, 8
NAME = sys.argv[2] if len(sys.argv) > 2 else "discord"
SYMBOL = "k" + NAME.capitalize() + "Icon"
OUT = os.path.join(os.path.dirname(__file__), "..", "kk", "src", f"{NAME}_icon.inc")

svg = open(sys.argv[1], encoding="utf-8").read()
d = re.search(r'<path d="([^"]+)"', svg).group(1)
view = 24.0

tokens = re.findall(r"[MmLlHhVvCcSsQqTtAaZz]|-?(?:\d+\.?\d*|\.\d+)(?:e-?\d+)?", d)
pos = 0


def num():
    global pos
    v = float(tokens[pos])
    pos += 1
    return v


def flag():
    # Arc flags are single digits and can be written against the next number
    # ("00" or "00.0842"): take one digit at a time.
    global pos
    t = tokens[pos]
    if len(t) > 1 and t[0] in "01":
        tokens[pos] = t[1:]
        return int(t[0])
    pos += 1
    return int(float(t))


def is_num():
    return pos < len(tokens) and not tokens[pos].isalpha()


def arc(p0, rx, ry, phi, large, sweep, p1):
    # SVG arc to points (endpoint to centre parametrisation).
    if rx == 0 or ry == 0:
        return [p1]
    cp, sp = math.cos(math.radians(phi)), math.sin(math.radians(phi))
    dx, dy = (p0[0] - p1[0]) / 2, (p0[1] - p1[1]) / 2
    x1, y1 = cp * dx + sp * dy, -sp * dx + cp * dy
    rx, ry = abs(rx), abs(ry)
    lam = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry)
    if lam > 1:
        rx, ry = rx * math.sqrt(lam), ry * math.sqrt(lam)
    num_ = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1
    den = rx * rx * y1 * y1 + ry * ry * x1 * x1
    co = math.sqrt(max(0.0, num_ / den)) * (-1 if large == sweep else 1)
    cx1, cy1 = co * rx * y1 / ry, -co * ry * x1 / rx
    cx = cp * cx1 - sp * cy1 + (p0[0] + p1[0]) / 2
    cy = sp * cx1 + cp * cy1 + (p0[1] + p1[1]) / 2

    def ang(ux, uy, vx, vy):
        return math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)

    t1 = ang(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry)
    dt = ang((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry)
    if not sweep and dt > 0:
        dt -= 2 * math.pi
    elif sweep and dt < 0:
        dt += 2 * math.pi
    pts = []
    n = max(4, int(abs(dt) * 16))
    for i in range(1, n + 1):
        t = t1 + dt * i / n
        x, y = rx * math.cos(t), ry * math.sin(t)
        pts.append((cp * x - sp * y + cx, sp * x + cp * y + cy))
    return pts


def cubic(p0, c1, c2, p1):
    pts = []
    for i in range(1, 25):
        t = i / 24
        u = 1 - t
        pts.append((u ** 3 * p0[0] + 3 * u * u * t * c1[0] + 3 * u * t * t * c2[0] + t ** 3 * p1[0],
                    u ** 3 * p0[1] + 3 * u * u * t * c1[1] + 3 * u * t * t * c2[1] + t ** 3 * p1[1]))
    return pts


subpaths, cur = [], []
x = y = sx = sy = 0.0
cmd = None
last_c2 = None
while pos < len(tokens):
    if not is_num():
        cmd = tokens[pos]
        pos += 1
        if cmd in "Zz":
            if cur:
                subpaths.append(cur)
            cur = []
            x, y = sx, sy
            last_c2 = None
            continue
    rel = cmd.islower()
    c = cmd.upper()
    ox, oy = (x, y) if rel else (0.0, 0.0)
    if c in "CS":
        if c == "C":
            c1 = (num() + ox, num() + oy)
        else:  # the first control point mirrors the last curve's second one
            c1 = (2 * x - last_c2[0], 2 * y - last_c2[1]) if last_c2 else (x, y)
        c2 = (num() + ox, num() + oy)
        p1 = (num() + ox, num() + oy)
        cur += cubic((x, y), c1, c2, p1)
        last_c2 = c2
        x, y = p1
        continue
    last_c2 = None
    if c == "M":
        if cur:
            subpaths.append(cur)
        x, y = num() + ox, num() + oy
        sx, sy = x, y
        cur = [(x, y)]
        cmd = "l" if rel else "L"
    elif c == "L":
        x, y = num() + ox, num() + oy
        cur.append((x, y))
    elif c == "H":
        x = num() + ox
        cur.append((x, y))
    elif c == "V":
        y = num() + oy
        cur.append((x, y))
    elif c == "A":
        rx, ry, phi = num(), num(), num()
        large, sweep = flag(), flag()
        p1 = (num() + ox, num() + oy)
        cur += arc((x, y), rx, ry, phi, large, sweep, p1)
        x, y = p1
    else:
        raise SystemExit(f"unsupported path command {cmd}")
if cur:
    subpaths.append(cur)

# Even-odd: each subpath flips the pixels it covers (the holes cut out of the shape).
big = SIZE * SCALE
mask = Image.new("L", (big, big), 0)
k = big / view
for sp_ in subpaths:
    layer = Image.new("L", (big, big), 0)
    ImageDraw.Draw(layer).polygon([(px * k, py * k) for px, py in sp_], fill=255)
    mask = ImageChops.logical_xor(mask.convert("1"), layer.convert("1")).convert("L")
small = mask.resize((SIZE, SIZE), Image.LANCZOS)
data = small.tobytes()
with open(OUT, "w", encoding="utf-8", newline="\n") as f:
    f.write(f"// {NAME.capitalize()}'s mark (Simple Icons, CC0) as a 64 x 64 alpha mask; made by tools/make_brand_icon.py.\n")
    f.write(f"constexpr int {SYMBOL}Size = {SIZE};\n")
    f.write(f"constexpr unsigned char {SYMBOL}[] = {{\n")
    for i in range(0, len(data), 24):
        f.write("    " + ", ".join(str(b) for b in data[i:i + 24]) + ",\n")
    f.write("};\n")
small.save(os.path.join(os.path.dirname(OUT), "..", "..", f"{NAME}_icon_preview.png"))
print(NAME, len(subpaths), "subpaths")
