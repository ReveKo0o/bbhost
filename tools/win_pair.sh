#!/bin/bash
# The cross-platform co-op pair: the Linux host on display :78 and the Windows build
# under wine as the guest on :77, both on the local private server, both driven by scripted pad
# presses (BBHOST_AUTOPRESS) because X input never reaches the game under wine. Each side opens
# the inventory, scrolls the Consumables list to its end and taps up a known number of rows to
# its bell (host: Beckoning Bell, row 155 of 187; guest: Small Resonant Bell, row 157), then
# confirms. Expect "np: member 2 (Hunter2) joined room N" on the host and "np: joined room N"
# on the guest, and the host's plate over its character in the guest's frame dump.
#   tools/win_pair.sh            (logs and the dump under build/win/pair4-*; PAIR_SECS, GUEST_DUMP)
# Needs: the private server on 20443/18671, tmp/np-host.toml + tmp/np-host-data,
# build/win/np-guest.toml + build/win/np-guest-data (the guest's save cloned from the host's),
# tmp/np-host-run.sh and tmp/win-guest-run.sh, and PAIR_SAVE (a lamp save: userdata0001/0010).
# Co-op pair, both sides on scripted pad presses (the list is scrolled to its end and then
# tapped up a known number of rows, so the bell row does not depend on the repeat rate) at the game's 30 fps: Linux host on :78 rings the
# Beckoning Bell (inventory row 155), the Windows guest under wine on :77 the Small Resonant Bell (row 157).
cd "$(dirname "$0")/.."
S=${PAIR_SAVE:?set PAIR_SAVE to the folder holding userdata0001 and userdata0010}
cp -p $S/userdata0001 $S/userdata0010 tmp/np-host-data/saves/SPRJ0005/
cp -p $S/userdata0001 $S/userdata0010 build/win/np-guest-data/saves/SPRJ0005/
export BBHOST_NP_TRACE=1 BBHOST_NP_TEST=probe BBHOST_TEST_INSIGHT=5 BBHOST_NO_GAMEPAD=1 BBHOST_SKIP_INTRO=1 BBHOST_POINTER_LOG=1
rm -f build/frame-${GUEST_DUMP:-9000}.ppm
DISPLAY=:78 BBHOST_AUTOPRESS="18:cross,24:cross,30:cross,36:cross,42:cross,48:cross,54:cross,f2400:options,f2500:cross,f2600:down:25000,f3400:up:100,f3420:up:100,f3440:up:100,f3460:up:100,f3480:up:100,f3500:up:100,f3520:up:100,f3540:up:100,f3560:up:100,f3580:up:100,f3600:up:100,f3620:up:100,f3640:up:100,f3660:up:100,f3680:up:100,f3700:up:100,f3720:up:100,f3740:up:100,f3760:up:100,f3780:up:100,f3800:up:100,f3820:up:100,f3840:up:100,f3860:up:100,f3880:up:100,f3900:up:100,f3920:up:100,f3940:up:100,f3960:up:100,f3980:up:100,f4000:up:100,f4060:cross,f4140:cross" \
  timeout -k 5 ${PAIR_SECS:-480} tmp/np-host-run.sh > build/win/pair4-host.log 2>&1 &
HOST=$!
sleep 5
WINEPREFIX=$PWD/build/win/wine WINEDEBUG=-all DISPLAY=:77 \
  BBHOST_AUTOPRESS="f1000:cross,f1150:cross,f1300:cross,f1450:cross,f1600:cross,f1800:cross,f2000:cross,f3600:options,f3700:cross,f3800:down:25000,f4600:up:100,f4620:up:100,f4640:up:100,f4660:up:100,f4680:up:100,f4700:up:100,f4720:up:100,f4740:up:100,f4760:up:100,f4780:up:100,f4800:up:100,f4820:up:100,f4840:up:100,f4860:up:100,f4880:up:100,f4900:up:100,f4920:up:100,f4940:up:100,f4960:up:100,f4980:up:100,f5000:up:100,f5020:up:100,f5040:up:100,f5060:up:100,f5080:up:100,f5100:up:100,f5120:up:100,f5140:up:100,f5160:up:100,f5220:cross,f5300:cross" \
  BBHOST_DUMP_FRAME=${GUEST_DUMP:-9000} timeout -k 5 ${PAIR_SECS:-480} tmp/win-guest-run.sh > build/win/pair4-guest.log 2>&1
wait $HOST
WINEPREFIX=$PWD/build/win/wine wineserver -k 2>/dev/null
echo PAIR-DONE >> build/win/pair4-guest.log
