"""Builds the next shader pack from players' "Share shaders" submissions.

    python tools/collect_shader_shares.py <current pack folder> <version> <out folder> [--min-players N]

Reads every open issue labelled "shaders" (needs the GitHub CLI, logged in),
downloads the .zip files attached to them, checks every shader and pipeline
record (format, size, and each pipeline's own hash), and adds to the current
pack only records that at least N different players sent (default 2). A
deliberately broken record would have to arrive from many accounts to get in.
Prints what each submission added; close the issues yourself once the new pack
is published (gh issue close <number> --comment "...").

Test the new pack with the dev build before uploading it.
"""
import argparse
import io
import json
import os
import re
import struct
import subprocess
import sys
import urllib.request
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_shader_pack import records  # noqa: E402

REPO = "TekRantGaming/king-kong-recompiled"
ATTACHMENT = re.compile(r"https://github\.com/(?:user-attachments/files|[\w.-]+/[\w.-]+/files)/\d+/[^\s)\]\"']+\.zip")
MAX_ZIP = 25 << 20


def load_pack(folder):
    """{file name: (header, {hash: record})} for an existing pack (or empty)."""
    pack = {}
    if not os.path.isdir(folder):
        return pack
    for name in os.listdir(folder):
        if not name.endswith((".xsh", ".xpso")):
            continue
        with open(os.path.join(folder, name), "rb") as f:
            data = f.read()
        header, recs = None, {}
        for h, rec in records(data):
            header = h
            recs[struct.unpack_from("<Q", rec)[0]] = rec
        pack[name] = (header, recs)
    return pack


def main():
    ap = argparse.ArgumentParser(description="Build the next shader pack from players' submissions.")
    ap.add_argument("current", help="folder holding the current pack")
    ap.add_argument("version", type=int, help="version number for the new pack")
    ap.add_argument("out", help="folder to write the new pack to")
    ap.add_argument("--min-players", type=int, default=2, help="different players needed per record (default 2)")
    a = ap.parse_args()
    current, version, out_dir, min_players = a.current, a.version, a.out, a.min_players
    pack = load_pack(current)

    issues = json.loads(subprocess.check_output(
        ["gh", "issue", "list", "-R", REPO, "--label", "shaders", "--state", "open", "--limit", "500",
         "--json", "number,author,body"]))
    seen = {}  # (file name, hash) -> {"players": set, "record": bytes, "header": bytes}
    report = []
    for issue in issues:
        player = issue["author"]["login"]
        urls = ATTACHMENT.findall(issue.get("body") or "")
        got, bad = 0, 0
        for url in urls:
            try:
                with urllib.request.urlopen(url, timeout=60) as r:
                    data = r.read(MAX_ZIP + 1)
                if len(data) > MAX_ZIP:
                    raise ValueError("too big")
                with zipfile.ZipFile(io.BytesIO(data)) as z:
                    for info in z.infolist():
                        name = os.path.basename(info.filename)
                        if not name.endswith((".xsh", ".xpso")) or info.file_size > MAX_ZIP:
                            continue
                        blob = z.read(info)
                        for header, rec in records(blob):
                            known = pack.get(name, (None, {}))[0]
                            if known is not None and header != known:
                                raise ValueError(f"{name} is from another runtime version")
                            key = (name, struct.unpack_from("<Q", rec)[0])
                            entry = seen.setdefault(key, {"players": set(), "record": rec, "header": header})
                            if entry["record"] != rec:
                                continue  # same hash, different bytes: ignore this copy
                            entry["players"].add(player)
                            got += 1
            except Exception as e:  # noqa: BLE001 - report and carry on with the others
                bad += 1
                print(f"#{issue['number']} ({player}): skipped {url}: {e}")
        report.append((issue["number"], player, len(urls), got, bad))

    added = {}
    for (name, h), entry in seen.items():
        header, recs = pack.setdefault(name, (entry["header"], {}))
        if h in recs or len(entry["players"]) < min_players:
            continue
        recs[h] = entry["record"]
        added[name] = added.get(name, 0) + 1

    os.makedirs(out_dir, exist_ok=True)
    lines = [f"version={version}"]
    for name, (header, recs) in sorted(pack.items()):
        with open(os.path.join(out_dir, name), "wb") as f:
            f.write(header)
            for rec in recs.values():
                f.write(rec)
        lines.append(name)
        print(f"{name}: {len(recs)} entries ({added.get(name, 0)} new)")
    with open(os.path.join(out_dir, "shader-pack.txt"), "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")

    waiting = sum(1 for (name, h), e in seen.items() if h not in pack.get(name, (None, {}))[1])
    print(f"\n{len(report)} submissions; {waiting} records still need more players to send them.")
    for number, player, files, got, bad in report:
        print(f"  #{number} {player}: {files} file(s), {got} records read, {bad} skipped")


if __name__ == "__main__":
    main()
