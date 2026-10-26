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
#
# PS2 cutscene playback needs the small LGPL FFmpeg libraries (tools/build_ffmpeg_linux.sh):
# KK_FFMPEG_DIR=<their install folder>, or KK_FFMPEG_SRC=<an FFmpeg 4.4 source tree, e.g. the ReXGlue SDK's
# thirdparty/FFmpeg> to build them here. Without either, the AppImage has no PS2 cutscenes.
set -euo pipefail

VERSION="${1:-dev}"
SDK_VERSION=0.10.0
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Build on the Linux filesystem: much faster than /mnt/c, and file names keep their case.
WORK="${KK_WORK:-$HOME/kk-linux-build}"

echo "== Packages"
if [ "${KK_SKIP_PACKAGES:-OFF}" != ON ]; then
sudo apt-get update
sudo apt-get install -y wget gnupg lsb-release ca-certificates
if ! command -v clang++-20 >/dev/null && ! sudo apt-get install -y clang-20 lld-20; then
  # Older releases: clang 20 from the official LLVM repository.
  CODENAME="$(lsb_release -cs)"
  wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | sudo gpg --dearmor --yes -o /usr/share/keyrings/llvm.gpg
  echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] http://apt.llvm.org/$CODENAME/ llvm-toolchain-$CODENAME-20 main" |
    sudo tee /etc/apt/sources.list.d/llvm-20.list
  sudo apt-get update
  sudo apt-get install -y clang-20 lld-20
fi
sudo apt-get install -y g++ cmake ninja-build pkg-config file unzip rsync desktop-file-utils \
  libx11-dev libx11-xcb-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxinerama-dev libxss-dev \
  libxkbcommon-dev libwayland-dev libdecor-0-dev libegl-dev libgl-dev libvulkan-dev \
  libasound2-dev libpulse-dev libpipewire-0.3-dev libdbus-1-dev libudev-dev libgtk-3-dev nasm
fi

echo "== Source"
mkdir -p "$WORK"
rsync -aL --delete --exclude out --exclude assets --exclude image.bin "$ROOT/kk/" "$WORK/kk/"
rsync -a --delete --exclude out --exclude build "$ROOT/native-renderer/" "$WORK/native-renderer/"
mkdir -p "$WORK/tools/rexglue" "$WORK/kk/assets"
cp "$ROOT/CHANGELOG.md" "$WORK/"  # the launcher's What's new and version history are built in
cp "$ROOT/MODDING.md" "$WORK/"  # the modding guide ships beside the program (task 200)
# The user manual's PDFium (task 333): tools/pdfium/linux-x64, or KK_PDFIUM_DIR as kk/CMakeLists.txt reads it.
if [ -f "$ROOT/tools/pdfium/linux-x64/lib/libpdfium.so" ]; then
  mkdir -p "$WORK/tools/pdfium" && rsync -aL --delete "$ROOT/tools/pdfium/linux-x64/" "$WORK/tools/pdfium/linux-x64/"
fi
# The build checks the translated code against your default.xex (from your own disc).
cp "$ROOT/kk/assets/default.xex" "$WORK/kk/assets/"

# The SDK: the official v$SDK_VERSION release, or REXGLUE_SDK_DIR=<an install> (the SDK built from
# source with tools/rexglue-patches, as the Windows release ships it).
SDK="$WORK/tools/rexglue/linux-amd64"
WANT="${REXGLUE_SDK_DIR:-official-$SDK_VERSION}"
if [ -n "${REXGLUE_SDK_DIR:-}" ]; then
  # A local build may have changed since the last run: refresh the copy every time.
  echo "== ReXGlue SDK from $REXGLUE_SDK_DIR"
  mkdir -p "$SDK"
  rsync -a --delete "$REXGLUE_SDK_DIR/" "$SDK/"
  echo "$WANT" > "$SDK/.kk-sdk-source"
elif [ "$(cat "$SDK/.kk-sdk-source" 2>/dev/null)" != "$WANT" ]; then
  rm -rf "$SDK"
  echo "== ReXGlue SDK $SDK_VERSION (linux-amd64)"
  wget -q -O /tmp/rexglue-linux.zip \
    "https://github.com/rexglue/rexglue-sdk/releases/download/v$SDK_VERSION/rexglue-sdk-$SDK_VERSION-linux-amd64.zip"
  unzip -q -o /tmp/rexglue-linux.zip -d "$WORK/tools/rexglue"
  echo "$WANT" > "$SDK/.kk-sdk-source"
