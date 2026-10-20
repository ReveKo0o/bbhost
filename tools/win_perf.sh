#!/bin/bash
# A frame-rate soak on a Windows machine over SSH:
# upload a package, run the frozen seed headless with frame statistics, fetch
# the log and summarise it the way tools/perf_soak.sh does on Linux.
#
#   SSHPASS=... tools/win_perf.sh NAME [ZIP] [VAR=value ...]
#
# ZIP (e.g. build/win/bbhost-win-<rev>.zip) is uploaded and unpacked once;
# without it the newest package already there runs. VAR=value pairs go into
# the game's environment. The password comes from SSHPASS (sshpass -e) and is
# never written anywhere. Headless because an RDP session caps presentation
# near 30 Hz; a windowed check needs someone at the machine.
#
# The run: Cross taps through the title and Continue (20-50 s), then seven
# rounds of the Linux soak's moves on a pad - run and sprint while turning the
# camera right, left and turning left, three R1 attacks, back, right while
# turning, six seconds still - from 70 s, ending at flip WIN_FLIPS (16500).
#
# WIN_HOST (user@address, required), WIN_DIR (C:\bbtest\perf, holds the
# eboot), WIN_APP0 (the game folder there, required), WIN_SEED (a tar.gz
# holding saves/ to start from) set where things are. Output: build/win-perf/NAME.log.
set -u
cd "$(dirname "$0")/.."
NAME=${1:?usage: tools/win_perf.sh NAME [ZIP] [VAR=value ...]}
shift
ZIP=
if [ $# -gt 0 ] && [[ "$1" == *.zip ]]; then ZIP=$1; shift; fi
[ -n "${SSHPASS:-}" ] || { echo "set SSHPASS (the Windows account's password) in the environment"; exit 1; }
HOST=${WIN_HOST:?set WIN_HOST to user@address of the Windows machine}
DIR=${WIN_DIR:-'C:\bbtest\perf'}
APP0=${WIN_APP0:?set WIN_APP0 to the game folder on the Windows machine}
SEED=${WIN_SEED:-'C:\bbtest\snd\seed.tar.gz'}
FLIPS=${WIN_FLIPS:-16500}
FDIR=${DIR//\\//}
OUT=build/win-perf
mkdir -p "$OUT"
export SSHPASS

# The SSH banner exchange fails about every other connect on the laptop.
rcmd() {
    local out rc i
    for i in 1 2 3 4 5 6; do
        out=$(sshpass -e ssh -o StrictHostKeyChecking=no -o ConnectTimeout=15 "$HOST" "$1" 2>&1); rc=$?
        if [ $rc -ne 255 ]; then printf '%s\n' "$out" | grep -v '^\*\*'; return $rc; fi
        sleep 3
    done
    printf '%s\n' "$out"; return 255
}
rput() {
    local i
    for i in 1 2 3 4 5 6; do
        sshpass -e scp -q -o StrictHostKeyChecking=no -o ConnectTimeout=15 "$1" "$HOST:$2" 2>/dev/null && return 0
        sleep 3
    done
    echo "copy to $2 failed"; return 1
}
rget() {
    local i
    for i in 1 2 3 4 5 6; do
        sshpass -e scp -q -o StrictHostKeyChecking=no -o ConnectTimeout=15 "$HOST:$1" "$2" 2>/dev/null && return 0
        sleep 3
    done
    echo "copy from $1 failed"; return 1
}

rcmd "if not exist $DIR mkdir $DIR" >/dev/null
BUILD=
if [ -n "$ZIP" ]; then
    BUILD=$(basename "$ZIP" .zip)
    rcmd "if exist $DIR\\$BUILD rmdir /s /q $DIR\\$BUILD" >/dev/null
    rput "$ZIP" "$FDIR/$BUILD.zip" || exit 1
    rcmd "cd /d $DIR && tar -xf $BUILD.zip && del $BUILD.zip" >/dev/null
else
    BUILD=$(rcmd "dir /b /ad /o-d $DIR\\bbhost-win-*" | tr -d '\r' | head -1)
fi
[ -n "$BUILD" ] || { echo "no package on the machine; pass a ZIP"; exit 1; }

# The config: no server (Play Offline), the frozen seed's data under DIR.
cat > "$OUT/perf.toml" <<TOML
[paths]
app0 = "$APP0"
eboot = "$FDIR/eboot-109-decrypted.bin"
data = "$FDIR/data"

[online]
host = "127.0.0.1"
scheme = "http"
verify_tls = false
online_id = "Hunter"

[video]
width = 1920
height = 1080

[startup]
skip_intro = true
TOML
taps="20:cross,25:cross,30:cross,35:cross,40:cross,45:cross,50:cross"
for r in 0 1 2 3 4 5 6; do
    t=$((70 + 27 * r))
    taps+=",$t:lup:6000,$t:circle:6000,$t:rright:6000"
    taps+=",$((t + 6)):lleft:5000,$((t + 6)):circle:5000,$((t + 6)):rleft:5000"
    taps+=",$((t + 11)):r1,$((t + 12)):r1,$((t + 13)):r1"
    taps+=",$((t + 14)):ldown:4000,$((t + 14)):circle:4000"
    taps+=",$((t + 18)):lright:3000,$((t + 18)):rright:3000"
done
env_lines=
for kv in "$@"; do env_lines+="set \"$kv\""$'\n'; done
cat > "$OUT/perf.cmd" <<CMD
@echo off
rem Written by tools/win_perf.sh: perf.cmd NAME BUILD - the frozen seed, headless, frame statistics.
cd /d $DIR\\%2
set BBHOST_NO_GAMEPAD=1
set BBHOST_SKIP_INTRO=1
set BBHOST_HEADLESS=1
set BBHOST_FRAME_STATS=1
set BBHOST_GAME_FPS=60
set BBHOST_EXIT_FLIP=$FLIPS
set BBHOST_CONFIG_DIR=$DIR\\cfg
set BBHOST_OPTIONS_PATH=$DIR\\cfg\\bbhost-options.toml
set BBHOST_AUTOPRESS=$taps
${env_lines}if not exist $DIR\\cfg mkdir $DIR\\cfg
echo [options]> $DIR\\cfg\\bbhost-options.toml
echo frame_cap = "60">> $DIR\\cfg\\bbhost-options.toml
if not exist $DIR\\data mkdir $DIR\\data
rmdir /s /q $DIR\\data\\saves 2>nul
tar -xzf $SEED -C $DIR\\data saves
bbhost.exe --config $DIR\\perf.toml > $DIR\\run-%1.log 2>&1
echo exit %ERRORLEVEL% >> $DIR\\run-%1.log
CMD
sed -i 's/$/\r/' "$OUT/perf.cmd" "$OUT/perf.toml"
rput "$OUT/perf.toml" "$FDIR/perf.toml" || exit 1
rput "$OUT/perf.cmd" "$FDIR/perf.cmd" || exit 1
rput tools/win/gpumem.ps1 "$FDIR/gpumem.ps1" || exit 1
echo "win_perf: $BUILD on $HOST, $FLIPS flips"
rcmd "$DIR\\perf.cmd $NAME $BUILD" >/dev/null
rget "$FDIR/run-$NAME.log" "$OUT/$NAME.log" || exit 1
tail -1 "$OUT/$NAME.log" | tr -d '\r'
python3 tools/soak_summary.py "$OUT/$NAME.log" --after-world 20
