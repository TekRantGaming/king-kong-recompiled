#!/usr/bin/env bash
# Builds the Linux AppImage of the beta line, the same way every time (native-renderer/docs/linux.md).
# Run inside Linux, or on the Windows build PC inside WSL (wsl -d Ubuntu-24.04, never the default distro):
#
#   bash tools/build_linux_beta.sh <version>        e.g. v2.0.0-beta.6
#
# Steps (each is skipped when its result is already there and up to date):
#   1. toolchain check (lists missing Ubuntu packages; installs nothing)
#   2. ReXGlue SDK source = REXGLUE_SDK_SRC (tools/make_linux_sdk_base.sh, the same as the Windows -038 source: v0.10.0 + tools/rexglue-patches +
#      sdk-patches 0001-0003, 0035, 0037, 0038) + sdk-patches 0039, 0040 (Linux waits) + 0041 (present
#      observer) + 0043 (scRGB HDR) + 0044, 0045 (HDR curve, Beta 11), checked file by file against the Windows SDK's source (REXGLUE_SDK_CHECK)
#   3. SDK build + install (clang 20, Release, Vulkan)
#   4. DXC: the official Microsoft Linux release (same version as Windows' dxcompiler.dll)
#   5. FFmpeg: tools/build_ffmpeg_linux.sh from the SDK's own thirdparty/FFmpeg (LGPL, PS2 cutscenes)
#   6. tools/build_appimage.sh (game build, AppDir, game-data and library checks, appimagetool)
#   7. package checks + SHA-256 into dist/SHA256SUMS-linux.txt
#
# Files the AppImage ships that are not in git (the same files as the Windows zip): KK_PACKAGE_DIR = a staged
# Windows package folder (KingKong/) to take kkpipelines.list, kkresources.list, benchmark-route.kkbr and
# FREECAM.txt from; or each one by KK_PIPELINE_LIST / KK_RESOURCE_LIST / KK_EXTRA_FILES.
#
# Other settings: KK_LINUX_ROOT (work folder, default ~/kk-linux-beta), KK_DXC_FROM (an extracted DXC Linux
# release), KK_APPIMAGETOOL (appimagetool already on disk; otherwise build_appimage.sh downloads the official
# one), KK_JOBS (parallel compile jobs).
set -euo pipefail

VERSION="${1:?usage: build_linux_beta.sh <version, e.g. v2.0.0-beta.6>}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BASE="${KK_LINUX_ROOT:-$HOME/kk-linux-beta}"
F=/mnt/f/KK-native-renderer
SDK_FROM="${REXGLUE_SDK_SRC:-$BASE/sdk-base-038}"  # made by tools/make_linux_sdk_base.sh
SDK_CHECK="${REXGLUE_SDK_CHECK:-$F/sdk-src-real-hdr-317}"
DXC_FROM="${KK_DXC_FROM:-$F/downloads/dxc-linux-v1.9.2609/x}"
JOBS="${KK_JOBS:-$(( $(nproc) > 2 ? $(nproc) - 2 : 1 ))}"
# The game build (a developer build, KK_DEV_TOOLS=ON, gets its own, so the two never rebuild each other).
WORKDIR="${KK_BUILD_DIR:-$BASE/work$([ "${KK_DEV_TOOLS:-OFF}" != ON ] || echo -dev)}"
PATCHES="0039 0040 0041 0043 0044 0045 0046 0047"
# Linux only fixes the Windows SDK source does not have (its files are left out of the step 2 comparison):
# 0046 joins a finished thread once (threading_posix.cpp; the benchmark start hung).
LINUX_ONLY="0046 0047"
mkdir -p "$BASE"
T0=$(date +%s)
step() { echo "== [$(( $(date +%s) - T0 )) s] $*"; }

step "1. toolchain"
missing=""
for c in clang-20:clang-20 clang++-20:clang-20 lld-20:lld-20 cmake:cmake ninja:ninja-build nasm:nasm make:make \
         rsync:rsync patch:patch file:file pkg-config:pkg-config g++:g++; do
  command -v "${c%%:*}" >/dev/null || missing="$missing ${c#*:}"
