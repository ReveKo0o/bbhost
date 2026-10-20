#!/bin/bash
# The smoke run for the Windows cross build: bbhost.exe under wine must reach
# flip SMOKE_FLIPS (default 18, the same as the Linux smoke's title-screen
# progress) and end itself there (BBHOST_EXIT_FLIP, exit 0) with the exit
# reports and no crash report. `timeout` cannot end a wine run with the
# reports (its SIGTERM never reaches the exe), which is why the run ends from
# inside.
#   tools/win_smoke.sh EXE LOG SRCDIR        (cmake --build build-win --target smoke)
# Needs a config with Z:-drive paths (BBHOST_WIN_CONFIG, default
# build/win/bbhost-win.toml), a wine prefix (WINEPREFIX, default
# build/win/wine) and a display (DISPLAY, default :77 - an Xvfb).
set -u
bin="$1"; log="$2"; src="$3"
cd "$src"
cfg=${BBHOST_WIN_CONFIG:-$src/build/win/bbhost-win.toml}
if [ ! -f "$cfg" ]; then
    echo "smoke-win: no config at $cfg (paths must be Z:/... for wine)"
    exit 1
fi
export WINEPREFIX=${WINEPREFIX:-$src/build/win/wine} WINEDEBUG=${WINEDEBUG:--all} DISPLAY=${DISPLAY:-:77}
export BBHOST_SMOKE=1 BBHOST_EXIT_FLIP=${SMOKE_FLIPS:-18} BBHOST_NO_GAMEPAD=1
# The window has to outlive its first seconds: made on a thread that then
# exited, Windows destroyed it while the game ran on without it, and a smoke
# that only counted flips passed. Looked for 12 s in, on the run's display.
rm -f "$log.window"
if command -v xwininfo >/dev/null; then
    (sleep 12; if xwininfo -display "$DISPLAY" -root -tree 2>/dev/null | grep -q 'Bloodborne (bbhost)'; then echo yes; else echo no; fi > "$log.window") &
fi
timeout -k 5 ${SMOKE_SECS:-240} wine "$bin" --config "Z:$cfg" > "$log" 2>&1; rc=$?
wait
wineserver -k 2>/dev/null
"$src/tools/coverage.py" "$log" || exit 1
if [ "$rc" -ne 0 ]; then
    echo "smoke-win failed: exit $rc (124 = flip $BBHOST_EXIT_FLIP not reached in ${SMOKE_SECS:-240} s)"
    exit 1
fi
if ! grep -q 'exit: flip .* reached' "$log"; then
    echo "smoke-win failed: the run ended without reaching its flip"
    exit 1
fi
if grep -q 'SIGSEGV pc=\|crash: unhandled' "$log"; then
    echo "smoke-win failed: crash report in the log"
    exit 1
fi
if [ -f "$log.window" ] && [ "$(cat "$log.window")" != yes ]; then
    echo "smoke-win failed: no game window on $DISPLAY 12 s into the run"
    exit 1
fi
if ! grep -q 'sceGnmSubmitAndFlip #' "$log"; then
    echo "smoke-win failed: no submitted flips"
    exit 1
fi
echo "smoke-win: ok (flip $BBHOST_EXIT_FLIP reached, exit 0)"
