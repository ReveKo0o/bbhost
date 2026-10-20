#!/bin/bash
# Run the game on a virtual display and drive its menus with the mouse.
#
#   tools/menu_drive.sh [steps...]      (see tools/menu_drive.py for the steps)
#
# Output goes to build/menudrive/: run.log and every screenshot a step takes.
# DISPLAY defaults to :77 (an Xvfb 1920x1080x24). The run is isolated from the
# machine it is on: no controller (BBHOST_NO_GAMEPAD), no scripted presses
# (BBHOST_AUTOPRESS unset - a flip-timed one fires wherever the mouse has got
# to by then). MENU_DRIVE_TIMEOUT caps the run (default 520 s).
set -u
export BBHOST_SETUP_WINDOW=0  # a harness run never stops at the setup window
cd "$(dirname "$0")/.."
OUT=${MENU_DRIVE_OUT:-build/menudrive}; rm -rf "$OUT"; mkdir -p "$OUT"
export DISPLAY=${DISPLAY_DRIVE:-:77} BBHOST_SKIP_INTRO=1
export BBHOST_POINTER_LOG=1 BBHOST_PAD_LOG=1 BBHOST_NO_GAMEPAD=1
# Pad steps (padplug, pad:, padstick:) drive a virtual controller instead: the
# game opens that one alone (BBHOST_GAMEPAD_MATCH), never the one on the desk.
case " $* " in *" pad"*) unset BBHOST_NO_GAMEPAD; export BBHOST_GAMEPAD_MATCH=045e:02ea ;; esac
unset BBHOST_AUTOPRESS
# The run's settings are a copy: a step that changes one (a key binding, a
# toggle) must not write the player's bbhost-options.toml.
[ -f bbhost-options.toml ] && cp bbhost-options.toml "$OUT/bbhost-options.toml"
export BBHOST_OPTIONS_PATH=$OUT/bbhost-options.toml
# -k: a game that does not end on SIGTERM (seen online) is killed 20 s later.
timeout -k 20 "${MENU_DRIVE_TIMEOUT:-520}" "${BBHOST_BIN:-./build/bbhost}" > "$OUT/run.log" 2>&1 &
PID=$!
sleep 3
WID=
for _ in $(seq 1 20); do
  WID=$(xdotool search --pid $PID 2>/dev/null | tail -1); [ -n "$WID" ] && break; sleep 1
done
# Under a wrapper (a debugger) the window belongs to a child: find it by name.
[ -z "$WID" ] && for _ in $(seq 1 60); do
  WID=$(xdotool search --name "bbhost" 2>/dev/null | tail -1); [ -n "$WID" ] && break; sleep 1
done
[ -n "$WID" ] && xdotool windowactivate --sync "$WID" 2>/dev/null
export WID
# The host only knows the pointer is over the window once it has moved there.
if [ -n "$WID" ]; then
  eval "$(xdotool getwindowgeometry --shell "$WID")"
  xdotool mousemove $((X + WIDTH / 2 - 10)) $((Y + HEIGHT / 2 - 10)); sleep 0.3; xdotool mousemove $((X + WIDTH / 2)) $((Y + HEIGHT / 2))
else
  xdotool mousemove 950 530; sleep 0.3; xdotool mousemove 960 540
fi
python3 tools/menu_drive.py "$OUT" "$@"
kill $PID 2>/dev/null; wait $PID 2>/dev/null
grep -E "pointer: list .*(hover|click)|mouse: menu|Segmentation|dumped core" "$OUT/run.log" | head -40
