#!/bin/bash
# A frame-rate benchmark in the world: load the save, turn the camera in
# steps and hold each view, walk on, turn again, run and attack - with frame
# statistics every second - and summarise the last 60 seconds.
#
#   tools/perf_tour.sh [NAME] [VAR=value...]
#
# NAME (default perf-tour) names build/NAME/ (run.log). VAR=value pairs go
# into the game's environment (BBHOST_SET_CACHE=0, BBHOST_GX_COST=1, ...).
# The run plays on a copy of data/ (tmp/data-scratch: the save walks, and the
# copy is thrown away), at the 60 fps frame cap, on the Xvfb display
# tools/menu_drive.sh uses, pinned to NUMA node PERF_TOUR_NODE (default 0)
# when numactl is there - unpinned runs are too noisy to compare.
set -u
export BBHOST_SETUP_WINDOW=0  # a harness run never stops at the setup window
cd "$(dirname "$0")/.."
NAME=${1:-perf-tour}
[ $# -gt 0 ] && shift
[ -f bbhost.toml ] || { echo "no bbhost.toml (see bbhost.example.toml)"; exit 1; }
mkdir -p tmp
rm -rf tmp/data-scratch
data=$(sed -n 's/^data *= *"\(.*\)"/\1/p' bbhost.toml | head -1)
cp -a "${data:-data}" tmp/data-scratch
# The config, with the data (and its mods) moved to the copy.
sed -e "s|^data *=.*|data = \"$PWD/tmp/data-scratch\"|" -e "s|^mods *=.*|mods = \"$PWD/tmp/data-scratch/mods\"|" bbhost.toml > tmp/perf-tour.toml
cat > tmp/perf-tour-run.sh <<EOF
#!/bin/bash
if [ -n "\${BB_OPT:-}" ] && [ -n "\${BBHOST_OPTIONS_PATH:-}" ]; then
  sed -i "/^frame_cap = /d" "\$BBHOST_OPTIONS_PATH"; sed -i "/^\[options\]/a \$BB_OPT" "\$BBHOST_OPTIONS_PATH"
fi
export SDL_AUDIO_DRIVER=\${SDL_AUDIO_DRIVER:-dummy}
pin=""
command -v numactl >/dev/null && pin="numactl --cpunodebind=${PERF_TOUR_NODE:-0} --membind=${PERF_TOUR_NODE:-0}"
exec \$pin "$PWD/build/bbhost" --config "$PWD/tmp/perf-tour.toml" "\$@"
EOF
chmod +x tmp/perf-tour-run.sh
steps=(title sleep:4 clicknow:0 waitlog:area.glare.luminance sleep:8)
for _ in 1 2 3 4 5 6; do steps+=(keys:l,0.4 sleep:3); done
steps+=(keys:w+space,4 sleep:2)
for _ in 1 2 3 4 5 6; do steps+=(keys:j,0.4 sleep:3); done
steps+=(keys:w+space+l,4 lclick:4 sleep:3)
limit=${PERF_TOUR_TIMEOUT:-260}  # seconds; raise it for runs that slow the boot (sampling the main thread)
env "$@" BBHOST_GAME_FPS=60 BB_OPT='frame_cap = "60"' BBHOST_FRAME_STATS=1 MENU_DRIVE_OUT="build/$NAME" MENU_DRIVE_TIMEOUT=$limit \
    BBHOST_BIN="$PWD/tmp/perf-tour-run.sh" timeout $((limit + 100)) tools/menu_drive.sh "${steps[@]}" > /dev/null 2>&1
python3 - "build/$NAME/run.log" <<'PY'
import re, sys
lines = [l for l in open(sys.argv[1]) if l.startswith('[bbhost] frames:')]
if len(lines) < 60:
    sys.exit('the run did not reach the world (%d seconds of frame statistics)' % len(lines))
v = []
for l in lines[-60:]:
    f = float(re.search(r'\(([0-9.]+)/s\)', l).group(1))
    m = re.search(r'bb-cp0:\d+ (\d+)%', l)
    v.append((f, int(m.group(1)) if m else 0))
fps = sorted(x[0] for x in v)
mid = v[8:48]
print('tour: fps min %.1f, p10 %.1f, mean %.1f; seconds under 58: %d' % (fps[0], fps[6], sum(fps) / 60, sum(1 for f in fps if f < 58)))
print('steady middle (40 s): fps %.2f, command processor %.1f%% = %.2f ms a frame' %
      (sum(x[0] for x in mid) / 40, sum(x[1] for x in mid) / 40, sum(x[1] / 100 / x[0] * 1000 for x in mid) / 40))
print('per second (fps@cp%): ' + ' '.join('%.0f@%d' % x for x in v))
PY
