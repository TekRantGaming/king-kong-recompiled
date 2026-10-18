# Linux: building and releasing the AppImage

From 17 Oct 2026 every beta ships for Windows and Linux (DECISIONS.md). The Linux build is one AppImage:
the game, its launcher and the native renderer on **Vulkan only** (no DirectX 12, no Classic renderer). This
page is the recipe (task 191). The Windows side is unchanged (`Build-KingKong.ps1`, `kk\build.bat kk-beta`).

## One command

Inside WSL on the build PC (always the `Ubuntu-24.04` distro, never the default `Ubuntu`) or on a Linux PC,
from the repository (worktree) root:

```
wsl -d Ubuntu-24.04
cd /mnt/f/KK-native-renderer/wt/<worktree>
KK_PACKAGE_DIR=/mnt/f/KK-native-renderer/testbuilds/<windows package>/KingKong \
KK_APPIMAGETOOL=/root/kk-beta4-081/work/appimagetool-x86_64.AppImage \
  bash tools/build_linux_beta.sh v2.0.0-beta.6
```

Output: `dist/KingKong-<version>-linux-x86_64.AppImage` and `dist/SHA256SUMS-linux.txt` (git-ignored). The work
folder is `~/kk-linux-beta` (`KK_LINUX_ROOT`): the SDK source and build, `sdk/` (the installed Linux SDK), `dxc/`,
`ffmpeg/`, `work/` (the game build and the AppDir), logs (`sdk-build.log`, `ffmpeg-build.log`,
`appimage-build.log`). Every step is skipped or incremental when its result is up to date: a first build takes
about 6 minutes on the 16-thread build PC (SDK 3, game 2, FFmpeg 0.5, squashfs 0.5), a rebuild after a source
change 1 to 3 minutes. `KK_DEV_TOOLS=ON` makes a developer AppImage (test aids, `-devtools` in the name if you
pass it in the version) in its own `work-dev/` folder; never release one.

The script, in order:

1. **Toolchain check.** Lists what is missing and the Ubuntu packages to install; it installs nothing.
2. **ReXGlue SDK source**: `REXGLUE_SDK_SRC` (default `F:\KK-native-renderer\sdk-src-upscalers-038`: SDK v0.10.0
   + `tools/rexglue-patches` + `native-renderer/sdk-patches` 0001-0003, 0035, 0037, 0038) + sdk-patches 0039,
   0040, 0041, 0043, then compared file by file with the Windows SDK's source (`REXGLUE_SDK_CHECK`, default
   `sdk-src-upscalers-043`): the build stops if any file differs.
3. **SDK build** (clang 20, Release, `REXGLUE_USE_VULKAN=ON`, preset `linux-amd64`) installed to `sdk/`.
4. **DXC**: the official Microsoft Linux release, the same version as Windows' `dxcompiler.dll`
   (`KK_DXC_FROM`, default `downloads\dxc-linux-v1.9.2609\x`: v1.9.2609, `libdxcompiler.so` 1.9.2609.5).
5. **FFmpeg** for the PS2 cutscenes: `tools/build_ffmpeg_linux.sh` from the SDK's own `thirdparty/FFmpeg`
   (4.4, LGPL-only, `lib*-kk.so.N`; see "Licences").
6. **`tools/build_appimage.sh`**: game build (`kk-linux-release` preset, `KK_DEV_TOOLS=OFF`), the AppDir, the
   game-data check (no shader packs, XEX, BF, image.bin), the missing-library check (`ldd` of king_kong, the
   renderer and DXC; the FFmpeg libraries must resolve to the bundled ones), then appimagetool.
7. **Package checks**: required files, the four shipped data files, no frame-generation libraries
   (`*dlssg*`, `*_fg*`), no Windows files, no `librexgpu-xenos`; size and SHA-256.

### Files that are not in git

