"""Build locked/unlocked fixtures only inside a task-135 private run copy."""
from pathlib import Path
import hashlib
import struct
import sys

root = Path(sys.argv[1]).resolve()
allowed = Path("F:/KK-native-renderer/runs").resolve()
if not root.is_relative_to(allowed) or not root.parent.name.startswith("task135-") or root.name != "userdata":
    raise SystemExit("Refusing a path outside this task's private run userdata")
sources = list(root.glob("*/555307D3/00000001/KKProfileSlot*.sav/KKProfile.dat"))
source = next((p for p in sources if p.stat().st_size == 0x8040), None)
if source is None:
    raise SystemExit("No private source save")
original = source.read_bytes()
title = source.parents[2]
for slot, unlocked in enumerate((False, True), 1):
    data = bytearray(original)
    data[:32] = bytes(32)
    name = b"Extras unlocked" if unlocked else b"Extras locked"
    data[:len(name)] = name
    struct.pack_into(">I", data, 0x20, 2000 if unlocked else 1000)
    struct.pack_into(">I", data, 0x34, 41 if unlocked else 0)
    for i in range(40):
        struct.pack_into(">I", data, 0x38 + 4 * i, 250000 if unlocked and i == 0 else 0)
    # The saved online-code words. Also exercise a real persisted unlock flag in the copy.
    for i in range(20):
        struct.pack_into(">I", data, 0x168 + 4 * i, int(unlocked and i == 15))
    target = title / "00000001" / f"KKProfileSlot{slot}.sav" / "KKProfile.dat"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    print(target.parent.name, hashlib.sha256(data).hexdigest())
# The alternate-ending mask itself is recomputed, not stored: its fixture uses the
# original completion/250,000-point conditions, leaving the real source untouched.
