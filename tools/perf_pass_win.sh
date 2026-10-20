#!/bin/bash
# A perf pass on this Windows machine: the game loads a save and stands in the
# world, headless at the 60 fps cap, while tools/win/perfmon.ps1 samples the
# process's GPU engines and memory the way Task Manager counts them; then a
# summary of the steady seconds (from 20 s after the world's first frame):
# fps, GPU 3D/compute %, dedicated/shared MiB, CPU, and bbhost's own 300-flip
# memory and GPU lines.
#
#   tools/perf_pass_win.sh NAME [VAR=value ...]
#
# Environment: BBHOST_EXE (default build-win/bbhost.exe); PERF_TOML, a config
# with [paths] app0 and eboot (default the per-user %APPDATA%/bbhost/bbhost.toml);
# PERF_SAVES, the saves folder copied in before each run (default the per-user
# config's data/saves); PERF_SECONDS (150), PERF_HEADLESS (1); PERF_UNCAPPED
# (1, the default: BBHOST_UNCAP=1 BBHOST_BENCH_UNCAPPED=1 - no frame target and
# flips complete at once, the machine's true frame rate standing still; 0: the
# 60 cap).
# Runs play on build/perf/data (a copy of the saves; the pipeline cache stays
# between runs so compiles do not land in the measured seconds) and write
# build/perf/NAME.log, NAME.perfmon.
set -u
cd "$(dirname "$0")/.."
NAME=${1:?NAME}
shift
root=$(pwd -W 2>/dev/null || pwd)
exe=${BBHOST_EXE:-$root/build-win/bbhost.exe}
appdata_cfg="${APPDATA:-$HOME/AppData/Roaming}/bbhost/bbhost.toml"
toml=${PERF_TOML:-$appdata_cfg}
[ -f "$toml" ] || { echo "no config with the game's paths: $toml (PERF_TOML)"; exit 1; }
field() { sed -n "s/^$1 *= *\"\(.*\)\"/\1/p" "$toml" | head -1; }
app0=$(field app0); eboot=$(field eboot); data=$(field data)
saves=${PERF_SAVES:-$data/saves}
[ -d "$saves" ] || { echo "no saves at $saves (PERF_SAVES)"; exit 1; }
secs=${PERF_SECONDS:-150}
out=build/perf
mkdir -p "$out/data" "$out/cfg"
rm -rf "$out/data/saves"
cp -r "$saves" "$out/data/saves"
wm=""; [ -n "${PERF_FULLSCREEN:-}" ] && wm='window_mode = "Fullscreen"'
cat > "$out/cfg/bbhost.toml" <<TOML
[paths]
app0 = "$app0"
eboot = "$eboot"
data = "$root/$out/data"
[online]
require_account = true
[player]
ime = "auto"
[video]
width = 1920
height = 1080
fps_cap = 60
$wm
[startup]
skip_intro = true
setup_window = false
[update]
check = false
[bbhost]
config_version = 3
TOML
rm -f "$out/data/bbhost/gpu-device-lost.txt" "$out/$NAME.perfmon"
# Title: Cross through the "press any button", Continue and any notice.
taps="20:cross,25:cross,30:cross,35:cross,40:cross,45:cross,50:cross"
powershell -NoProfile -ExecutionPolicy Bypass -File tools/win/perfmon.ps1 -Name bbhost -Seconds "$secs" -Out "$root/$out/$NAME.perfmon" > /dev/null 2>&1 &
mon=$!
[ -n "${PERF_FULLSCREEN:-}" ] && { PERF_HEADLESS=0; powershell -NoProfile -ExecutionPolicy Bypass -File tools/win/keep_focus.ps1 -Name bbhost -Seconds "$((secs + 60))" > /dev/null 2>&1 & }
uncap=()
[ "${PERF_UNCAPPED:-1}" = 1 ] && uncap=(BBHOST_UNCAP=1 BBHOST_BENCH_UNCAPPED=1)
env BBHOST_CONFIG_DIR="$root/$out/cfg" BBHOST_SETUP_WINDOW=0 BBHOST_NO_GAMEPAD=1 BBHOST_SKIP_INTRO=1 \
    BBHOST_HEADLESS=${PERF_HEADLESS:-1} BBHOST_FRAME_STATS=1 BBHOST_GAME_FPS=60 \
    BBHOST_EXIT_SECONDS=$secs BBHOST_AUTOPRESS="$taps" "${uncap[@]}" "$@" \
    timeout $((secs + 120)) "$exe" --config "$root/$out/cfg/bbhost.toml" > "$out/$NAME.log" 2>&1
echo "exit $?" >> "$out/$NAME.log"
wait $mon 2>/dev/null
python tools/perf_pass_summary.py "$out/$NAME.log" "$out/$NAME.perfmon"
