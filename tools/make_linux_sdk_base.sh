#!/usr/bin/env bash
# Makes the ReXGlue SDK source the Linux recipe starts from (tools/build_linux_beta.sh, REXGLUE_SDK_SRC): the
# same source as the Windows -038 copy (removed from the build PC on 25 Oct), rebuilt from files that stay:
#   C:\rexsrc (v0.10.0 + tools/rexglue-patches 0001-0012 as commits) + rexglue-patches 0015-0017
#   + native-renderer/sdk-patches 0001-0003, 0035, 0037, 0038.
# build_linux_beta.sh then adds 0039 to 0045 and compares every file with the Windows SDK's source.
#
#   bash tools/make_linux_sdk_base.sh [output folder, default ~/kk-linux-beta/sdk-base-038]
set -euo pipefail
R="$(cd "$(dirname "$0")/.." && pwd)"
REXSRC="${KK_REXSRC:-/mnt/c/rexsrc}"
D="${1:-$HOME/kk-linux-beta/sdk-base-038}"
[ -d "$REXSRC/src" ] || { echo "error: no ReXGlue source at $REXSRC (KK_REXSRC)" >&2; exit 1; }
rm -rf "$D"; mkdir -p "$D"
rsync -a --exclude /out --exclude /.git "$REXSRC/" "$D/"  # submodules keep their .git files (the SDK checks them)
ap() {
  tr -d '\r' < "$1" > "$D.patch"
  if patch -d "$D" -p1 --dry-run -s -f < "$D.patch" >/dev/null 2>&1; then
    patch -d "$D" -p1 -s -f < "$D.patch"; echo "  $(basename "$1") applied"
  elif patch -d "$D" -p1 -R --dry-run -s -f < "$D.patch" >/dev/null 2>&1; then
    echo "  $(basename "$1") already in the source"
  else
    echo "error: $1 does not apply" >&2; exit 1
  fi
}
for n in 0015 0016 0017; do ap "$(ls "$R/tools/rexglue-patches/$n"-*.patch)"; done
for n in 0001 0002 0003 0035 0037 0038; do ap "$(ls "$R/native-renderer/sdk-patches/$n"-*.patch)"; done
rm -f "$D.patch"
# A new id each time, so build_linux_beta.sh copies the source again (its step 2 stamp).
date +%s.%N > "$D/.kk-base-id"
echo "SDK source ready: $D"