done
for h in X11/Xlib.h X11/Xlib-xcb.h wayland-client.h xkbcommon/xkbcommon.h vulkan/vulkan.h alsa/asoundlib.h pulse/pulseaudio.h \
         dbus/dbus.h libudev.h gtk/gtk.h; do
  [ -e "/usr/include/$h" ] || [ -n "$(find /usr/include -path "*/$h" -print -quit 2>/dev/null)" ] || missing="$missing (header $h)"
done
if [ -n "$missing" ]; then
  echo "missing:$missing" >&2
  echo "Ubuntu 24.04: sudo apt install clang-20 lld-20 g++ cmake ninja-build nasm make rsync patch file pkg-config \\" >&2
  echo "  libx11-dev libx11-xcb-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxinerama-dev libxss-dev libxkbcommon-dev \\" >&2
  echo "  libwayland-dev libdecor-0-dev libegl-dev libgl-dev libvulkan-dev libasound2-dev libpulse-dev \\" >&2
  echo "  libpipewire-0.3-dev libdbus-1-dev libudev-dev libgtk-3-dev" >&2
  exit 1
fi

step "2. SDK source ($SDK_FROM + sdk-patches $PATCHES)"
SRC="$BASE/sdk-src"
STAMP="$(cd "$REPO/native-renderer/sdk-patches" && for n in $PATCHES; do sha256sum $n-*.patch; done | sha256sum | cut -c1-16)"
STAMP="$STAMP $(git -C "$SDK_FROM" rev-parse --short HEAD 2>/dev/null || cat "$SDK_FROM/.kk-base-id" 2>/dev/null || echo nogit)"
if [ "$(cat "$SRC/.kk-sdk-stamp" 2>/dev/null)" != "$STAMP" ]; then
  [ -d "$SDK_FROM/src" ] || { echo "error: no SDK source at $SDK_FROM (REXGLUE_SDK_SRC)" >&2; exit 1; }
  # out/ (the build) is kept. By content, not times: a changed file gets a new time, so the build compiles
  # exactly what changed (a copied older time would hide a change from ninja).
  rsync -rlpD --checksum --delete --exclude out --exclude .kk-sdk-stamp "$SDK_FROM/" "$SRC/"
  for n in $PATCHES; do
    p="$(ls "$REPO/native-renderer/sdk-patches/$n"-*.patch)"
    tr -d '\r' < "$p" > "$BASE/$n.patch"
    if patch -d "$SRC" -p1 --dry-run -s -f < "$BASE/$n.patch" >/dev/null 2>&1; then
      patch -d "$SRC" -p1 -s -f < "$BASE/$n.patch"; echo "  $n applied"
    elif patch -d "$SRC" -p1 -R --dry-run -s -f < "$BASE/$n.patch" >/dev/null 2>&1; then
      echo "  $n already in the source"
    else
      echo "error: $p does not apply to $SDK_FROM" >&2; exit 1
    fi
  done
  echo "$STAMP" > "$SRC/.kk-sdk-stamp"
fi
if [ -d "$SDK_CHECK/src" ]; then
  : > "$BASE/linux-only-files.txt"
  for n in $LINUX_ONLY; do
    tr -d '\r' < "$(ls "$REPO/native-renderer/sdk-patches/$n"-*.patch)" | sed -n 's|^+++ b/||p' >> "$BASE/linux-only-files.txt"
  done
  # Every file of the Windows SDK's source (its include/ and src/) must be the same here (line ends aside).
  diffs=0
  while IFS= read -r f; do
    grep -qx "$f" "$BASE/linux-only-files.txt" 2>/dev/null && continue
    if ! diff -q --strip-trailing-cr "$SDK_CHECK/$f" "$SRC/$f" >/dev/null 2>&1; then echo "  differs from Windows: $f"; diffs=$((diffs+1)); fi
  done < <(cd "$SDK_CHECK" && find include src -type f)
  [ $diffs -eq 0 ] || { echo "error: the Linux SDK source differs from $SDK_CHECK in $diffs files" >&2; exit 1; }
  echo "  same as the Windows SDK source ($SDK_CHECK) in every file it has"