fi

# The PS2 cutscenes' decoder libraries (LGPL FFmpeg, kk/src/ps2_movie_decoder_ffmpeg.cpp).
FFMPEG_DIR="${KK_FFMPEG_DIR:-}"
if [ -z "$FFMPEG_DIR" ] && [ -n "${KK_FFMPEG_SRC:-}" ]; then
  echo "== FFmpeg for the PS2 cutscenes from $KK_FFMPEG_SRC"
  FFMPEG_DIR="$WORK/ffmpeg-kk"
  bash "$ROOT/tools/build_ffmpeg_linux.sh" "$KK_FFMPEG_SRC" "$FFMPEG_DIR"
fi
[ -n "$FFMPEG_DIR" ] || echo "warning: KK_FFMPEG_DIR/KK_FFMPEG_SRC not set: this AppImage cannot play the PS2 cutscenes" >&2

echo "== Build"
cd "$WORK/kk"
# KK_DEV_TOOLS=ON builds the developer test aids in (for testing only, never a release).
cmake --preset kk-linux-release -DKK_DEV_TOOLS="${KK_DEV_TOOLS:-OFF}" -DNR_DXC_DIR="${KK_DXC_DIR:-}" \
  -DKK_FFMPEG_DIR="$FFMPEG_DIR" -DKK_PDFIUM_DIR="${KK_PDFIUM_DIR:-$([ ! -d "$WORK/tools/pdfium/linux-x64" ] || echo "$WORK/tools/pdfium/linux-x64")}"
cmake --build --preset kk-linux-release -- -k 0
BIN="$WORK/kk/out/build/kk-linux-release"

echo "== AppDir"
APPDIR="$WORK/AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib"
cp "$BIN/king_kong" "$APPDIR/usr/bin/"
# The runtime, the GPU plugin and anything else the build put beside the binary.
find "$BIN" -maxdepth 1 -name '*.so*' -exec cp -a {} "$APPDIR/usr/bin/" \;
# The button prompt pictures (kk_button_prompts and the launcher) live beside the program.
cp -a "$BIN/glyphs" "$APPDIR/usr/bin/"
# Task 200: the built-in Kong mods (read from beside the program) and the modding guide.
[ -d "$BIN/mods-builtin" ] && cp -a "$BIN/mods-builtin" "$APPDIR/usr/bin/"
[ -f "$BIN/MODDING.md" ] && cp -a "$BIN/MODDING.md" "$APPDIR/usr/bin/"
# Task 331: the launcher's loading loops (launcher/video, read from beside the program).
[ -d "$BIN/launcher" ] && cp -a "$BIN/launcher" "$APPDIR/usr/bin/"
# The owner's Easter egg game (task 357; Linux: owner, 25 Oct): the official Electron runtime for Linux
# (KK_EGG_ELECTRON_ZIP, the version the Windows build has) with the game's own app.asar (KK_EGG_ASAR, the
# Windows package's) as resources/egg.asar, the same game, started by kk/egg-linux (resources/app), which lets
# the launcher pause it (easter_egg.cpp). launcher/easter-egg/LegallyDistinctGorilla.
if [ -n "${KK_EGG_ELECTRON_ZIP:-}" ] && [ -n "${KK_EGG_ASAR:-}" ]; then
  EGG="$APPDIR/usr/bin/launcher/easter-egg"
  rm -rf "$EGG" && mkdir -p "$EGG"
  unzip -q "$KK_EGG_ELECTRON_ZIP" -d "$EGG"
  mv "$EGG/electron" "$EGG/LegallyDistinctGorilla"
  rm -f "$EGG/chrome-sandbox" "$EGG/resources/default_app.asar"  # no setuid helper in an AppImage (--no-sandbox)
  cp "$KK_EGG_ASAR" "$EGG/resources/egg.asar"
  cp -r "$ROOT/kk/egg-linux" "$EGG/resources/app"
else
  echo "warning: KK_EGG_ELECTRON_ZIP / KK_EGG_ASAR not set: no Easter egg game in this AppImage" >&2
