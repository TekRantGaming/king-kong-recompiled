"""Builds a shader pack from one or more shader caches.

    python tools/make_shader_pack.py <version> <out folder> <cache folder> [<cache folder> ...]

Each cache folder is a game cache (the one holding shaders/shareable), for
example Documents/king_kong/cache or one made with --cache_root. The records
are merged the same way the launcher merges a pack into a player's cache:
every distinct shader and pipeline once, keyed by its hash. Writes the
storage files and shader-pack.txt to <out folder>; upload them to the GitHub
release "shader-packs".
"""
import os
import struct
import sys

import xxhash  # pip install xxhash

SHADER_MAGIC, PIPELINE_MAGIC, PIPELINE_RECORD = 0x48534558, 0x53504558, 72


def records(data):
    magic = struct.unpack_from("<I", data)[0]
    if magic == SHADER_MAGIC:
        at, header = 8, data[:8]
        while at + 12 <= len(data):
            count = struct.unpack_from("<I", data, at + 8)[0] & 0x7FFFFFFF
            size = 12 + count * 4
            if at + size > len(data):
                break
            yield header, data[at:at + size]
            at += size
    elif magic == PIPELINE_MAGIC:
        header = data[:12]
        for at in range(12, len(data) - PIPELINE_RECORD + 1, PIPELINE_RECORD):
            rec = data[at:at + PIPELINE_RECORD]
            if xxhash.xxh3_64_intdigest(rec[8:]) != struct.unpack_from("<Q", rec)[0]:
                break  # damaged from here on, like the runtime does
            yield header, rec
    else:
        raise ValueError("not a shader storage file")


def main():
    version, out_dir, caches = int(sys.argv[1]), sys.argv[2], sys.argv[3:]
    merged = {}  # file name -> (header, {hash: record})
    for cache in caches:
        shareable = os.path.join(cache, "shaders", "shareable")
        for name in sorted(os.listdir(shareable)):
            with open(os.path.join(shareable, name), "rb") as f:
                data = f.read()
            header, recs = merged.setdefault(name, (None, {}))
            for h, rec in records(data):
                if header is None:
                    header = h
                elif h != header:
                    raise SystemExit(f"{cache}: {name} is from a different runtime version")
                recs.setdefault(struct.unpack_from("<Q", rec)[0], rec)
            merged[name] = (header, recs)
    os.makedirs(out_dir, exist_ok=True)
    lines = [f"version={version}"]
    for name, (header, recs) in sorted(merged.items()):
        with open(os.path.join(out_dir, name), "wb") as f:
            f.write(header)
            for rec in recs.values():
                f.write(rec)
        lines.append(name)
        print(f"{name}: {len(recs)} entries")
    with open(os.path.join(out_dir, "shader-pack.txt"), "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
