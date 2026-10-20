#!/bin/bash
# SprjFlipper::Update compare mode: the guest's runs, ours replays each frame; a world walk at the given game fps.
cd "$(dirname "$0")/.."
fps=$1; flips=$2
export DISPLAY=:78 BBHOST_NO_GAMEPAD=1 BBHOST_SKIP_INTRO=1 BBHOST_FRAME_SOURCE=2 BBHOST_GAME_FPS=$fps
BBHOST_AUTOPRESS="f1000:down,f1040:cross,f1150:cross,f1300:cross,f1450:cross,f1600:cross,f1800:cross,f2000:cross,f3300:lup:6000,f3600:lright:4000" \
  BBHOST_EXIT_FLIP=$flips timeout -k 5 420 tmp/np-host-run.sh > build/frame-compare-$fps.log 2>&1
echo "EXIT=$?" >> build/frame-compare-$fps.log
