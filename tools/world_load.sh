#!/usr/bin/env bash
# Load SPRJ0005 (Hunter's Dream save in data/saves/) through the title menu
# and dump world frames. Japanese SKU: Circle confirms.
#
#   ./tools/world_load.sh              # 5 min, dump flips 3600, 3900, 4200
#   ./tools/world_load.sh 360          # longer
#   BBHOST_HEADLESS=1 ./tools/world_load.sh
#
# Title sits at ~30 fps for the first minute (unclean-exit "OK", then PRESS
# START). Flip 3600 is the item-lore loading card; the world is in flight by
# 3900 (~5 fps so 4200 may miss a 240 s timeout).
set -euo pipefail
export BBHOST_SETUP_WINDOW=0  # a harness run never stops at the setup window
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
export BBHOST_AUTOPRESS="${BBHOST_AUTOPRESS:-70:circle,84:circle,100:circle,116:circle,130:circle,150:circle,170:circle}"
export BBHOST_GPU_PROFILE="${BBHOST_GPU_PROFILE:-1}"
export BBHOST_DUMP_FRAME="${BBHOST_DUMP_FRAME:-3600,3900,4200}"
# With DUMP_FRAME, dump every colour target too so a black display can be
# compared to the scene G-buffer / HDR (the 3900 transfusion shot was
# subtitle-on-black).
export BBHOST_DUMP_ALL="${BBHOST_DUMP_ALL:-1}"
LOG="${BBHOST_LOG:-tmp/world-load.log}"
mkdir -p tmp build
SECS="${1:-300}"
echo "world_load: timeout ${SECS}s log=$LOG autopress=$BBHOST_AUTOPRESS dump=$BBHOST_DUMP_FRAME"
set +e
timeout "$SECS" ./build/bbhost >"$LOG" 2>&1
rc=$?
set -e
echo "world_load: exit $rc"
rg -n 'sceGnmSubmitAndFlip #(600|900|1200)|fill:|dummy-images|draw-failures|V# at 0x|skip guest writeback|SIGSEGV|window closed|autopress:' "$LOG" | tail -n 40
if ls build/frame-*.ppm >/dev/null 2>&1; then
  echo "world_load: dumps:"
  ls -l build/frame-*.ppm
fi
exit "$rc"
