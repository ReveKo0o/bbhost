#!/bin/bash
# Five real game instances in one session: a host, two cooperators and two
# invaders - the most Bloodborne's rooms hold (CreateJoinRoom asks maxSlot 5).
#
#   host    level 30 (the Chime Maiden's condition), rings the Beckoning Bell
#           twice; once a cooperator is in, the maiden appears and rings and
#           the host asks the sign server for invaders
#   coop1/2 ring the Small Resonant Bell and stay
#   red1/2  ring the Sinister Resonant Bell a few minutes in and stay
#
# Outside the chalice dungeons the maiden stops ringing when one invader is
# in (common events 9240/9260; the maps' own xx04710/xx04720), so the second
# invader joins only with NET_MODS naming an overlay folder whose
# dvdroot_ps4/event files lift that to two. It is
# copied into every game's data mods: every client runs the map's events,
# and a guest whose stop event is the shipped one ends the host's maiden at
# the first invader. NET_HOST_MODS gives it to the host only, to show that.
#
# NET_SSINFO_SET passes BB_SSINFO_SET to the test server, which sets ss.info
# values, for instance the guard timer that keeps a host from taking another
# invader for 7 minutes of game time after one has left:
# NET_SSINFO_SET=SummonDataGetListInvationGuardTimerLimitationTimeRate=0.
#
#   NET_SEED=<dir> [NET_MODS=<dir>|NET_HOST_MODS=<dir>] [NET_SSINFO_SET=...] tools/nettest/game_five.sh
#
# Same server, configs and displays as game_trio.sh, plus :81 and :82 for the
# invaders. The seed stands in Central Yharnam (m24_01), whose maiden is
# entity 2410750 with area flag 2410 (BBHOST_TEST_FLAGS turns it off).
set -u
cd "$(dirname "$0")/../.."
R=$PWD
OUT=${NET_OUT:-$R/build/nettest-five}
TREE=${NET_SERVER_TREE:?NET_SERVER_TREE: the private server package directory}
SEED=${NET_SEED:?NET_SEED: a folder with userdata0001 and userdata0010}
APP0=${BBHOST_APP0:?BBHOST_APP0: the game folder that contains dvdroot_ps4}
rm -rf "$OUT"; mkdir -p "$OUT"

BB_SSINFO_SET=${NET_SSINFO_SET:-} python3 tools/nettest/bench.py --server-tree "$TREE" --serve --http-port 18671 --ssinfo-port 20443 \
  --stun-port 3478 --relay-ports 4000-4199 --work "$OUT/server" > "$OUT/server-run.log" 2>&1 &
SRV=$!
for _ in $(seq 1 60); do grep -q "server up" "$OUT/server-run.log" && break; sleep 1; done
grep -q "server up" "$OUT/server-run.log" || { echo "server did not start"; cat "$OUT/server-run.log"; kill $SRV; exit 1; }

role_cfg() {  # role online_id p2p_port data
  local d="$OUT/$1"; mkdir -p "$d/cfg"
  cat > "$d/bbhost.toml" <<EOF
[paths]
app0 = "$APP0"
data = "$4"
tmp = "$R/tmp"
eboot = "$R/eboot-109-decrypted.bin"

[online]
host = "127.0.0.1"
scheme = "http"
verify_tls = false
online_id = "$2"
np_server = "http://127.0.0.1:18671"
stun_server = "127.0.0.2:3478"
p2p_port = $3

[video]
width = 1920
height = 1080
fps_cap = 30

[startup]
skip_intro = true
EOF
  cat > "$d/run.sh" <<EOF
#!/bin/bash
export SDL_AUDIO_DRIVER=\${SDL_AUDIO_DRIVER:-dummy} BBHOST_CONFIG_DIR="$d/cfg"
exec "$R/build/bbhost" --config "$d/bbhost.toml" "\$@"
EOF
  chmod +x "$d/run.sh"
  mkdir -p "$4/saves/SPRJ0005"
  cp -p "$SEED/userdata0001" "$SEED/userdata0010" "$4/saves/SPRJ0005/"
  rm -rf "$4/mods/dvdroot_ps4/event"
}
D=$R/tmp
role_cfg host Hunter 9307 "$D/inv5-p1-data"
role_cfg coop1 Hunter2 9308 "$D/inv5-p2-data"
role_cfg coop2 Hunter3 9309 "$D/inv5-p3-data"
role_cfg red1 Hunter4 9310 "$D/inv5-p4-data"
role_cfg red2 Hunter5 9311 "$D/inv5-p5-data"
if [ -n "${NET_HOST_MODS:-}" ]; then
  mkdir -p "$D/inv5-p1-data/mods"
  cp -r "$NET_HOST_MODS/." "$D/inv5-p1-data/mods/"
  echo "host overlay: $(cd "$NET_HOST_MODS" && find . -type f | tr '\n' ' ')"
