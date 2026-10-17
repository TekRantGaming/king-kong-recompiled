#!/usr/bin/env bash
# One headless run of a King Kong AppImage on Linux or in WSL (wsl -d Ubuntu-24.04), with nothing on the
# desktop: a private X server (Xvfb :191, 1280x720; its framebuffer file gives captures), lavapipe (Mesa's CPU
# Vulkan driver), a private PulseAudio with only a null sink (the SDK's SDL has no dummy audio driver), and
# private user data, cache and settings in the run folder (the AppImage keeps king_kong.toml beside itself, so
# each run gets its own copy of it). Game data is read-only. native-renderer/docs/linux.md, "Testing".
#
#   tools/linux_headless_run.sh <name> <appimage> <seconds> [game args...]
#
# Env: KK_GAME_DATA (game files; default the beta4-next worktree's kk/assets on F:), KK_RUNS (run folders;
# default ~/kk-linux-beta/runs), FRESH=1 (empty user data = first start; else a copy of KK_USERDATA_FROM,
# default F:/KK-native-renderer/userdata), CACHE_FROM=<run folder> (its cache: shaders ready; a run with a
# cache is not a first start), STEPS=<file> (developer AppImage: scripted launcher input,
# KK_DEV_LAUNCHER_INPUT_FILE; its "shot:" lines become captures), SERVE=<folder> (serve the content ZIPs
# kk-4x-usv2.zip and KK_Cuts_AI.zip from it on 127.0.0.1:18791 and point a developer AppImage at them),
# STOP_ON=<regex> + STOP_DELAY=<s> (stop that long after the log matches), SHOTS="<s> ..." (captures).
# Stops only its own processes (matched by the run folder). Output: <run>/run.log, stdout.txt, shots/*.png.
set -u
NAME=$1 APP=$2 LIMIT=$3; shift 3
TOOLS="$(cd "$(dirname "$0")" && pwd)"
RUNS="${KK_RUNS:-$HOME/kk-linux-beta/runs}"
GAME="${KK_GAME_DATA:-/mnt/f/KK-native-renderer/wt/beta4-next/kk/assets}"
RUN=$RUNS/$NAME-$(date +%H%M%S)
mkdir -p $RUN/shots $RUN/fb
echo $RUN > $RUNS/last.txt
if [ "${FRESH:-0}" = 1 ]; then mkdir -p $RUN/userdata
else rsync -a "${KK_USERDATA_FROM:-/mnt/f/KK-native-renderer/userdata}/" $RUN/userdata/; fi
[ -z "${CACHE_FROM:-}" ] || cp -a "$CACHE_FROM/cache" $RUN/cache
export XDG_RUNTIME_DIR=$RUN/xdg PULSE_SERVER=unix:$RUN/pulse.sock
mkdir -p -m 700 $XDG_RUNTIME_DIR
pulseaudio -n --daemonize=no --exit-idle-time=-1 --use-pid-file=no --log-target=file:$RUN/pulse.log \
  -L "module-null-sink sink_name=kknull" -L "module-native-protocol-unix socket=$RUN/pulse.sock auth-anonymous=1" &
PA=$!
Xvfb :191 -screen 0 1280x720x24 -fbdir $RUN/fb -nolisten tcp > $RUN/xvfb.log 2>&1 &
XV=$!
SRV=
if [ -n "${SERVE:-}" ]; then
  python3 -I $TOOLS/range_server.py "$SERVE" 18791 > $RUN/server.log 2>&1 &
  SRV=$!
  export KK_DEV_CONTENT_URL_TEX=http://127.0.0.1:18791/kk-4x-usv2.zip KK_DEV_CONTENT_URL_CUT=http://127.0.0.1:18791/KK_Cuts_AI.zip
fi
sleep 2
unset WAYLAND_DISPLAY
export DISPLAY=:191 SDL_VIDEO_DRIVER=x11 SDL_AUDIO_DRIVER=pulseaudio APPIMAGE_EXTRACT_AND_RUN=1 \
  VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0 SDL_WINDOW_ACTIVATE_WHEN_RAISED=0 KK_DEV_AUTOSKIP=1 KK_DEV_NO_PADS=1
if [ -n "${STEPS:-}" ]; then cp "$STEPS" $RUN/steps.txt; export KK_DEV_LAUNCHER_INPUT_FILE=$RUN/steps.txt; fi
cp "$APP" $RUN/KingKong.AppImage
cd $RUN
START=$(date +%s)
$RUN/KingKong.AppImage --fullscreen=false --window_width=1280 --window_height=720 --kk_fullscreen_mode=desktop \
  --log_flush_interval=1 --log_max_file_size_mb=4000 --log_max_files=2 \
  --user_data_root=$RUN/userdata --game_data_root="$GAME" --cache_root=$RUN/cache --log_file=$RUN/run.log \
  --kk_check_updates=false --kk_discord_status=false --kk_online_asked=true --kk_online_count=false \
  "$@" > $RUN/stdout.txt 2>&1 &
echo "run $RUN"
grab() { python3 -I $TOOLS/xwd2png.py $RUN/fb/Xvfb_screen0 $RUN/shots/$1.png 0.5 2>/dev/null; }
( tail -F -n +1 $RUN/run.log 2>/dev/null | grep --line-buffered -a -o "KK dev: shot [A-Za-z0-9_.-]*" |
  while read -r _ _ _ s; do sleep 1.5; grab $s; done ) &
SH=$!
for s in ${SHOTS:-}; do ( sleep $s; grab t$s ) & done
end=$(( $(date +%s) + LIMIT ))
while [ $(date +%s) -lt $end ]; do
  pgrep -f "$RUN/userdata" >/dev/null || break
  if [ -n "${STOP_ON:-}" ] && grep -a -q -E "$STOP_ON" $RUN/run.log 2>/dev/null; then
    sleep ${STOP_DELAY:-6}; grab stop; echo "stop condition met"; break
  fi
  sleep 2
done
ELAPSED=$(( $(date +%s) - START ))
LEFT=$(pgrep -f "$RUN/userdata" | tr '\n' ' ')
[ -z "$LEFT" ] || { kill $LEFT 2>/dev/null; sleep 4; kill -9 $(pgrep -f "$RUN/userdata") 2>/dev/null; }
grab zz-last
pkill -P $SH 2>/dev/null; kill $SH $PA $XV $SRV 2>/dev/null
wait 2>/dev/null
echo "elapsed $ELAPSED s; leftover: $(pgrep -af "$RUN" | grep -v pgrep || echo none)"
