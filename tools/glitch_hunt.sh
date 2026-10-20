#!/bin/bash
# Hunt the frames that flash or stretch, with nobody watching (host/glitch.cpp).
#
#   tools/glitch_hunt.sh [NAME] [VAR=value...]
#
# Plays the frozen seed (PERF_SOAK_DATA, default tmp/seed-data, on a scratch
# copy) with BBHOST_GLITCH=1 on the menu driver's Xvfb display: the title and
# main menu, the in-game menu opened and closed, then combat - light, strong
# and firearm attacks, transforms, dodges and camera turns - and the menu
# again. Every frame is compared with its neighbours and every draw's
# coverage with the same draw's either side; each catch is written under
# build/NAME/glitch/<flip>/ with the frames around it and the frame's draws.
#
# Afterwards it prints each catch and makes build/NAME/glitch/<flip>/strip.png
# (before | caught | after, half size) and build/NAME/glitch/sheet.png (every
# strip), which is what to look at first: the detector finds one-frame events,
# and a muzzle flash is one too - the report's draw spikes say which draw.
#
# NAME defaults to glitch-hunt. VAR=value pairs go into the game's
# environment (BBHOST_GLITCH_CELL=0.08, BBHOST_BUFFER_SHADOW=0, ...), so the
# same script A/Bs a suspected cause: compare the catch counts of two runs
# against the frames each one watched. GLITCH_ROUNDS (default 6) sets how
# many menu-and-combat rounds; each is about 50 s.
set -u
export BBHOST_SETUP_WINDOW=0  # a harness run never stops at the setup window
cd "$(dirname "$0")/.."
NAME=${1:-glitch-hunt}
[ $# -gt 0 ] && shift
[ -f bbhost.toml ] || { echo "no bbhost.toml (see bbhost.example.toml)"; exit 1; }
mkdir -p tmp
rm -rf tmp/data-scratch
data=$(sed -n 's/^data *= *"\(.*\)"/\1/p' bbhost.toml | head -1)
seed=${PERF_SOAK_DATA:-tmp/seed-data}
[ -d "$seed" ] || seed=${data:-data}
cp -a "$seed" tmp/data-scratch
sed -e "s|^data *=.*|data = \"$PWD/tmp/data-scratch\"|" -e "s|^mods *=.*|mods = \"$PWD/tmp/data-scratch/mods\"|" bbhost.toml > tmp/glitch-hunt.toml
cat > tmp/glitch-hunt-run.sh <<EOF
#!/bin/bash
if [ -n "\${BB_OPT:-}" ] && [ -n "\${BBHOST_OPTIONS_PATH:-}" ]; then
  sed -i "/^frame_cap = /d" "\$BBHOST_OPTIONS_PATH"; sed -i "/^\[options\]/a \$BB_OPT" "\$BBHOST_OPTIONS_PATH"
fi
export SDL_AUDIO_DRIVER=\${SDL_AUDIO_DRIVER:-dummy}
exec "\${BBHOST_EXE:-$PWD/build/bbhost}" --config "$PWD/tmp/glitch-hunt.toml" "\$@"
EOF
chmod +x tmp/glitch-hunt-run.sh

rounds=${GLITCH_ROUNDS:-6}
steps=(title sleep:4 clicknow:0 waitlog:area.glare.luminance sleep:6)
for _ in $(seq 1 "$rounds"); do
  # The in-game menu, opened and closed.
  for _ in 1 2 3 4; do steps+=(key:Escape sleep:2 key:Escape sleep:2); done
  # Combat: into whatever is ahead, every kind of attack, turning.
  steps+=(keys:w+l,3 lclick:4 key:v sleep:1.5 key:x sleep:1 keys:a+space,1 lclick:3 key:c sleep:1.5
          pan:18,2 keys:w+space,3 lclick:4 key:x sleep:1 key:v sleep:1.5 keys:s+space,2 key:q sleep:0.5
          lclick:5 key:x sleep:1 key:c sleep:1 pan:-18,2 keys:d+w,2 lclick:3 key:q sleep:0.5)
done
timeout_s=$(( 150 + rounds * 55 ))
out=build/$NAME
env "$@" BBHOST_GLITCH=1 BBHOST_GLITCH_DIR="$out/glitch" BBHOST_GAME_FPS=60 BB_OPT='frame_cap = "60"' \
    MENU_DRIVE_OUT="$out" MENU_DRIVE_TIMEOUT=$timeout_s BBHOST_BIN="$PWD/tmp/glitch-hunt-run.sh" \
    timeout $(( timeout_s + 60 )) tools/menu_drive.sh "${steps[@]}" > /dev/null 2>&1

log=$out/run.log
[ -f "$log" ] || { echo "no run log"; exit 1; }
grep -q 'area.glare.luminance' "$log" || echo "note: the run never reached the world (menus only)"
grep -E 'dumped core|Segmentation' "$log" | head -3
echo "== catches"
grep -E '^\[bbhost\] glitch: (flip|  spike)' "$log" | sed 's/^\[bbhost\] glitch: //' | head -120
echo "== summary"
grep -E '^\[bbhost\] glitch: [0-9]+ frames watched' "$log" | sed 's/^\[bbhost\] //'
# The kinds worth looking at first, from every catch (catches.csv): most
# one-frame events are fast effects (sparks) and are not glitches.
[ -f "$out/glitch/catches.csv" ] && python3 - "$out/glitch/catches.csv" <<'PY'
import csv, sys, collections
rows = list(csv.DictReader(open(sys.argv[1])))
f = lambda r, k: float(r[k])
def flips(rs): return ' '.join(r['flip'] + ('*' if r['written'] == '1' else '') for r in rs[:12])
black = [r for r in rows if f(r, 'mean') < 0.01 and min(f(r, 'mean_before'), f(r, 'mean_after')) > 0.03]
bright = [r for r in rows if f(r, 'mean') - max(f(r, 'mean_before'), f(r, 'mean_after')) > 0.05]
big = [r for r in rows if f(r, 'percent') >= 10]
screen = [r for r in rows if f(r, 'spike_viewport_percent') >= 50]
visible = [r for r in screen if f(r, 'percent') >= 1]
print("== triage (* written to glitch/<flip>/)")
print(f"black frames: {len(black)}  {flips(black)}")
print(f"brighter by > 0.05 for one frame: {len(bright)}  {flips(bright)}")
print(f"one-frame changes over 10% of the frame: {len(big)}  {flips(big)}")
print(f"draws covering over half their viewport for one frame: {len(screen)}, {len(visible)} of them visible in the picture")
by = collections.Counter(r['spike_pipeline'] for r in visible)
for p, n in by.most_common(6): print(f"  {p}: {n}")
PY
# Strips to look at: before | caught | after.
if [ -d "$out/glitch" ]; then
  strips=()
  for d in "$out"/glitch/*/; do
    b=$(ls "$d"*-before.ppm 2>/dev/null | head -1); c=$(ls "$d"*-caught.ppm 2>/dev/null | head -1)
    c2=$(ls "$d"*-caught2.ppm 2>/dev/null | head -1); a=$(ls "$d"*-after.ppm 2>/dev/null | head -1)
    [ -n "$b" ] && [ -n "$c" ] && [ -n "$a" ] || continue
    convert "$b" "$c" ${c2:+"$c2"} "$a" -resize 50% +append "${d}strip.png" 2>/dev/null && strips+=("${d}strip.png")
  done
  if [ ${#strips[@]} -gt 0 ]; then
    montage "${strips[@]:0:24}" -tile 1x -geometry +0+6 -resize 1440x "$out/glitch/sheet.png" 2>/dev/null
    echo "== ${#strips[@]} catches written: $out/glitch/<flip>/strip.png, all of them in $out/glitch/sheet.png"
  fi
fi
