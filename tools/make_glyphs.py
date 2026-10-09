"""Builds kk/glyphs from Xelu's Free Controller & Key Prompts (CC0).

Download the pack from https://thoseawesomeguys.com/prompts/, unzip it, then:
    python tools/make_glyphs.py <unzipped pack folder>

Writes kk/glyphs/<system>/<slot>.png for each controller style and
kk/glyphs/keyboard/<key>.png named like the runtime's keybind names, trimmed to
their visible pixels and scaled down to 64 px.
"""
import os
import sys

from PIL import Image

PACK = sys.argv[1]
OUT = os.path.join(os.path.dirname(__file__), "..", "kk", "glyphs")
SIZE = 64

# Slot names match kk/src/glyphs.cpp.
SYSTEMS = {
    "xbox_series": ("Xbox Series", "XboxSeriesX_", {
        "a": "A", "b": "B", "x": "X", "y": "Y", "back": "View", "start": "Menu",
        "dpad": "Dpad", "dpad_up": "Dpad_Up", "stick": "Left_Stick", "stick_click": "Left_Stick_Click",
        "stick_move": "Left_Stick", "lt": "LT", "rt": "RT", "lb": "LB", "rb": "RB"}),
    "ps5": ("PS5", "PS5_", {
        "a": "Cross", "b": "Circle", "x": "Square", "y": "Triangle", "back": "Share", "start": "Options",
        "dpad": "Dpad", "dpad_up": "Dpad_Up", "stick": "Left_Stick", "stick_click": "Left_Stick_Click",
        "stick_move": "Left_Stick", "lt": "L2", "rt": "R2", "lb": "L1", "rb": "R1"}),
    # The PS2 and PS3 pads share the same coloured face symbols, Select and Start.
    "ps2": (os.path.join("Others", "PS3"), "PS3_", {
        "a": "Cross", "b": "Circle", "x": "Square", "y": "Triangle", "back": "Select", "start": "Start",
        "dpad": "Dpad", "dpad_up": "Dpad_Up", "stick": "Left_Stick", "stick_click": "Left_Stick_Click",
        "stick_move": "Left_Stick", "lt": "L2", "rt": "R2", "lb": "L1", "rb": "R1"}),
}

# Runtime keybind name -> pack file name, where they differ.
KEY_ALIASES = {
    "Return": "Enter", "NumpadEnter": "Enter", "Escape": "Esc", "Delete": "Del", "Control": "Ctrl",
    "Up": "Arrow_Up", "Down": "Arrow_Down", "Left": "Arrow_Left", "Right": "Arrow_Right",
    "LBracket": "Bracket_Left", "RBracket": "Bracket_Right", "Backtick": "Tilda", "PageUp": "Page_Up",
    "PageDown": "Page_Down", "CapsLock": "Caps_Lock", "NumLock": "Num_Lock", "PrintScreen": "Print_Screen",
    "NumpadPlus": "Plus", "NumpadMinus": "Minus", "NumpadStar": "Asterisk", "NumpadSlash": "Slash",
    "LMB": "Mouse_Left", "RMB": "Mouse_Right", "MMB": "Mouse_Middle",
}


def save(src, dst):
    im = Image.open(src).convert("RGBA")
    box = im.getchannel("A").point(lambda a: 255 if a > 8 else 0).getbbox()
    if box:
        im = im.crop(box)
    im.thumbnail((SIZE, SIZE), Image.LANCZOS)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    im.save(dst, optimize=True)


for system, (folder, prefix, slots) in SYSTEMS.items():
    for slot, name in slots.items():
        save(os.path.join(PACK, folder, f"{prefix}{name}.png"), os.path.join(OUT, system, f"{slot}.png"))

keys = os.path.join(PACK, "Keyboard & Mouse", "Dark")
names = [f[:-len("_Key_Dark.png")] for f in os.listdir(keys) if f.endswith("_Key_Dark.png")]
for name in names:
    save(os.path.join(keys, f"{name}_Key_Dark.png"), os.path.join(OUT, "keyboard", f"{name}.png"))
for key, name in KEY_ALIASES.items():
    if name in names:
        save(os.path.join(keys, f"{name}_Key_Dark.png"), os.path.join(OUT, "keyboard", f"{key}.png"))
for i in range(10):
    if str(i) in names:
        save(os.path.join(keys, f"{i}_Key_Dark.png"), os.path.join(OUT, "keyboard", f"Numpad{i}.png"))
print("done")