The AppImage ships the same game-derived data files as the Windows zip, read beside the program (inside the
read-only AppImage mount): `kkpipelines.list`, `kkresources.list`, `benchmark-route.kkbr` and `FREECAM.txt`.
`KK_PACKAGE_DIR` takes all four from the staged Windows package folder; `KK_PIPELINE_LIST`,
`KK_RESOURCE_LIST` and `KK_EXTRA_FILES` (space-separated paths) override them one by one. The Beta 6 package
must ship the task 171 list (`testbuilds\beta6-work\pipelines\kkpipelines-beta6-171.list`, 7,594 records:
Beta 5's merged Windows + Ally list plus the route recordings) on both systems.

### No downloads during the build

`tools/build_appimage.sh` downloads appimagetool and appimagetool downloads the AppImage runtime unless they
are already on disk: `KK_APPIMAGETOOL` (the copy in `/root/kk-beta4-081/work/`) and `KK_APPIMAGE_RUNTIME`
(by default the runtime is cut from the front of that appimagetool and kept as `~/kk-linux-beta/runtime-x86_64`).
`REXGLUE_SDK_DIR` (set by the script) stops it downloading the official SDK.

## Toolchain (Ubuntu 24.04, official repositories only)

```
sudo apt install clang-20 lld-20 g++ cmake ninja-build nasm make rsync patch file pkg-config \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxinerama-dev libxss-dev libxkbcommon-dev \
  libwayland-dev libdecor-0-dev libegl-dev libgl-dev libvulkan-dev libasound2-dev libpulse-dev \
  libpipewire-0.3-dev libdbus-1-dev libudev-dev libgtk-3-dev
```

For the headless test also `xvfb mesa-vulkan-drivers pulseaudio` (all already in the build PC's Ubuntu-24.04;
task 191 installed nothing). The AppImage needs at run time: glibc 2.38 or newer (it is built on Ubuntu 24.04:
Ubuntu 24.04, Fedora 39+, Bazzite run it; Ubuntu 22.04 with 2.35 cannot; the C++ runtime, GLIBCXX_3.4.32, is
bundled and AppRun uses it only when the system's is older), a Vulkan 1.3 driver
(Mesa RADV/ANV, NVIDIA), `curl` for the content downloads and the Workshop page, `libdbus-1.so.3` for
notifications (optional; falls back to `notify-send`, then a pop-up), FUSE 2 to mount the AppImage (or
`--appimage-extract-and-run`).

## SDK patch levels: Windows and Linux

| Patch | Windows SDK -043 | Linux SDK (task 191) | Notes |
|---|---|---|---|
| rexglue-patches 0001-0017, sdk-patches 0001-0003 | yes | yes | in the -038 source's history |
| 0035 present semaphores per image | yes | yes | |
| 0036 AMD FSR luma history | yes (FidelityFX DLLs) | no | Windows DLLs only; no FSR 3.1 on Linux |
| 0037 UI paint on demand | yes | yes | |
| 0038 Vulkan presenter keeps the swapchain pipeline format | yes | yes | the Ally stutter fix (task 083) |
| 0039 POSIX WaitMultiple sleeps the real time | in the source, no effect | yes | Linux only (audio worker spin, task 085) |
| 0040 timer queue sleeps instead of spinning | in the source, no effect | yes | Linux only (`#if` guarded) |
| 0041 pacing / benchmark present observer | yes | yes (unused) | the game calls it on Windows only (benchmark host presents, handoff) |
| 0043 scRGB HDR output | yes | yes (inactive) | HDR is offered only where Windows reports an HDR monitor |

Before task 191 the Linux SDK (`/root/kk-beta4-081/sdk`, built 15 Oct) had everything up to 0040 but not
0041/0043. No game feature depended on them on Linux, but the HDR settings then had no SDK option behind
them; the new SDK makes the two sources identical (step 2 checks it).

## Upscalers on Linux

| Upscaler | Linux | Why |
|---|---|---|
| FSR 1, NVIDIA Image Scaling (spatial, SDK patch 0008) | yes | built into the SDK |
| DLSS (Super Resolution, DLAA) | not yet | the DLSS SDK 310.9.1 ships Linux x86_64 files (`lib/Linux_x86_64/libnvsdk_ngx.a`, `rel/libnvidia-ngx-dlss.so.310.9.1`), but `upscaler_dlss.cpp` is Windows-only (windows.h, file version, D3D12 parts), `cmake/upscalers.cmake` adds it on WIN32 only, and the launcher's "which upscalers run here" query is a D3D12 probe. A port is a separate job and needs an NVIDIA GPU with the proprietary Linux driver to verify (WSL has none). Never ship `libnvidia-ngx-dlssg` (frame generation). |
| XeSS | no | the XeSS SDK 3.0.2 has Windows DLLs only |
| FSR 3.1 / FSR 4 | no | AMD ships signed Windows DLLs only (FidelityFX SDK 1.1.4, FSR SDK 2.3.0) |

The launcher and F8 offer only what is built in; on Linux the upscaler choice is FSR 1/NIS (render scale +
spatial upscale) or off.

The temporal upscaler question (`nr_upscaler_query`) is a Direct3D 12 probe, so on Linux it answers
"none here" at once; the launcher takes that as the final answer (`OfferedTemporalUpscalers`, Linux only,
task 191), and the setup's Upscaling page and Display > Upscaling show DLSS/FSR/XeSS as not available instead
of "Checking" for ever.

## Linux behaviour worth knowing

- **Settings, logs and game files live beside the `.AppImage` file** (`king_kong.toml`, `logs/`, `game/`;
  `OnConfigurePaths`), because the program itself runs from a read-only mount. A `--config=` path is not
  used. Tests must therefore run their own copy of the AppImage in their run folder (a run from `dist/`
  leaves `dist/king_kong.toml`, and the next run there is no longer a first start).
- **Launcher backdrops from the game's trailer** (`Trailer.wmv`) are cut with the system's `ffmpeg` program
  (`launcher_art.cpp`). Without it (e.g. WSL; check SteamOS/Bazzite) the log says "no stills from the videos
  (is ffmpeg installed?)" and the setup and Home pages use the plain dark background. The bundled FFmpeg has
  no WMV/ASF decoder (only H.264/AAC for the PS2 cutscenes); adding the LGPL `asf` demuxer and `wmv3`/`vc1`
  decoders and decoding in-process would remove the dependency (not done).
- **The PS2 cutscene setup page and Settings > Advanced > Cutscenes rows** depend on
  `kk::movies::PlaybackAvailable()`; branches still using the old `kPs2MoviePlayback` constant hide them on
  Linux (the launcher job switches it). The content service itself downloads and installs the cutscenes on
  Linux (task 191 tour).
- **Graphics pictures** on the setup pages are made at run time from the player's own game files (chapter
  pictures on Linux, as the movie stills need Media Foundation); nothing ships for them (task 219).

## What the AppImage contains (Beta 6 test, task 191)

`usr/bin/`: `king_kong`, `librexruntime.so`, `librexgpu-native.so`, `libTracyClient.so`, `libdxcompiler.so`,
`libavcodec-kk.so.58`, `libavformat-kk.so.58`, `libavutil-kk.so.56`, `libswresample-kk.so.3`,
`libswscale-kk.so.5`, `glyphs/`, `kkpipelines.list`, `kkresources.list`, `benchmark-route.kkbr`, `FREECAM.txt`,
`THIRD-PARTY-NOTICES.txt`; `usr/lib/`: `libstdc++.so.6`, `libgcc_s.so.1` (used only on older systems);
`usr/share/licenses/ffmpeg` and `usr/share/licenses/dxc`; `AppRun`, `king_kong.desktop`, `king_kong.png`.
Size 48,634,360 bytes (46.4 MiB; AppDir 144 MB unpacked). Two builds of the same source differ in their SHA-256 (squashfs
times); compare contents, not hashes.

## Licences

- **FFmpeg** (LGPL 2.1 or later): separate replaceable shared libraries beside the program, the licence text,
  the exact source revision and configure line in `usr/share/licenses/ffmpeg`, and a THIRD-PARTY-NOTICES
  section. With a public release the FFmpeg source must stay available (the SDK's `thirdparty/FFmpeg` at the
  recorded revision; offer it, or link the upstream release tag, in the release notes).
- **DXC**: `LICENSE-MS.txt`, `LICENSE-LLVM.txt` and the release's notices (`DXC-ThirdPartyNotices.txt`,
  SPIRV-Tools, SPIRV-Headers, DirectX-Headers, Apache 2.0) in `usr/share/licenses/dxc`.