fi
# The SDK's own copies win: the build only refreshes the ones beside the binary when it relinks.
for lib in librexruntime.so libTracyClient.so; do
  [ ! -e "$SDK/lib/$lib" ] || cp -a "$SDK/lib/$lib" "$APPDIR/usr/bin/"
done
# Native rendering only: the Classic (Xenos) GPU plugin never ships, even if an old build left it beside the binary.
rm -f "$APPDIR/usr/bin/"librexgpu-xenos.so*
# The SDK needs a GCC 13 C++ runtime (GLIBCXX_3.4.32); bundle it for older distros.
for lib in libstdc++.so.6 libgcc_s.so.1; do
  cp -L "$(g++ -print-file-name=$lib 2>/dev/null || true)" "$APPDIR/usr/lib/" 2>/dev/null ||
    cp -L "/usr/lib/x86_64-linux-gnu/$lib" "$APPDIR/usr/lib/"
done
# The shipped pipeline list (kkpipelines.list, the same file as the Windows zip's; not in git): the game
# reads it beside the program (inside the AppImage's read-only mount) and makes every pipeline at start,
# instead of compiling them in play on each area's first visit (task 083: up to 3.9 s frames without it).
if [ -n "${KK_PIPELINE_LIST:-}" ]; then
  cp "$KK_PIPELINE_LIST" "$APPDIR/usr/bin/kkpipelines.list"
else
  echo "warning: KK_PIPELINE_LIST not set: no kkpipelines.list, pipelines are compiled in play" >&2
fi
# Recorded chapter resource content keys, supplied outside Git just like the pipeline list.
# The renderer reads the shipped list beside the program, including in a read-only AppImage.
if [ -n "${KK_RESOURCE_LIST:-}" ]; then
  cp "$KK_RESOURCE_LIST" "$APPDIR/usr/bin/kkresources.list"
else
  echo "warning: KK_RESOURCE_LIST not set: no kkresources.list, chapter resources are learned in play" >&2
fi
# Other files the Windows zip ships beside the program and the game looks for there (space-separated paths,
# e.g. benchmark-route.kkbr for About > Run benchmark, FREECAM.txt);
# not in git, like the lists above.
for f in ${KK_EXTRA_FILES:-}; do
  cp -a "$f" "$APPDIR/usr/bin/"
done
cp "$ROOT/docs/images/icon.png" "$APPDIR/king_kong.png"
# The FFmpeg libraries came beside the binary with the *.so* above; their licence and source note go here.
if [ -n "$FFMPEG_DIR" ]; then
  mkdir -p "$APPDIR/usr/share/licenses"
  cp -a "$FFMPEG_DIR/share/licenses/ffmpeg" "$APPDIR/usr/share/licenses/"
else
  rm -f "$APPDIR/usr/bin/"lib*-kk.so*  # left beside the binary by an earlier build with FFmpeg
fi
cp "$ROOT/kk/THIRD-PARTY-NOTICES.txt" "$APPDIR/usr/bin/"
if [ -n "${KK_DXC_DIR:-}" ]; then
  mkdir -p "$APPDIR/usr/share/licenses/dxc"
  find "$KK_DXC_DIR" -maxdepth 1 -iname '*LICENSE*' -exec cp {} "$APPDIR/usr/share/licenses/dxc/" \;
  # The notices for the code inside libdxcompiler.so (SPIRV-Tools, SPIRV-Headers, DirectX-Headers, Apache 2.0):
  # the official repository's licence files at the release tag, in <KK_DXC_DIR>/licences (build_linux_beta.sh).
  [ ! -d "$KK_DXC_DIR/licences" ] || cp "$KK_DXC_DIR/licences/"* "$APPDIR/usr/share/licenses/dxc/"
