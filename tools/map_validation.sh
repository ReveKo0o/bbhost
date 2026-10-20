#!/bin/bash
# Ground truth for the private server's world map, gathered by playing.
#
#   tools/map_validation.sh [seed...]        seeds: central host clinic
#                                            (default: all three)
#   tools/map_validation.sh --blocks all|m22_00_00_00,m23_00_00_00 [--per-run N]
#       every block the lamps reach: the frozen seed's character with every
#       lamp lit (BBHOST_TEST_UNLOCK_LAMPS=1) travels to each block's first
#       lamp the way the Hunter's Dream headstones do (plugin lamp_warp:
#       ReturnPointParam row -> return point, then WarpNextStage_Bonfire) and
#       tours it: its lamps, player starts, notes, 20 navmesh points
#       (tools/mapval_blocks.py), a walk, a fall death. One game session per N
#       blocks (default 1, ~7 minutes each), run directory b_<first block>.
#
# The seeds (all three load into Central Yharnam, m24_01_00_00, today):
#   central  tmp/seed-data, Continue: the frozen seed's street character, which
#            enemies kill ~15 s in (a natural death at a known spot)
#   host     tmp/np-host-data, Continue: the online-test host character, at
#            the Central Yharnam lamp (not Cathedral Ward any more)
#   clinic   tmp/seed-data, Load Game slot 0: the early clinic character
#
# For each seed: a scratch copy of its save, the game online against a
# private server started here on 127.0.0.1 (never a real one), the
# map-validation probe (tools/mapval_plugin.c, BBHOST_TEST_WARP=1) logging the
# player's position every second and touring the loaded block - every lamp,
# the player starts, a few landmarks, one enemy kill and one fall death -
# while the game uploads its play logs. The server's ingest worker writes
# history.db; tools/mapval_dataset.py joins the two into the dataset.
#
# Everything lands in $MAPVAL_OUT (default tmp/mapval/<date-time>):
#   server/            the scratch server root (code copy, DB copy, playlogs)
#   <seed>/            run.log, drive.txt, tour.txt, targets.json, shots
#   dataset.json, targets.csv, events.csv, frame_check.json, summary.txt,
#   pixels.json/csv    (tools/mapval_pixels.py: the points through the world
#                      map's transform table, when it exists), timings.txt
# A seed takes 6-7 minutes. Calls with the same MAPVAL_OUT add seeds to one
# run (one seed per call when a caller caps a command's time); the dataset
# always covers every seed the directory holds.
#
# Env: MAPVAL_OUT, MAPVAL_DISPLAY (:77), MAPVAL_SERVER_SRC (the server
# checkout, read only), MAPVAL_PYTHON (its venv python), MAPVAL_HOLD (seconds
# the game stays up after the tour for the last uploads, 100), MAPVAL_START
# (seconds in the world before the tour, 45: the seed's scripted death and
# respawn are over by then), MAPVAL_ENEMIES (enemy placements per seed, 6),
# MAPVAL_CORPUS (a history.db of real players, read only, for the MapOffset
# check; "" for none), MAPVAL_TRANSFORMS (the world map's transform table),
# BBHOST_BIN (default build/bbhost).
set -u
export BBHOST_SETUP_WINDOW=0  # a harness run never stops at the setup window
cd "$(dirname "$0")/.."
REPO=$PWD
MAIN=$(cd "$(git rev-parse --git-common-dir)/.." && pwd)   # the main checkout: seeds, eboot, bbhost.toml
# --blocks LIST|all [--per-run N]: lamp travel through map blocks (see the
# header); each game session takes N blocks (default 1) and is one run
# directory, b_<first block>.
SEEDS=()
while [ $# -gt 0 ]; do
  case $1 in
    --blocks) BLOCKS=$2; shift 2 ;;
    --per-run) PER_RUN=$2; shift 2 ;;
    *) SEEDS+=("$1"); shift ;;
  esac