- **stb_image 2.29** (Workshop pictures, Linux only), **NVIDIA Image Scaling**, **FSR 1/CAS**, **NVRHI**,
  Vulkan-Headers, SDL and the SDK's other libraries: in `THIRD-PARTY-NOTICES.txt` (the same file as Windows).
- No upscaler SDK runtime ships on Linux, so the DLSS/XeSS/FidelityFX licence terms for redistributed
  binaries do not apply to the AppImage (their notices stay in the shared file).

## Testing without a Linux PC (WSL, headless)

`tools/linux_headless_run.sh` runs an AppImage with nothing on the desktop: a private Xvfb (`:191`,
1280x720, `-fbdir`; `tools/xwd2png.py` turns the framebuffer into PNG captures), lavapipe (`VK_DRIVER_FILES` =
Mesa's `lvp_icd.json`), a private PulseAudio with only a null sink (the SDK's SDL has no dummy audio driver),
`APPIMAGE_EXTRACT_AND_RUN=1` (no FUSE in WSL), its own copy of the AppImage, private user data and cache, game
data read-only from `/mnt/f`. It stops only its own processes (matched by the run folder in their command
line). The script's header lists the options. The release smoke test:

```
# 1. first start with the RELEASE AppImage up to the texture pack page (shader setup timing)
FRESH=1 STOP_ON="KK: setup: texture pack" tools/linux_headless_run.sh first dist/KingKong-<v>-linux-x86_64.AppImage 900
# 2. the whole setup + Workshop + both downloads with the DEVELOPER AppImage (scripted input, local ZIP server)
FRESH=1 SERVE=/mnt/f/KK-tex-pack-test/downloads STEPS=<steps file> STOP_ON="KK dev: shot home-done" \
  tools/linux_headless_run.sh tour dist/KingKong-<v>-devtools-linux-x86_64.AppImage 1800
# 3. one chapter for about 2 minutes of play with the RELEASE AppImage (cache of run 1: shaders ready)
CACHE_FROM=<run 1 folder> tools/linux_headless_run.sh chapter dist/KingKong-<v>-linux-x86_64.AppImage 420 \
  --kk_launcher=false --kk_start=chapter:0:2
```

