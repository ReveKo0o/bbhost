#!/bin/bash
# A/B the menu flash with the opportunity count on the record.
#
#   VARS='BBHOST_BUFFER_SHADOW=0' tools/menu_flash_ab.sh
#
# Twelve menu transitions, and it reports flashes *against how many chances it
# had*. This matters more than it sounds: a slower configuration reaches the
# world later and gets through fewer transitions in the same wall clock, so a
# bare "0 flashes" reads as a fix when it only means the test never ran.
# BBHOST_VERTEX_INPUT=0, BBHOST_BUFFER_SHADOW=0 and BBHOST_SHADOW_VERIFY=1 all
# looked like fixes on the short script at about a third of the world frames;
# on this one BBHOST_VERTEX_INPUT=0 flashes like the rest.
#
# A run is comparable when its world-frame count is within about a third of the
# baseline's (~20,000) and it got 17 or more list events. Baseline is 3-4
# flashes.
cd "$(dirname "$0")/.."
O=build/catch-ab2
env $VARS BBHOST_DUMP_ON_CHANGE=0.25 \
  MENU_DRIVE_OUT=$O MENU_DRIVE_TIMEOUT=${MDT:-520} timeout $((${MDT:-520}+20)) tools/menu_drive.sh \
  title sleep:4 clicknow:0 waitlog:area.glare.luminance sleep:8 \
  key:Escape sleep:3 key:Escape sleep:3 key:Escape sleep:3 key:Escape sleep:3 \
  key:Escape sleep:3 key:Escape sleep:3 key:Escape sleep:3 key:Escape sleep:3 \
  key:Escape sleep:3 key:Escape sleep:3 key:Escape sleep:3 key:Escape sleep:3 >/dev/null 2>&1
J=$(grep -h 'averages' $O/run.log | grep -cE 'averages 0.[45][0-9] and moved 0.4')
L=$(grep -c 'pointer: list' $O/run.log)
B=$(grep -hoE 'pool: [0-9]+ borrows' $O/run.log | grep -oE '[0-9]+')
echo "VARS=${VARS:-none}: $J flashes over $L list events, ${B:-0} world frames"
