#!/usr/bin/env bash
# Builds the Linux AppImage (run inside Linux or WSL from the repository root).
#
# Needs the translated game code in kk/generated/default (made by setup.ps1 on
# Windows or `rexglue codegen` on Linux from your own disc). The code is the same
# for every platform, so a Windows checkout can be built here through /mnt/c.
#
#   tools/build_appimage.sh [version]
#
# Output: dist/KingKong-<version>-linux-x86_64.AppImage
set -euo pipefail

VERSION="${1:-dev}"
SDK_VERSION=0.10.0
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Build on the Linux filesystem: much faster than /mnt/c, and file names keep their case.
WORK="${KK_WORK:-$HOME/kk-linux-build}"

echo "== Packages"
if ! command -v clang++-20 >/dev/null; then
  sudo apt-get update
  sudo apt-get install -y wget gnupg lsb-release software-properties-common
  wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh && sudo bash /tmp/llvm.sh 20
fi
sudo apt-get install -y cmake ninja-build pkg-config file unzip rsync desktop-file-utils \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxinerama-dev libxss-dev \
  libxkbcommon-dev libwayland-dev libdecor-0-dev libegl-dev libgl-dev libvulkan-dev \
  libasound2-dev libpulse-dev libpipewire-0.3-dev libdbus-1-dev libudev-dev libgtk-3-dev

echo "== Source"
mkdir -p "$WORK"
rsync -a --delete --exclude out --exclude assets --exclude image.bin "$ROOT/kk/" "$WORK/kk/"
mkdir -p "$WORK/tools/rexglue"

echo "== ReXGlue SDK $SDK_VERSION (linux-amd64)"
if [ ! -d "$WORK/tools/rexglue/linux-amd64" ]; then
  wget -q -O /tmp/rexglue-linux.zip \
    "https://github.com/rexglue/rexglue-sdk/releases/download/v$SDK_VERSION/rexglue-sdk-$SDK_VERSION-linux-amd64.zip"
  unzip -q -o /tmp/rexglue-linux.zip -d "$WORK/tools/rexglue"
fi

echo "== Build"
cd "$WORK/kk"
cmake --preset kk-linux-release
cmake --build --preset kk-linux-release
BIN="$WORK/kk/out/build/kk-linux-release"

echo "== AppDir"
APPDIR="$WORK/AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib"
cp "$BIN/king_kong" "$APPDIR/usr/bin/"
# The runtime, the GPU plugin and anything else the build put beside the binary.
find "$BIN" -maxdepth 1 -name '*.so*' -exec cp -a {} "$APPDIR/usr/bin/" \;
for lib in librexruntime.so librexgpu-xenos.so libTracyClient.so; do
  [ -e "$BIN/$lib" ] || [ ! -e "$WORK/tools/rexglue/linux-amd64/lib/$lib" ] || \
    cp -a "$WORK/tools/rexglue/linux-amd64/lib/$lib" "$APPDIR/usr/bin/"
done
cp "$ROOT/docs/images/icon.png" "$APPDIR/king_kong.png"
cat > "$APPDIR/king_kong.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=King Kong
Comment=Peter Jackson's King Kong, Xbox 360 version, native PC port
Exec=king_kong
Icon=king_kong
Categories=Game;
Terminal=false
EOF
cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/bin:$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$HERE/usr/bin/king_kong" "$@"
EOF
chmod +x "$APPDIR/AppRun"

echo "== Missing libraries check"
LD_LIBRARY_PATH="$APPDIR/usr/bin" ldd "$APPDIR/usr/bin/king_kong" | grep "not found" || echo "none"

echo "== AppImage"
TOOL="$WORK/appimagetool-x86_64.AppImage"
[ -x "$TOOL" ] || { wget -q -O "$TOOL" \
  https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage && chmod +x "$TOOL"; }
mkdir -p "$ROOT/dist"
OUT="$ROOT/dist/KingKong-$VERSION-linux-x86_64.AppImage"
ARCH=x86_64 "$TOOL" --appimage-extract-and-run "$APPDIR" "$OUT"
ls -la "$OUT"