The steps file of the task 191 tour (one command per line): `wait-setup:6, shot:s6-texture, pad:a, wait-modal,
pad:right, pad:a, pad:a, wait-setup:8, focus:Next, pad:a, wait-setup:9, focus:Next, pad:a, wait-setup:10,
focus:Next, pad:a, wait-setup:11, focus:Next, pad:a, wait-home, content:download:cut, pad:down, pad:down,
pad:a, pause:14, workshop:state, workshop:open:0, pause:8, kb:b, pause:2, kb:b, wait-content, content:status,
shot:home-done` (add `shot:<name>` and `rows` where wanted; later setup pages need their own steps).

lavapipe cannot show: performance and frame pacing (CPU rasteriser, seconds per frame), GPU driver bugs and
pipeline-cache behaviour of RADV/NVIDIA, the graphics pipeline library's fast linking on a real driver,
VRAM limits, Wayland, gamescope / Steam Game Mode, controllers, real audio devices, desktop notifications,
FUSE mounting. Those need the owner's ROG Ally (Bazzite) or another Linux PC.

## Release checklist (Windows zip + Linux AppImage)

1. Same source commit for both; the version in `kk/CMakeLists.txt` (`KK_VERSION`) bumped, CHANGELOG entry final.
2. Windows: the usual package (DECISIONS.md "Before handing over any test build": NVSDK_NGX + xess in
   `rexgpu-native.dll`, every upscaler DLL, Windows tar, PK header).
3. Linux: `bash tools/build_linux_beta.sh v<version>` with `KK_PACKAGE_DIR` = the staged Windows `KingKong`
   folder (same lists, route and FREECAM.txt) and `KK_PIPELINE_LIST` if the package's list is older. The script
   must end with "done", all four data files "has", "same as the Windows SDK source", no "developer test aids".
4. Smoke test in WSL (launcher first start up to Home, one chapter for 2 minutes) with the release AppImage.
5. Owner test on the ROG Ally (Desktop Mode and Steam Game Mode) before any public release.
6. Release assets: `KingKong-v<version>-windows-x64.zip`, `KingKong-v<version>-linux-x86_64.AppImage`, one
   `SHA256SUMS.txt` with both; release notes say: Linux = Vulkan only, FSR 1/NIS only (no DLSS/XeSS/FSR 3.1 yet),
   needs curl; FFmpeg source offer (LGPL).
7. Delete test shader caches as for Windows; nothing game-derived in git.