fi
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
# Run from a copy outside the AppImage's mount. The mount is served by a helper process that ends as soon as it
# gets a signal; Steam signals every process of a game at once when it stops it, so the game, still quitting,
# lost its own code and crashed (SIGBUS) at each exit. The copy is made once per AppImage file (its size and
# time: an update gets a new one) under ~/.cache/king_kong/appimage; older copies are removed.
# KK_APPIMAGE_COPY=0 runs from the mount as before.
if [ -n "$APPIMAGE" ] && [ "${KK_APPIMAGE_COPY:-1}" != 0 ]; then
  ID="$(stat -c '%s-%Y' "$APPIMAGE" 2>/dev/null)"
  BASE="${XDG_CACHE_HOME:-$HOME/.cache}/king_kong/appimage"
  if [ -n "$ID" ] && [ ! -f "$BASE/$ID/.complete" ]; then
    # Other versions' copies go (a running one keeps its files open: Linux frees them when it ends).
    for old in "$BASE"/*; do [ "$old" = "$BASE/$ID" ] || rm -rf "$old"; done
    PART="$BASE/.part-$$"
    rm -rf "$BASE/$ID"
    if mkdir -p "$PART" && cp -a "$HERE/." "$PART/" && touch "$PART/.complete"; then
      mv -T "$PART" "$BASE/$ID" 2>/dev/null || rm -rf "$PART"
    else
      rm -rf "$PART"
    fi
  fi
  if [ -n "$ID" ] && [ -f "$BASE/$ID/.complete" ]; then
    HERE="$BASE/$ID"
    export KK_APPIMAGE_COPY_DIR="$HERE"
  fi
fi
LIBS="$HERE/usr/bin"
# Use the bundled C++ runtime only when the system's is older than the one
# the game needs; a newer system copy is kept for the graphics drivers.
SYS_CXX="$(ldconfig -p 2>/dev/null | awk '/libstdc\+\+\.so\.6 .*x86-64/ {print $NF; exit}')"
if [ -z "$SYS_CXX" ] || ! grep -q GLIBCXX_3.4.32 "$SYS_CXX" 2>/dev/null; then LIBS="$LIBS:$HERE/usr/lib"; fi
export LD_LIBRARY_PATH="$LIBS${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$HERE/usr/bin/king_kong" "$@"
EOF
chmod +x "$APPDIR/AppRun"

echo "== Game data check"
# Nothing derived from the game may ship: the native renderer's shader packs are built on the
# player's machine (native-renderer/docs/shaders.md, "The pack on the player's machine").
if find "$APPDIR" \( -iname '*.pack' -o -iname '*.pack.stamp' -o -iname '*.kksh' -o -iname 'xeshaders.bin'     -o -iname 'image.bin' -o -iname '*.xex' -o -iname '*.bf' -o -iname '*.xsh' \) | grep .; then
  echo "error: files derived from the game are in the AppDir (above)" >&2
  exit 1
fi

echo "== Missing libraries check"
for lib in king_kong librexgpu-native.so libdxcompiler.so; do
  [ -f "$APPDIR/usr/bin/$lib" ] || { echo "error: missing $lib" >&2; exit 1; }
  DEPS="$(LD_LIBRARY_PATH="$APPDIR/usr/bin:$APPDIR/usr/lib" ldd "$APPDIR/usr/bin/$lib")"
  if grep 'not found' <<< "$DEPS"; then exit 1; fi
done
if [ -n "$FFMPEG_DIR" ]; then
  # The cutscene decoder must use the bundled LGPL build, never a system FFmpeg.
  DEPS="$(LD_LIBRARY_PATH="$APPDIR/usr/bin:$APPDIR/usr/lib" ldd "$APPDIR/usr/bin/king_kong")"
  for lib in libavformat-kk libavcodec-kk libavutil-kk libswscale-kk libswresample-kk; do
    grep -q "$lib.so.[0-9]* => $APPDIR/usr/bin/" <<< "$DEPS" || { echo "error: $lib is not the bundled one" >&2; exit 1; }
  done
fi

echo "== AppImage"
TOOL="${KK_APPIMAGETOOL:-$WORK/appimagetool-x86_64.AppImage}"  # KK_APPIMAGETOOL: a copy already on disk
[ -x "$TOOL" ] || { wget -q -O "$TOOL" \
  https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage && chmod +x "$TOOL"; }
mkdir -p "$ROOT/dist"
OUT="$ROOT/dist/KingKong-$VERSION-linux-x86_64.AppImage"
# KK_APPIMAGE_RUNTIME: the AppImage type 2 runtime already on disk (otherwise appimagetool downloads it).
RUNTIME_ARGS=()
[ -z "${KK_APPIMAGE_RUNTIME:-}" ] || RUNTIME_ARGS=(--runtime-file "$KK_APPIMAGE_RUNTIME")
ARCH=x86_64 "$TOOL" --appimage-extract-and-run "${RUNTIME_ARGS[@]}" "$APPDIR" "$OUT"
ls -la "$OUT"
