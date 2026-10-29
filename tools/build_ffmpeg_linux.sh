#!/usr/bin/env bash
# Builds the small FFmpeg libraries the Linux game uses to play the restored PS2 cutscenes
# (kk/src/ps2_movie_decoder_ffmpeg.cpp). Run inside Linux or WSL.
#
#   tools/build_ffmpeg_linux.sh <ffmpeg-source> <install-folder>
#
# <ffmpeg-source>: an FFmpeg 4.4 source tree. The ReXGlue SDK's own copy works (its thirdparty/FFmpeg, the
# FFmpeg the runtime already uses for game audio), and so does an official release from ffmpeg.org
# (ffmpeg-4.4.x.tar.xz). The tree is exported to a private copy; it is never changed.
# <install-folder>: where include/, lib/ and share/licenses/ffmpeg go; pass it to CMake as KK_FFMPEG_DIR
# (tools/build_appimage.sh does this when KK_FFMPEG_DIR is set).
#
# What is built, and the licence: only the MP4 demuxer, the H.264 and AAC decoders and parsers, the file
# protocol, libswscale and libswresample, as shared libraries named lib*-kk.so.N so they can never be mixed
# up with a system FFmpeg. No GPL or non-free parts are enabled, so the libraries are LGPL 2.1 or later. They
# are linked dynamically and shipped as separate files beside the program (a player may replace them), with
# the licence text and the exact source and configure line in share/licenses/ffmpeg.
# Needs: a C compiler (clang-20 or cc), make, nasm (Ubuntu: apt install nasm) for the fast x86 decoders.
set -euo pipefail

SRC="${1:?usage: build_ffmpeg_linux.sh <ffmpeg-source> <install-folder>}"
PREFIX="${2:?usage: build_ffmpeg_linux.sh <ffmpeg-source> <install-folder>}"
SRC="$(cd "$SRC" && pwd)"
mkdir -p "$PREFIX"
PREFIX="$(cd "$PREFIX" && pwd)"
WORK="${KK_FFMPEG_WORK:-$PREFIX.work}"
CC="${CC:-$(command -v clang-20 || command -v cc)}"
command -v nasm >/dev/null || { echo "error: nasm is needed (Ubuntu: sudo apt install nasm)" >&2; exit 1; }

echo "== FFmpeg source -> $WORK/src"
rm -rf "$WORK/src" "$WORK/build"
mkdir -p "$WORK/src" "$WORK/build"
REVISION="(not a git checkout)"
if git -C "$SRC" rev-parse HEAD >/dev/null 2>&1; then
  # The committed files with Unix line endings (a Windows checkout has CRLF, which configure cannot run).
  REVISION="$(git -C "$SRC" rev-parse HEAD) ($(git -C "$SRC" remote get-url origin 2>/dev/null || echo 'no remote'))"
  git -C "$SRC" -c core.autocrlf=false -c core.eol=lf archive HEAD | tar -x -C "$WORK/src"
else
  cp -a "$SRC/." "$WORK/src/"
  # A copy of a Windows checkout (tools/make_linux_sdk_base.sh from C:\rexsrc): its text files to Unix line endings.
  grep -rlI $'\r' "$WORK/src" | xargs -r sed -i 's/\r$//'
fi
# The SDK's copy carries files generated for its own premake build; configure makes its own, and an
# out-of-tree build refuses to run (or would include the wrong lists) while they are in the source tree.
mkdir -p "$WORK/premake-generated"
for f in config.h libavcodec/bsf_list.c libavcodec/codec_list.c libavcodec/parser_list.c \
         libavformat/demuxer_list.c libavformat/muxer_list.c libavformat/protocol_list.c \
         libavutil/avconfig.h libavutil/ffversion.h; do
  if [ -f "$WORK/src/$f" ]; then
    mkdir -p "$WORK/premake-generated/$(dirname "$f")"
    mv "$WORK/src/$f" "$WORK/premake-generated/$f"
  fi
done

OPTIONS=(--prefix="$PREFIX" --build-suffix=-kk --cc="$CC"
  --disable-everything --disable-autodetect --disable-programs --disable-doc
  --disable-avdevice --disable-avfilter --disable-postproc --disable-network --disable-debug
  --enable-shared --disable-static --enable-pic
  --enable-protocol=file --enable-demuxer=mov --enable-decoder=h264,aac --enable-parser=h264,aac
  # The game's own logo movies at the launcher's start (Video/Ubisoft, Universal, Wingnut.wmv: ASF, WMV3 with
  # WMA 2 or WMA Pro): on Windows Media Foundation plays them; here these LGPL decoders.
  --enable-demuxer=asf --enable-decoder=wmv3,wmav2,wmapro
  --enable-swscale --enable-swresample)
# FFmpeg 4.4 still has libavresample (off by default in later versions).
grep -q -- '--disable-avresample' "$WORK/src/configure" && OPTIONS+=(--disable-avresample)

echo "== configure"
cd "$WORK/build"
"$WORK/src/configure" "${OPTIONS[@]}" > "$WORK/configure.log" 2>&1 || { tail -20 "$WORK/configure.log"; exit 1; }
grep -q '^License: LGPL version 2.1 or later' "$WORK/configure.log" ||
  { echo "error: the configured FFmpeg is not LGPL" >&2; grep '^License' "$WORK/configure.log" >&2; exit 1; }
echo "== build"
make -j"$(nproc)" > "$WORK/make.log" 2>&1 || { grep -E 'error|Error' "$WORK/make.log" | head -20; exit 1; }
make install > "$WORK/install.log" 2>&1
rm -rf "$PREFIX/share/ffmpeg"  # examples only
strip --strip-unneeded "$PREFIX"/lib/lib*-kk.so.*.*.*

echo "== licence"
L="$PREFIX/share/licenses/ffmpeg"
mkdir -p "$L"
cp "$WORK/src/COPYING.LGPLv2.1" "$L/"
cp "$WORK/src/LICENSE.md" "$L/" 2>/dev/null || true
cat > "$L/SOURCE.txt" <<EOF
FFmpeg $(cat "$WORK/src/RELEASE" 2>/dev/null || echo '?') (LGPL 2.1 or later), used by King Kong Recompiled on Linux
to play the restored PS2 cutscenes (MP4: H.264 video, AAC audio).

Source: $REVISION
(the FFmpeg copy in the ReXGlue SDK's thirdparty/FFmpeg; the same build works from an official FFmpeg 4.4
release, https://ffmpeg.org/releases/). Built with tools/build_ffmpeg_linux.sh from the King Kong
Recompiled source:
configure ${OPTIONS[*]/--prefix=*/--prefix=<install folder>}

The libraries (lib*-kk.so.N) are dynamically linked and may be replaced with your own build of the same
FFmpeg version and options.
EOF
ls -la "$PREFIX"/lib/lib*-kk.so.*.*.*
du -ch "$PREFIX"/lib/lib*-kk.so.*.*.* | tail -1