done
if [ -n "${BLOCKS:-}" ]; then
  [ "$BLOCKS" = all ] && BLOCKS=m21_00_00_00,m21_01_00_00,m22_00_00_00,m23_00_00_00,m24_00_00_00,m24_01_00_00,m24_02_00_00,m25_00_00_00,m26_00_00_00,m27_00_00_00,m28_00_00_00,m32_00_00_00,m33_00_00_00,m34_00_00_00,m35_00_00_00,m36_00_00_00
  IFS=, read -ra BL <<< "$BLOCKS"
  n=${PER_RUN:-1}
  for ((i = 0; i < ${#BL[@]}; i += n)); do
    SEEDS+=("blocks:$(IFS=,; echo "${BL[*]:i:n}")")
  done
fi
[ ${#SEEDS[@]} -eq 0 ] && SEEDS=(central host clinic)
OUT=${MAPVAL_OUT:-$REPO/tmp/mapval/$(date +%Y%m%d-%H%M%S)}
SRC=${MAPVAL_SERVER_SRC:?set MAPVAL_SERVER_SRC to the private server checkout}
PY=${MAPVAL_PYTHON:-$(cd "$SRC/../.." && pwd)/venv/bin/python}
DISP=${MAPVAL_DISPLAY:-:77}
HOLD=${MAPVAL_HOLD:-100}
START=${MAPVAL_START:-45}
BIN=${BBHOST_BIN:-$REPO/build/bbhost}
cfg() { sed -n "s|^$1 *= *\"\(.*\)\"|\1|p" "$MAIN/bbhost.toml" | head -1; }
APP0=${MAPVAL_APP0:-$(cfg app0)}
EBOOT=${MAPVAL_EBOOT:-$(cfg eboot)}
mkdir -p "$OUT"
T0=$(date +%s)
say() { echo "[$(( $(date +%s) - T0 ))s] $*" | tee -a "$OUT/timings.txt"; }
say "out $OUT, seeds ${SEEDS[*]}, server from $SRC"

[ -x "$BIN" ] || { echo "no $BIN (build bbhost first)"; exit 1; }
[ -d "$APP0/dvdroot_ps4" ] && [ -f "$EBOOT" ] || { echo "app0/eboot not found ($APP0, $EBOOT)"; exit 1; }
for p in 20443 18671; do
  if ss -ltn "sport = :$p" | grep -q LISTEN; then echo "port $p is taken: another server is running"; exit 1; fi
done

# --- the probe --------------------------------------------------------------
cc -shared -fPIC -O2 -Wall -I"$REPO/include" -o "$OUT/mapval.so" "$REPO/tools/mapval_plugin.c" -lm || exit 1

# --- the local server ---------------------------------------------------------
# A copy of the server code, not a symlink: its paths are relative to its own
# files (dirname(__file__)/../data), so a symlinked tree would write into the
# checkout it points at.
# A second call with the same MAPVAL_OUT (one seed per call, when a caller
# caps a command's time) keeps the server's copy, its accounts and history.db.
SRV=$OUT/server
if [ ! -f "$SRV/data/bloodborne.db" ]; then
  mkdir -p "$SRV/data" "$SRV/playlogs"
  rsync -a --exclude __pycache__ --exclude '*.db' --exclude '*.db-wal' --exclude '*.db-shm' "$SRC/server/" "$SRV/server/"
  for f in bloodborne.db bloodborne.db-wal bloodborne.db-shm website_secret; do
    [ -f "$SRC/data/$f" ] && cp "$SRC/data/$f" "$SRV/data/"
  done
fi
export BB_DASH_PASS=x BB_PUBLIC_HOST=127.0.0.1
export BB_PLAYLOG_EVENTS=RegularLog,ChrDead,ChrFallDead,ChrAttackDamage,ChangeMapInfo,EventLog,MeasureTime,PickUpItem
export BB_INGEST_PLAYLOG_DIR=$SRV/playlogs BB_INGEST_HISTORY_DB=$SRV/data/history.db BB_INGEST_GAME_DB=$SRV/data/bloodborne.db
(cd "$SRV" && exec "$PY" -m server.main --host 127.0.0.1 --no-ssl --stun --stun-bind 127.0.0.1) > "$SRV/server.log" 2>&1 &
SERVER=$!
(cd "$SRV" && exec "$PY" -m server.ingest) > "$SRV/ingest.log" 2>&1 &
INGEST=$!
DRIVE=
cleanup() {
  if [ -n "$DRIVE" ]; then
    kill -- -"$DRIVE" 2>/dev/null
    sleep 3
    kill -9 -- -"$DRIVE" 2>/dev/null   # the game ignores SIGTERM while online
    # menu_drive.sh's own `timeout` puts the game in another process group:
    # this run's game by its config path.
    for p in $(pgrep -x bbhost); do
      tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null | grep -qF "$OUT/" && kill -9 "$p" 2>/dev/null
    done
  fi
  kill $SERVER $INGEST 2>/dev/null
  wait $SERVER $INGEST 2>/dev/null
}
trap cleanup EXIT
trap 'exit 130' INT TERM
for _ in $(seq 1 60); do
  ss -ltn "sport = :20443" | grep -q LISTEN && ss -ltn "sport = :18671" | grep -q LISTEN && break
  sleep 1
done
ss -ltn "sport = :18671" | grep -q LISTEN || { echo "the server did not start: $SRV/server.log"; tail -20 "$SRV/server.log"; exit 1; }
say "server pid $SERVER, ingest pid $INGEST"

# --- the seeds ------------------------------------------------------------------
# name: data to copy, block, main-menu item (0 Continue, 1 Load Game slot 0),
# first enemy placement of the spread, kill step (radius [x y z [team]])
seed_info() {
  case $1 in
    central) echo "$MAIN/tmp/seed-data m24_01_00_00 0 0 60 -210.8 -39.7 205.6 23" ;;
    host)    echo "$MAIN/tmp/np-host-data m24_01_00_00 0 60 80 -191.6 -50.6 145.0 23" ;;
    clinic)  echo "$MAIN/tmp/seed-data m24_01_00_00 1 120 80 -216.7 -54.7 178.1 23" ;;
    *) return 1 ;;
  esac
}

for seed in "${SEEDS[@]}"; do
  unlock=0
  if [ "${seed#blocks:}" != "$seed" ]; then
    # A block tour: the frozen seed's character, every lamp lit, lamp travel.
    blist=${seed#blocks:}
    name=b_${blist%%,*}
    data=$MAIN/tmp/seed-data; block=$blist; menu=0; unlock=1
    D=$OUT/$name
  else
    info=$(seed_info "$seed") || { say "unknown seed $seed"; continue; }
    read -r data block menu estart kill <<< "$info"
    name=$seed
    D=$OUT/$seed
  fi
  rm -rf "$D"; mkdir -p "$D"
  cp -a "$data" "$D/data"
  mkdir -p "$D/data/plugins"; cp "$OUT/mapval.so" "$D/data/plugins/"
  if [ $unlock = 1 ]; then
    "$PY" "$REPO/tools/mapval_blocks.py" --app0 "$APP0" --blocks "$blist" --out "$D" --navmesh "${MAPVAL_NAVMESH:-20}" 2>&1 | grep -v Oodle | tee -a "$OUT/timings.txt"
    # A user per run: the dataset joins the server's rows to a run by it.
    online_id=MV$(echo "${blist%%,*}" | tr -d _ | cut -c2-7)
  else
    python3 "$REPO/tools/mapval_targets.py" --app0 "$APP0" --map "$block" --out "$D" --kill "$kill" --enemy-start "$estart" --enemies "${MAPVAL_ENEMIES:-6}" | tee -a "$OUT/timings.txt"
    online_id=MapVal$(echo "$seed" | tr a-z A-Z | cut -c1-4)
  fi
  cat > "$D/bbhost.toml" <<EOF
[paths]
app0 = "$APP0"
data = "$D/data"
tmp = "$D/tmp"
eboot = "$EBOOT"

[online]
host = "127.0.0.1"
scheme = "http"
verify_tls = false
online_id = "$online_id"
stun_server = "127.0.0.1:3478"
np_server = "http://127.0.0.1:18671"
auth_server = "http://127.0.0.1:18671"

[player]
name = "$online_id"

[video]
width = 1920
height = 1080
fps_cap = 30

[startup]
skip_intro = true
EOF
  cat > "$D/run.sh" <<EOF
#!/bin/bash
export SDL_AUDIO_DRIVER=dummy
exec "$BIN" --config "$D/bbhost.toml" "\$@"
EOF
  chmod +x "$D/run.sh"
  # The main menu: Continue, or Load Game and the first slot (the clinic
  # character is slot 0 of the frozen seed).
  if [ "$menu" = 1 ]; then
    load=(clicknow:1 click:0 click:0)
  else
    load=(clicknow:0)
  fi
  # The kill: the probe puts the player next to the enemy and says so; lock
  # on and attack until it reports the enemy dead (or its 60 s run out).
  steps=(title:online sleep:4 "${load[@]}" waitlog:area.glare.luminance sleep:10 shot:world
         'waitlog:mapval kill-ready' sleep:1.5 key:q sleep:0.5 lclick:6 shot:kill key:q sleep:0.4 key:q lclick:8
         key:q sleep:0.4 key:q lclick:8 key:q sleep:0.4 key:q lclick:8
         'waitlog:mapval (tour-done|fall-dead)' shot:fall 'waitlog:mapval tour-done' shot:end sleep:$HOLD)
  if [ $unlock = 1 ]; then
    # Per block: on the probe's walk-ready, a natural walk - forward, left,
    # back, right (one of them has room even against a wall) - and a shot;
    # the probe's walk step lasts 12 s.
    steps=(title:online sleep:4 "${load[@]}" waitlog:area.glare.luminance sleep:10 shot:world)
    IFS=, read -ra bl <<< "$blist"
    for b in "${bl[@]}"; do
      steps+=("waitlog:mapval (walk-ready walk_$b|travel-failed $b)" keyhold:w,3 keyhold:a,2.5 "shot:walk-$b" keyhold:s,3 keyhold:d,2.5)
    done
    steps+=('waitlog:mapval tour-done' shot:end sleep:$HOLD)
  fi
  say "run $name: $block from $data (menu item $menu), online as $online_id"
  # Its own session (process group), so an interrupted run takes the game
  # down with it instead of leaving it running.
  BBHOST_TEST_WARP=1 BBHOST_TEST_UNLOCK_LAMPS=$unlock BBHOST_MAPVAL_TOUR="$D/tour.txt" BBHOST_MAPVAL_START=$START \
  MENU_DRIVE_ONLINE_CONFIG="$D/bbhost.toml" DISPLAY_DRIVE=$DISP MENU_DRIVE_OUT="$D/out" \
  MENU_DRIVE_TIMEOUT=$(( 900 + HOLD )) BBHOST_BIN="$D/run.sh" \
    setsid timeout $(( 960 + HOLD )) "$REPO/tools/menu_drive.sh" "${steps[@]}" > "$D/drive.txt" 2>&1 &
  DRIVE=$!
  wait $DRIVE
  DRIVE=
  mv "$D/out/"* "$D/" 2>/dev/null; rmdir "$D/out" 2>/dev/null
  rm -rf "$D/data/bbhost" "$D/data/cache.bin" "$D/data/mods"   # large caches; the saves stay
  n=$(grep -c "mapval at " "$D/run.log" 2>/dev/null)
  say "run $name: done - $(grep -c 'travel-done' "$D/run.log") travels ok, $(grep -c 'travel-failed' "$D/run.log") failed, $n targets reached, $(grep -c 'mapval pos' "$D/run.log") position lines, $(grep -c 'dumped core\|Segmentation' "$D/run.log") crashes, $(find "$SRV/playlogs/incoming" "$SRV/playlogs/raw" -type f -name '*_*' 2>/dev/null | grep -vc '/\.tmp/') uploads so far"
done

# --- ingest, stop, dataset ---------------------------------------------------------
sleep 5
kill $INGEST 2>/dev/null; wait $INGEST 2>/dev/null
(cd "$SRV" && "$PY" -m server.ingest --once) >> "$SRV/ingest.log" 2>&1
cleanup; trap - EXIT
say "server stopped; ingested"
CORPUS=${MAPVAL_CORPUS:-}  # the world map's history.db, optional
# The dataset covers every seed this OUT holds (earlier calls' too).
ALL=(); for d in "$OUT"/*/run.log; do [ -f "$d" ] && ALL+=("$(basename "$(dirname "$d")")"); done
"$PY" "$REPO/tools/mapval_dataset.py" --out "$OUT" --server "$SRV" --seeds "${ALL[@]}" ${CORPUS:+--corpus "$CORPUS"} | tee "$OUT/summary.txt"
"$PY" "$REPO/tools/mapval_pixels.py" --dataset "$OUT/dataset.json" | tee -a "$OUT/summary.txt"
say "done: $OUT/summary.txt"