else
  echo "  warning: no Windows SDK source to compare with ($SDK_CHECK)" >&2
fi

step "3. SDK build -> $BASE/sdk"
if [ ! -f "$SRC/out/build/linux-amd64/CMakeCache.txt" ]; then
  (cd "$SRC" && cmake --preset linux-amd64 -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 \
     -DCMAKE_INSTALL_PREFIX="$BASE/sdk" -DREXGLUE_USE_VULKAN=ON) > "$BASE/sdk-configure.log" 2>&1 ||
    { tail -20 "$BASE/sdk-configure.log"; exit 1; }
fi
cmake --build "$SRC/out/build/linux-amd64" --config Release -j "$JOBS" > "$BASE/sdk-build.log" 2>&1 ||
  { grep -E "error|FAILED" "$BASE/sdk-build.log" | head -20; exit 1; }
cmake --install "$SRC/out/build/linux-amd64" --config Release > "$BASE/sdk-install.log" 2>&1

step "4. DXC ($DXC_FROM)"
if [ ! -f "$BASE/dxc/lib/libdxcompiler.so" ]; then
  [ -f "$DXC_FROM/lib/libdxcompiler.so" ] || { echo "error: no DXC Linux release at $DXC_FROM (KK_DXC_FROM)" >&2; exit 1; }
  rsync -a "$DXC_FROM/" "$BASE/dxc/"
fi
# The licence files of the code inside libdxcompiler.so (the official repository's, at the release tag; the
# Linux archive itself has only LICENSE-MS and LICENSE-LLVM): KK_DXC_LICENCES.
DXC_LIC="${KK_DXC_LICENCES:-$F/downloads/dxc-licences-v1.9.2609}"
if [ -d "$DXC_LIC" ]; then mkdir -p "$BASE/dxc/licences" && cp "$DXC_LIC/"* "$BASE/dxc/licences/"
else echo "  warning: no DXC licence files at $DXC_LIC (KK_DXC_LICENCES)" >&2; fi
"$BASE/dxc/bin/dxc" --version 2>/dev/null | head -1 || true

step "5. FFmpeg (LGPL, PS2 cutscenes) -> $BASE/ffmpeg"
if ! ls "$BASE/ffmpeg/lib/"libavcodec-kk.so.* >/dev/null 2>&1; then
  bash "$REPO/tools/build_ffmpeg_linux.sh" "$SRC/thirdparty/FFmpeg" "$BASE/ffmpeg" > "$BASE/ffmpeg-build.log" 2>&1 ||
    { tail -20 "$BASE/ffmpeg-build.log"; exit 1; }
fi

step "6. game + AppImage (tools/build_appimage.sh $VERSION)"
PKG="${KK_PACKAGE_DIR:-}"
pick() { [ -n "$PKG" ] && [ -e "$PKG/$1" ] && echo "$PKG/$1" || true; }
export KK_PIPELINE_LIST="${KK_PIPELINE_LIST:-$(pick kkpipelines.list)}"
export KK_RESOURCE_LIST="${KK_RESOURCE_LIST:-$(pick kkresources.list)}"
if [ -z "${KK_EXTRA_FILES:-}" ]; then
  KK_EXTRA_FILES="$(pick benchmark-route.kkbr) $(pick FREECAM.txt)"
