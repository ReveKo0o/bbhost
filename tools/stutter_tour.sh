#!/bin/bash
# Stutters while the world streams: the map-validation probe
# (tools/mapval_plugin.c, BBHOST_TEST_WARP=1) warps the player through one
# block - its lamps, player starts and enemy placements, 21 points 9.5 s
# apart for Central Yharnam - with frame statistics every second, the
# command processor's stall lines from 40 ms (BBHOST_STALL_MS) and, given
# BBHOST_HLE_COUNT=1, the call sites the main loop waited at in each long
# frame; then tools/stutter_summary.py on the tour's window.
#
#   tools/stutter_tour.sh [NAME] [VAR=value...]
#
# NAME (default stutter-tour) names build/NAME/ (run.log). VAR=value pairs go
# into the game's environment (BBHOST_HLE_COUNT=1, BBHOST_GPU_PROFILE=1,
# BBHOST_GX_RATE=1, BBHOST_SAMPLE=..., an A/B switch). As tools/perf_soak.sh:
# a copy of the data (PERF_SOAK_DATA, a frozen seed keeps runs comparable),
# a config made from PERF_SOAK_TOML (a test config pointing at the dev or a
# local server, never the players' one), the 60 fps cap, the menu driver's
# Xvfb display, NUMA node PERF_SOAK_NODE (0). STUTTER_BLOCK (m24_01_00_00)
# picks the block, STUTTER_HOLD (8) the seconds held at each point,
# STUTTER_START (40) the seconds in the world before the first warp (the
# frozen seed's scripted death and respawn are over by then).
#
# A tour takes ~6.5 minutes. Runs differ less than soaks (no combat), but a
# first visit's shader misses land in every run from the same seed: compare
# two or more of each arm, alternating, with the box idle.
set -u
cd "$(dirname "$0")/.."
NAME=${1:-stutter-tour}
[ $# -gt 0 ] && shift
BASE_TOML=${PERF_SOAK_TOML:-bbhost.toml}
[ -f "$BASE_TOML" ] || { echo "no $BASE_TOML (see bbhost.example.toml)"; exit 1; }
mkdir -p tmp
cfg() { sed -n "s|^$1 *= *\"\(.*\)\"|\1|p" "$BASE_TOML" | head -1; }
app0=$(cfg app0)
[ -d "$app0/dvdroot_ps4" ] || { echo "app0 not found ($app0)"; exit 1; }
# The probe and the tour.
cc -shared -fPIC -O2 -Wall -Iinclude -o tmp/stutter-mapval.so tools/mapval_plugin.c -lm || exit 1
rm -rf tmp/stutter-tour && mkdir -p tmp/stutter-tour
python3 tools/mapval_targets.py --app0 "$app0" --map "${STUTTER_BLOCK:-m24_01_00_00}" --out tmp/stutter-tour --kill "" --fall "" \
    --notes 0 --enemies 16 --hold "${STUTTER_HOLD:-8}" > /dev/null || exit 1
# The data copy, with the probe as its plugin, and the run's config.
rm -rf tmp/data-scratch
data=$(cfg data)
cp -a "${PERF_SOAK_DATA:-${data:-data}}" tmp/data-scratch
mkdir -p tmp/data-scratch/plugins && cp tmp/stutter-mapval.so tmp/data-scratch/plugins/mapval.so
sed -e "s|^data *=.*|data = \"$PWD/tmp/data-scratch\"|" -e "s|^mods *=.*|mods = \"$PWD/tmp/data-scratch/mods\"|" "$BASE_TOML" > tmp/stutter-tour.toml
cat > tmp/stutter-tour-run.sh <<EOS
#!/bin/bash
if [ -n "\${BB_OPT:-}" ] && [ -n "\${BBHOST_OPTIONS_PATH:-}" ]; then
  sed -i "/^frame_cap = /d" "\$BBHOST_OPTIONS_PATH"; sed -i "/^\[options\]/a \$BB_OPT" "\$BBHOST_OPTIONS_PATH"
fi
export SDL_AUDIO_DRIVER=\${SDL_AUDIO_DRIVER:-dummy}
pin=""
command -v numactl >/dev/null && pin="numactl --cpunodebind=${PERF_SOAK_NODE:-0} --membind=${PERF_SOAK_NODE:-0}"
exec \$pin "\${BBHOST_EXE:-$PWD/build/bbhost}" --config "$PWD/tmp/stutter-tour.toml" "\$@"
EOS
chmod +x tmp/stutter-tour-run.sh
# A config folder of its own: the run never writes the player's.
mkdir -p tmp/stutter-cfgdir
env BBHOST_CONFIG_DIR="${BBHOST_CONFIG_DIR:-$PWD/tmp/stutter-cfgdir}" "$@" BBHOST_GAME_FPS=60 BB_OPT='frame_cap = "60"' BBHOST_FRAME_STATS=1 BBHOST_TEST_WARP=1 BBHOST_STALL_MS=40 \
    BBHOST_MAPVAL_TOUR="$PWD/tmp/stutter-tour/tour.txt" BBHOST_MAPVAL_START="${STUTTER_START:-40}" \
    MENU_DRIVE_OUT="build/$NAME" MENU_DRIVE_TIMEOUT=420 BBHOST_BIN="$PWD/tmp/stutter-tour-run.sh" \
    timeout 520 tools/menu_drive.sh title sleep:4 clicknow:0 waitlog:area.glare.luminance 'waitlog:mapval tour-done' sleep:5 > /dev/null 2>&1
grep -aq 'mapval tour-done' "build/$NAME/run.log" 2>/dev/null || { echo "build/$NAME: the tour did not finish (see run.log)"; exit 1; }
python3 tools/stutter_summary.py "build/$NAME/run.log"
