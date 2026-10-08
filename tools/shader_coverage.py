"""How much of the game's own shader list a shader cache has seen.

    python tools/shader_coverage.py <game folder or xeshaders.bin> <cache or .xsh> [<cache or .xsh> ...] [--missing]

The game ships every shader it was built with in Shaders/xeshaders.bin (an
'SDB2' database: the HLSL sources, then a table of 36-byte entries {kind,
vertex key (u64), pixel key (u64), extra (u64), offset, size}, all
big-endian; kind 1 = vertex shader, 2 = pixel shader). Each entry is an
Xbox 360 shader container whose header gives the microcode's offset (word 1)
and size (word 2). Pixel shaders reach the GPU unchanged, so a cache's pixel
shader records (keyed by XXH3 of their microcode) match the database
exactly. Vertex shaders don't: the console patches each one's vertex fetches
for the mesh it draws, so only pixel shaders are counted.

Each cache is a folder holding shaders/shareable (or the .xsh itself).
Prints coverage per shader family for each input and for all of them
together; --missing also lists the pixel keys nobody has seen.
"""
import os
import struct
import sys
from collections import Counter

import xxhash  # pip install xxhash

FAMILIES = ["generic", "PsAfterEffects", "PsShadow", "PsApplyShadow", "PsLightShaft", "PsWater", "PsSPG2",
            "PsHeatShimmer", "PsBlurShadow", "PsRain", "PsShadowQuad", "PsCompositeShadow", "PsBlurShadowPC", "PsFur",
            "PsSprite", "PsOcean", "PsReflection"]


def load_database(path):
    if os.path.isdir(path):
        path = os.path.join(path, "Shaders", "xeshaders.bin")
    d = open(path, "rb").read()
    magic, _, _, count, _, table = struct.unpack_from(">6I", d, 0)
    if magic != 0x32424453:
        raise SystemExit(f"{path}: not the game's shader database")
    pixel = {}  # XXH3 of microcode -> pixel key
    for i in range(count):
        kind, _, _, key_hi, key_lo, _, _, off, size = struct.unpack_from(">9I", d, table + i * 36)
        if kind != 2:
            continue
        ucode_off, ucode_size = struct.unpack_from(">2I", d, off + 4)
        ucode = d[off + ucode_off:off + ucode_off + ucode_size]
        pixel[xxhash.xxh3_64_intdigest(ucode)] = (key_hi << 32) | key_lo
    return pixel


def cache_pixel_hashes(path):
    if os.path.isdir(path):
        shareable = os.path.join(path, "shaders", "shareable")
        files = [os.path.join(shareable, n) for n in os.listdir(shareable) if n.endswith(".xsh")]
    else:
        files = [path]
    seen = set()
    for f in files:
        d = open(f, "rb").read()
        at = 8
        while at + 12 <= len(d):
            h, word = struct.unpack_from("<QI", d, at)
            count = word & 0x7FFFFFFF
            if word >> 31:
                seen.add(h)
            at += 12 + count * 4
    return seen


def report(name, pixel, seen):
    total, hit = Counter(), Counter()
    for h, key in pixel.items():
        family = key >> 56
        total[family] += 1
        hit[family] += h in seen
    all_hit, all_total = sum(hit.values()), sum(total.values())
    print(f"{name}: {all_hit} of {all_total} pixel shaders ({100 * all_hit / all_total:.1f}%)")
    for family in sorted(total):
        label = FAMILIES[family] if family < len(FAMILIES) else f"type {family}"
        print(f"   {label:20s} {hit[family]:5d} / {total[family]:5d}")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    pixel = load_database(args[0])
    union = set()
    for cache in args[1:]:
        seen = cache_pixel_hashes(cache)
        union |= seen
        report(cache, pixel, seen)
    if len(args) > 2:
        report("all together", pixel, union)
    if "--missing" in sys.argv:
        for h, key in sorted(pixel.items(), key=lambda kv: kv[1]):
            if h not in union:
                print(f"missing {key:016X}")


if __name__ == "__main__":
    main()
