"""Edits Alfred Leete's 1914 "Britons wants you" poster (public domain) into
"Players: wants your shaders" for the King Kong launcher."""
import sys

import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont

SRC, OUT = sys.argv[1], sys.argv[2]
img = np.array(Image.open(SRC).convert("RGB"))
H, W = img.shape[:2]
FONTS = "C:/Windows/Fonts/"
RED = (232, 78, 80)
INK = (58, 52, 45)

# --- 1. find the old lettering --------------------------------------------
r, g, b = img[..., 0].astype(int), img[..., 1].astype(int), img[..., 2].astype(int)
lum = (r * 3 + g * 6 + b) // 10
red_text = (r - g > 70) & (r > 150)          # the red words
dark = lum < 150                             # ink: the figure and "WANTS YOU"
region = np.zeros((H, W), bool)
region[40:230, 30:620] = True                # BRITONS
region[720:900, 20:630] = True               # slogan, GOD SAVE THE KING, credits
words = np.zeros((H, W), bool)
words[555:705, 250:525] = True               # "WANTS YOU"

mask = (red_text & region) | (dark & region)
# In the "WANTS YOU" box take the dark ink, but not Kitchener's sleeve: drop any
# dark blob that reaches outside the box.
n, labels = cv2.connectedComponents((dark).astype(np.uint8), connectivity=8)
inside = np.zeros(n, bool)
for lab in np.unique(labels[words & dark]):
    ys, xs = np.nonzero(labels == lab)
    if ys.min() >= 555 and ys.max() < 705 and xs.min() >= 250 and xs.max() < 525:
        inside[lab] = True
mask |= inside[labels] & dark
mask[562:600, 268:310] |= dark[562:600, 268:310] & ~(labels[562:600, 268:310] == labels[600, 230])
mask = cv2.dilate(mask.astype(np.uint8) * 255, np.ones((7, 7), np.uint8))
# The letters' light inline sits inside them; fill whole letters by closing.
mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, np.ones((9, 9), np.uint8))
clean = cv2.inpaint(img, mask, 9, cv2.INPAINT_TELEA)
# Inpainting smooths away the paper's grain; add a little back where it worked.
rng = np.random.default_rng(1914)
grain = rng.normal(0, 2.2, clean.shape)
clean = np.where(mask[..., None] > 0, np.clip(clean + grain, 0, 255), clean).astype(np.uint8)
# Next to the sleeve the fill borrows its dark colour; use plain nearby paper there.
px = img[600:650, 540:600].reshape(-1, 3).mean(0)  # clean paper, right of the words
y0, y1, x0, x1 = 560, 612, 262, 360
yy, xx = np.mgrid[y0:y1, x0:x1]
feather = np.clip(np.minimum.reduce([xx - x0, x1 - xx, yy - y0, y1 - yy]) / 6.0, 0, 1)
patch = px + rng.normal(0, 2.2, (y1 - y0, x1 - x0, 3))
counts = np.bincount(labels.ravel(), weights=dark.ravel().astype(float))
counts[0] = 0
figure = labels[y0:y1, x0:x1] == int(np.argmax(counts))  # Kitchener: the largest dark shape
w = (feather * ~figure)[..., None]  # everything but the sleeve
clean[y0:y1, x0:x1] = (clean[y0:y1, x0:x1] * (1 - w) + patch * w).astype(np.uint8)
im = Image.fromarray(clean)


# --- 2. new lettering -------------------------------------------------------
def glyphs(text, font_file, box_w, box_h, stretch_y=1.0):
    """The text as a mask filling box_w x box_h (stretched taller if asked)."""
    size = 400
    while size > 6:
        f = ImageFont.truetype(FONTS + font_file, size)
        l, t, rr, bb = f.getbbox(text)
        if rr - l <= box_w and (bb - t) * stretch_y <= box_h:
            break
        size -= 1
    l, t, rr, bb = f.getbbox(text)
    m = Image.new("L", (rr - l + 8, bb - t + 8), 0)
    ImageDraw.Draw(m).text((4 - l, 4 - t), text, font=f, fill=255)
    if stretch_y != 1.0:
        m = m.resize((m.width, int(m.height * stretch_y)), Image.LANCZOS)
    return np.array(m)


def place(box, m, align="center"):
    x0, y0, x1, y1 = box
    x = x0 + ((x1 - x0) - m.shape[1]) // 2 if align == "center" else x0
    y = y0 + ((y1 - y0) - m.shape[0]) // 2
    return x, y


def paint(box, text, font_file, colour, stretch_y=1.0, align="center"):
    m = glyphs(text, font_file, box[2] - box[0], box[3] - box[1], stretch_y)
    x, y = place(box, m, align)
    a = Image.fromarray(m)
    im.paste(Image.new("RGB", a.size, colour), (x, y), a)


def engraved(box, text, font_file, stretch_y=1.0):
    """Leete's "YOU" style: mottled ink with a thin light line inside the edge.
    Drawn at 3x and scaled down so the edges stay smooth."""
    S = 3
    bw, bh = box[2] - box[0] - 6, box[3] - box[1] - 6
    m = glyphs(text, font_file, bw * S, bh * S, stretch_y)
    m = np.pad(m, 4 * S)
    solid = (m > 110).astype(np.uint8)
    body = cv2.dilate(solid, np.ones((2 * S + 1, 2 * S + 1), np.uint8))      # outline ~1 px
    a = cv2.erode(solid, np.ones((2 * S + 1, 2 * S + 1), np.uint8))
    b = cv2.erode(solid, np.ones((3 * S + 1, 3 * S + 1), np.uint8))
    line = (a > 0) & (b == 0)                                                  # ~0.5 px inline
    tex = cv2.GaussianBlur(rng.normal(0, 30, solid.shape), (0, 0), 2.5 * S)
    ink = np.clip(np.array(INK, float)[None, None] + 14 + tex[..., None], 0, 255)
    rgba = np.zeros(solid.shape + (4,), float)
    rgba[..., :3] = ink
    rgba[..., 3] = body * 255.0
    rgba[line] = (226, 192, 145, 255)
    big = Image.fromarray(rgba.astype(np.uint8), "RGBA")
    small = big.resize((big.width // S, big.height // S), Image.LANCZOS)
    x, y = place(box, np.zeros((small.height, small.width)))
    im.paste(small, (x, y), small)


paint((70, 72, 580, 214), "PLAYERS", "impact.ttf", RED, stretch_y=1.25)
paint((318, 566, 462, 588), "\u201cWANTS", "georgiab.ttf", INK)
engraved((276, 588, 516, 652), "YOUR", "georgiab.ttf", stretch_y=1.15)
engraved((276, 650, 516, 698), "SHADERS", "georgiab.ttf", stretch_y=1.05)
paint((508, 650, 528, 670), "\u201d", "georgiab.ttf", INK)
paint((58, 734, 590, 790), "SHARE THEM AFTER YOU PLAY!", "impact.ttf", RED, stretch_y=1.3)
paint((150, 796, 500, 826), "GOD SAVE THE KONG", "georgiab.ttf", RED)
paint((300, 840, 595, 856), "For your fellow recompatriots", "georgia.ttf", RED)

im.save(OUT)
print("saved", OUT)
