#!/bin/bash
# Drive bbhost to a menu screen and photograph it.
#
# Blind autopress navigation by wall time does not reproduce: the same script
# at 30 fps and at 25 fps (a run still loading) presses at different points in
# the game and lands on different rows. Four runs were lost to that. Every tap
# here is therefore keyed to a **flip**, which is the game's own clock, and
# every screenshot waits for the log to report the tap it follows rather than
# for a stopwatch.
#
#   tools/menu_run.sh <out-dir> "<extra taps>" "<shots>"
#
# taps:  f<flip>:<button>[:<hold ms>],...   appended to the walk to System
# shots: <flip>:<name>,...                  taken once that tap is logged
#
# Env: anything bbhost reads (BBHOST_PC_OPTIONS=0 drops the PC rows).
set -u
export BBHOST_SETUP_WINDOW=0  # a harness run never stops at the setup window
cd "$(dirname "$0")/.."
OUT=${1:?out dir}; EXTRA=${2:-}; SHOTS=${3:-}
LOG=$OUT/run.log
rm -rf "$OUT"; mkdir -p "$OUT"

# Always the headless display: the shell may have DISPLAY pointing at a real
# desktop, and a test run belongs nowhere near it.
export DISPLAY=${MENU_RUN_DISPLAY:-:77}
export BBHOST_SKIP_INTRO=${BBHOST_SKIP_INTRO:-1}
# A copy of the settings, so nothing the walk changes lands in the player's.
[ -f bbhost-options.toml ] && cp bbhost-options.toml "$OUT/bbhost-options.toml"
export BBHOST_OPTIONS_PATH=$OUT/bbhost-options.toml
export BBHOST_AUTOPRESS_HOLD_MS=${BBHOST_AUTOPRESS_HOLD_MS:-150}

# The walk from boot to the System list, in flips, measured from a run that
# worked. The title menu opens with **Play Offline** already under the cursor,
# so pressing Down there wraps onto Play Online and the run ends in a network
# error - do not add one. Menus wrap, so Up from the top row is the last row.
# MENU_RUN_WALK replaces it - for in-game screens, where the walk is a Continue
# rather than a trip into System.
WALK=${MENU_RUN_WALK:-"f1769:cross,f2309:cross,f3029:up,f3269:up,f3509:cross"}
export BBHOST_AUTOPRESS="$WALK${EXTRA:+,$EXTRA}"

timeout "${BBHOST_TIMEOUT:-300}" ./build/bbhost > "$LOG" 2>&1 &
PID=$!

shot_after() {  # <flip> <name>
    local flip=$1 name=$2 n=0
    while ! grep -q "at flip $flip\$" "$LOG" 2>/dev/null; do
        kill -0 $PID 2>/dev/null || return 1
        sleep 1; n=$((n+1)); [ $n -gt 400 ] && return 1
    done
    sleep 3
    import -display "$DISPLAY" -window root "$OUT/$name.png" 2>/dev/null && echo "shot $name (after flip $flip)"
}

IFS=',' read -ra want <<< "$SHOTS"
for s in "${want[@]:-}"; do
    [ -z "$s" ] && continue
    shot_after "${s%%:*}" "${s##*:}"
done
wait $PID
echo "=== taps ==="; grep -o "autopress: .*" "$LOG"
echo "=== pc-options ==="; grep -o "pc-options: .*" "$LOG"
echo "=== crash ==="; grep -c "dumped core" "$LOG"