fi
export KK_EXTRA_FILES
export KK_EGG_ASAR="${KK_EGG_ASAR:-$(pick launcher/easter-egg/resources/app.asar)}"
export KK_EGG_ELECTRON_ZIP="${KK_EGG_ELECTRON_ZIP:-$F/downloads/electron-v45.0.0-linux-x64/electron-v45.0.0-linux-x64.zip}"
export KK_SKIP_PACKAGES=ON KK_WORK="$WORKDIR" REXGLUE_SDK_DIR="$BASE/sdk" KK_DXC_DIR="$BASE/dxc" \
       KK_FFMPEG_DIR="$BASE/ffmpeg" CMAKE_BUILD_PARALLEL_LEVEL="$JOBS" KK_DEV_TOOLS="${KK_DEV_TOOLS:-OFF}"
[ -z "${KK_APPIMAGETOOL:-}" ] || export KK_APPIMAGETOOL
# The AppImage runtime (the small program at the front of the file): KK_APPIMAGE_RUNTIME, or the one at the
# front of the appimagetool on disk, so appimagetool does not download it on every build.
RT="${KK_APPIMAGE_RUNTIME:-$BASE/runtime-x86_64}"
if [ ! -s "$RT" ] && [ -x "${KK_APPIMAGETOOL:-}" ]; then
  off="$("$KK_APPIMAGETOOL" --appimage-offset)" && head -c "$off" "$KK_APPIMAGETOOL" > "$RT"
fi
[ ! -s "$RT" ] || export KK_APPIMAGE_RUNTIME="$RT"
(cd "$REPO" && bash tools/build_appimage.sh "$VERSION") > "$BASE/appimage-build.log" 2>&1 ||
  { grep -E "error|FAILED" "$BASE/appimage-build.log" | head -30; exit 1; }
grep -E "^warning|native-renderer: upscalers" "$BASE/appimage-build.log" | sort -u || true

step "7. package checks"
OUT="$REPO/dist/KingKong-$VERSION-linux-x86_64.AppImage"
A="$WORKDIR/AppDir"
fail=0
for f in usr/bin/king_kong usr/bin/librexgpu-native.so usr/bin/librexruntime.so usr/bin/libdxcompiler.so usr/bin/libpdfium.so \
         usr/bin/THIRD-PARTY-NOTICES.txt usr/share/licenses/ffmpeg usr/share/licenses/dxc usr/bin/glyphs \
         usr/bin/launcher/easter-egg/LegallyDistinctGorilla usr/bin/launcher/easter-egg/resources/egg.asar \
         usr/bin/launcher/easter-egg/resources/app/main.js \
         usr/bin/launcher/video/save-notice-loop.mp4 usr/bin/launcher/video/returning-loop.mp4 usr/bin/launcher/video/trg-startup.mp4 AppRun; do
  [ -e "$A/$f" ] || { echo "  MISSING $f"; fail=1; }
done
ls "$A/usr/bin/"libavcodec-kk.so.* >/dev/null 2>&1 || { echo "  MISSING the FFmpeg libraries"; fail=1; }
for f in kkpipelines.list kkresources.list benchmark-route.kkbr FREECAM.txt; do
  [ -e "$A/usr/bin/$f" ] && echo "  has $f" || echo "  warning: no $f (KK_PACKAGE_DIR)"
done
# No frame generation (DECISIONS 11 Oct), no Windows files, no Classic renderer.
if find "$A" \( -iname '*dlssg*' -o -iname '*_fg*' -o -iname '*.dll' -o -iname '*.exe' -o -iname 'librexgpu-xenos*' \) | grep .; then
  echo "  error: files above must not ship"; fail=1
fi
grep -q "KK_DEV_TOOLS:BOOL=OFF" "$WORKDIR/kk/out/build/kk-linux-release/CMakeCache.txt" ||
  echo "  note: developer test aids are ON in this build (not for release)"
[ $fail -eq 0 ] || exit 1
SIZE=$(stat -c %s "$OUT")
SUM=$(sha256sum "$OUT" | cut -d' ' -f1)
echo "$SUM  $(basename "$OUT")" > "$REPO/dist/SHA256SUMS-linux.txt"
echo "  $OUT"
echo "  $SIZE bytes ($(( SIZE / 1048576 )) MiB), SHA-256 $SUM"
step "done"
