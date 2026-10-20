#!/bin/bash
# A sampled world walk on Linux for tools/surface.py: guest frames of the main loop and the
# engine's worker threads through perf_events, from the world's first frames to the exit flip.
cd "$(dirname "$0")/.."
export DISPLAY=:78 BBHOST_NO_GAMEPAD=1 BBHOST_SKIP_INTRO=1 BBHOST_NP_TEST=probe
export BBHOST_SAMPLE="main,FD4JobWorker,EzWorkPool,GXRenderThread,GXWorker,MOMainThread,HavokWorkerThread,SpClothVertexUpdate,Core.Res,FaceGenMan,FMOD,NexusRevolution,bb-cp0"
export BBHOST_SAMPLE_PERF=1 BBHOST_SAMPLE_HZ=997 BBHOST_SAMPLE_FROM_FLIP=2200 BBHOST_SAMPLE_OUT=build/samples-surface
BBHOST_AUTOPRESS="f1000:down,f1040:cross,f1150:cross,f1300:cross,f1450:cross,f1600:cross,f1800:cross,f2000:cross,f3300:lup:6000,f3600:lright:4000" \
  BBHOST_EXIT_FLIP=5200 timeout -k 5 420 tmp/np-host-run.sh > build/surface-run.log 2>&1
echo "EXIT=$?" >> build/surface-run.log
