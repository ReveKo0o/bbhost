#!/bin/bash
# A frame-rate soak in the world: load the save and run, turn and attack for
# about three minutes, with frame statistics every second - then summarise
# the main loop's work per frame and the hitches.
#
#   tools/perf_soak.sh [NAME] [VAR=value...]
#
# NAME (default perf-soak) names build/NAME/ (run.log). VAR=value pairs go
# into the game's environment (BBHOST_TEX_ASYNC=0, BBHOST_HLE_COUNT=1, ...);
# BBHOST_EXE=path runs another binary (keep it out of build/NAME: the menu
# driver empties that directory). Like tools/perf_tour.sh it plays on a copy
# of data/ (tmp/data-scratch), at the 60 fps frame cap, on the menu driver's
# Xvfb display, pinned to NUMA node PERF_SOAK_NODE (default 0).
# PERF_SOAK_DATA=dir plays on a copy of dir instead: a frozen seed keeps runs
# comparable after the real save moves on (a played session saves where it
# stopped, and the soak then starts somewhere else). PERF_SOAK_TOML=file is
# the config the run's is made from (default bbhost.toml): a test config that
# points at the dev or a local server, never the one players use.
#
# The numbers to compare, after the first 40 seconds (the load):
#   work avg / p95   the main loop's work per frame, the limiter's spin left
#                    out (engine/frame_rate.cpp) - under 16.7 ms holds 60
#   over 16.7        frames whose work passed it
#   stalls           command-processor stalls (`stall:` lines, a flip gap)
#   frames >= 50 ms  main-loop hitches (`main loop:` lines)
# Runs differ (combat, streaming): compare two or more of each, alternating.
set -u
export BBHOST_SETUP_WINDOW=0  # a harness run never stops at the setup window
cd "$(dirname "$0")/.."
NAME=${1:-perf-soak}
[ $# -gt 0 ] && shift
BASE_TOML=${PERF_SOAK_TOML:-bbhost.toml}
[ -f "$BASE_TOML" ] || { echo "no $BASE_TOML (see bbhost.example.toml)"; exit 1; }
mkdir -p tmp
rm -rf tmp/data-scratch
data=$(sed -n 's/^data *= *"\(.*\)"/\1/p' "$BASE_TOML" | head -1)
cp -a "${PERF_SOAK_DATA:-${data:-data}}" tmp/data-scratch
sed -e "s|^data *=.*|data = \"$PWD/tmp/data-scratch\"|" -e "s|^mods *=.*|mods = \"$PWD/tmp/data-scratch/mods\"|" "$BASE_TOML" > tmp/perf-soak.toml
cat > tmp/perf-soak-run.sh <<EOF
#!/bin/bash
if [ -n "\${BB_OPT:-}" ] && [ -n "\${BBHOST_OPTIONS_PATH:-}" ]; then
  sed -i "/^frame_cap = /d" "\$BBHOST_OPTIONS_PATH"; sed -i "/^\[options\]/a \$BB_OPT" "\$BBHOST_OPTIONS_PATH"
fi
export SDL_AUDIO_DRIVER=\${SDL_AUDIO_DRIVER:-dummy}
pin=""
command -v numactl >/dev/null && pin="numactl --cpunodebind=${PERF_SOAK_NODE:-0} --membind=${PERF_SOAK_NODE:-0}"
exec \$pin "\${BBHOST_EXE:-$PWD/build/bbhost}" --config "$PWD/tmp/perf-soak.toml" "\$@"
EOF
chmod +x tmp/perf-soak-run.sh
steps=(title sleep:4 clicknow:0 waitlog:area.glare.luminance sleep:5)
for _ in 1 2 3 4 5 6 7; do steps+=(keys:w+space+l,6 keys:a+space+j,5 lclick:3 keys:s+space,4 keys:d+l,3 sleep:6); done
env "$@" BBHOST_GAME_FPS=60 BB_OPT='frame_cap = "60"' BBHOST_FRAME_STATS=1 MENU_DRIVE_OUT="build/$NAME" MENU_DRIVE_TIMEOUT=500 \
    BBHOST_BIN="$PWD/tmp/perf-soak-run.sh" timeout 600 tools/menu_drive.sh "${steps[@]}" > /dev/null 2>&1
python3 - "build/$NAME/run.log" <<'PY'
import re, sys
lines = open(sys.argv[1]).read().split('\n')
fi = [i for i, l in enumerate(lines) if l.startswith('[bbhost] frames:')]
if len(fi) < 60:
    sys.exit('the run did not reach the world (%d seconds of frame statistics)' % len(fi))
rows = []
for i in fi[40:]:
    l = lines[i]
    f = float(re.search(r'\(([0-9.]+)/s\)', l).group(1))
    k = re.search(r'main loop work avg ([0-9.]+) p95 ([0-9.]+) max ([0-9.]+) ms, (\d+) over', l)
    w = re.search(r'main loop waited (\d+) ms', l)
    rows.append((f, float(k.group(1)) if k else 0, float(k.group(2)) if k else 0, int(k.group(4)) if k else 0, int(w.group(1)) if w else 0))
after = lines[fi[40]:]
stalls = [l for l in after if l.startswith('[bbhost] stall:')]
longf = [float(m.group(1)) for l in after for m in [re.search(r'main loop: a ([0-9.]+) ms frame', l)] if m]
n = len(rows)
frames = sum(r[0] for r in rows)
print('%d s: fps mean %.2f, seconds under 58: %d; main loop work avg %.2f ms, p95 %.2f, over 16.7: %d frames (%.1f%%), '
      'waited %.0f ms/s; stalls %d; frames >= 50 ms %d (worst %.0f)' %
      (n, frames / n, sum(1 for r in rows if r[0] < 58), sum(r[1] for r in rows) / n, sum(r[2] for r in rows) / n,
       sum(r[3] for r in rows), 100.0 * sum(r[3] for r in rows) / max(1, frames), sum(r[4] for r in rows) / n,
       len(stalls), sum(1 for x in longf if x >= 50), max(longf) if longf else 0))
PY
