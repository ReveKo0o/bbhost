#!/bin/bash
# The co-op pair across a NAT: the Linux host on :78 at the machine's LAN address,
# the Linux guest on :79 inside a pasta user-mode network namespace (10.0.2.15 behind pasta's
# NAT, no root needed), the private server (with --stun) on the LAN address. Same scripted
# presses as tools/win_pair.sh. Expect on the guest: "stun: ... sees our P2P port 9308 as
# <LAN address>:<mapped>" (a port that is not 9308), "net: punch Hunter ... peer heard", and
# "np: joined room N"; on the host: the member joining and its punch toward the mapped port.
#   PAIR_SAVE=<dir> tools/nat_pair.sh   (logs under build/nat-pair-*.log; PAIR_SECS, GUEST_DUMP)
# PAIR_SAVE holds the userdata0001/userdata0010 both sides start from (a lamp outdoors).
cd "$(dirname "$0")/.."
S=${PAIR_SAVE:?set PAIR_SAVE to the folder holding userdata0001 and userdata0010}
cp -p $S/userdata0001 $S/userdata0010 tmp/np-host-data/saves/SPRJ0005/
cp -p $S/userdata0001 $S/userdata0010 tmp/np-guest-data/saves/SPRJ0005/
export BBHOST_NP_TRACE=1 BBHOST_NP_TEST=probe BBHOST_TEST_INSIGHT=5 BBHOST_NO_GAMEPAD=1 BBHOST_SKIP_INTRO=1 BBHOST_POINTER_LOG=1
rm -f build/frame-${GUEST_DUMP:-9000}.ppm
DISPLAY=:78 BBHOST_AUTOPRESS="18:cross,24:cross,30:cross,36:cross,42:cross,48:cross,54:cross,f2400:options,f2500:cross,f2600:down:25000,f3400:up:100,f3420:up:100,f3440:up:100,f3460:up:100,f3480:up:100,f3500:up:100,f3520:up:100,f3540:up:100,f3560:up:100,f3580:up:100,f3600:up:100,f3620:up:100,f3640:up:100,f3660:up:100,f3680:up:100,f3700:up:100,f3720:up:100,f3740:up:100,f3760:up:100,f3780:up:100,f3800:up:100,f3820:up:100,f3840:up:100,f3860:up:100,f3880:up:100,f3900:up:100,f3920:up:100,f3940:up:100,f3960:up:100,f3980:up:100,f4000:up:100,f4060:cross,f4140:cross" \
  timeout -k 5 ${PAIR_SECS:-480} tmp/np-nat-host-run.sh > build/nat-pair-host.log 2>&1 &
HOST=$!
sleep 5
DISPLAY=:79 BBHOST_AUTOPRESS="f1000:cross,f1150:cross,f1300:cross,f1450:cross,f1600:cross,f1800:cross,f2000:cross,f3600:options,f3700:cross,f3800:down:25000,f4600:up:100,f4620:up:100,f4640:up:100,f4660:up:100,f4680:up:100,f4700:up:100,f4720:up:100,f4740:up:100,f4760:up:100,f4780:up:100,f4800:up:100,f4820:up:100,f4840:up:100,f4860:up:100,f4880:up:100,f4900:up:100,f4920:up:100,f4940:up:100,f4960:up:100,f4980:up:100,f5000:up:100,f5020:up:100,f5040:up:100,f5060:up:100,f5080:up:100,f5100:up:100,f5120:up:100,f5140:up:100,f5160:up:100,f5220:cross,f5300:cross" BBHOST_DUMP_FRAME=${GUEST_DUMP:-9000} \
  pasta --config-net -a 10.0.2.15 -n 24 -g 10.0.2.2 --quiet -- timeout -k 5 ${PAIR_SECS:-480} tmp/np-nat-guest-run.sh > build/nat-pair-guest.log 2>&1
wait $HOST
echo PAIR-DONE >> build/nat-pair-guest.log