fi
if [ -n "${NET_MODS:-}" ]; then
  for n in 1 2 3 4 5; do
    mkdir -p "$D/inv5-p$n-data/mods"
    cp -r "$NET_MODS/." "$D/inv5-p$n-data/mods/"
  done
  echo "overlay for every game: $(cd "$NET_MODS" && find . -type f | tr '\n' ' ')"
fi

export BBHOST_GAME_FPS=60 BBHOST_NP_TRACE=1 BBHOST_NP_TEST=probe BBHOST_TEST_INSIGHT=5 BBHOST_RES=960x540
use() { echo "key:Escape waitnew:count.24.cells.6 sleep:1 click:0 waitnew:count.187.cells.1 sleep:1 move:1150,750 sleep:1 wheelto:$1 sleep:1 shot:$2 keyhold:Return,0.6 waitnew:count.2.cells.1.lines.5 sleep:1 click:0 sleep:4 shot:$2-used"; }
WORLD="title:online sleep:4 clicknow:0 waitlog:area.glare.luminance sleep:25"
C1="$WORLD $(use 157 c1-bell) waitnew:np:.joined.room sleep:60 shot:c1-coop sleep:1500 shot:c1-end"
C2="$WORLD $(use 157 c2-bell) waitnew:np:.joined.room sleep:60 shot:c2-coop sleep:1500 shot:c2-end"
R1="$WORLD sleep:300 $(use 159 r1-sinister) waitnew:np:.joined.room sleep:90 shot:r1-invade sleep:1200 shot:r1-end"
R2="$WORLD sleep:330 $(use 159 r2-sinister) waitnew:np:.joined.room sleep:90 shot:r2-invade sleep:1200 shot:r2-end"
HOST="$WORLD $(use 155 h-bell) waitnew:np:.member.*joined sleep:45 shot:h-coop1 $(use 155 h-bell2) waitnew:np:.member.*joined sleep:60 shot:h-coop2 sleep:30 shot:h-maiden waitn:3,np:.member.*joined sleep:60 shot:h-red1 waitn:4,np:.member.*joined sleep:60 shot:h-red2 sleep:300 shot:h-end"
drive() {  # role display steps...
  local role=$1 disp=$2; shift 2
  local extra=()
  # Every game renders the minimum unless NET_HOST_RENDER=full gives the host
  # its full picture: five full games share one GPU at a few frames a second,
  # where the menus crawl and more first summons fail.
  extra=(BBHOST_RENDER_MIN=1 BBHOST_BUFFER_SHADOW=0)
  if [ "$role" = host ]; then
    extra=(BBHOST_TEST_LEVEL=30 "BBHOST_TEST_FLAGS=2410=0,12414220?,12414221?,12414222?"
           BBHOST_NP_WATCH_FLAGS=2410,12414220,12414221,12414222,12414223)
    [ "${NET_HOST_RENDER:-min}" = full ] || extra+=(BBHOST_RENDER_MIN=1 BBHOST_BUFFER_SHADOW=0)
  fi
  env "${extra[@]}" MENU_DRIVE_OUT="$OUT/$role/drive" DISPLAY_DRIVE=$disp BBHOST_BIN="$OUT/$role/run.sh" \
    MENU_DRIVE_ONLINE_CONFIG="$OUT/$role/bbhost.toml" MENU_DRIVE_TIMEOUT=2400 \
    tools/menu_drive.sh "$@" > "$OUT/$role/drive.log" 2>&1
}
drive coop1 :79 $C1 & P1=$!
sleep 40
drive coop2 :80 $C2 & P2=$!
sleep 40
drive red1 :81 $R1 & P3=$!
sleep 20
drive red2 :82 $R2 & P4=$!
sleep 20
drive host :78 $HOST & P5=$!
wait $P5
# The host's run decides: once it is over, end the other games and their
# drivers, which may be in a long sleep step (by PID: the patterns name this
# run's folders, which no shell here carries).
for pid in $(pgrep -f "build/bbhost --config $OUT/") $(pgrep -f "menu_drive.py $OUT/"); do kill "$pid" 2>/dev/null; done
wait $P1 $P2 $P3 $P4 2>/dev/null
kill $SRV 2>/dev/null; wait $SRV 2>/dev/null
python3 tools/nettest/game_verdict.py --five "$OUT"
