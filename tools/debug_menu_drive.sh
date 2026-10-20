#!/bin/bash
# The world on a scratch copy of the frozen seed, then menu_drive steps - for
# walking the debug menu.
#
#   tools/debug_menu_drive.sh NAME [VAR=value...] -- steps...
#
# The steps run once the world is up. `key:grave` opens the menu; Return goes
# in, BackSpace back, Escape (Options, START in the menu's guide) opens its
# sub-window. The seed's character stands by enemies that kill it about 15 s
# in: the menu stays up through the death, and after `sleep:50` it has
# respawned at the lamp, where nothing reaches it. Output: build/NAME/ (run.log
# and the shots) and build/NAME.drive.txt. RUN_SECS caps the run (300);
# SRC_DATA picks the data to copy (tmp/seed-data), never the player's data/.
# Steps are words, so in zsh expand a string of them with ${=steps}.
set -u
export BBHOST_SETUP_WINDOW=0  # a harness run never stops at the setup window
cd "$(dirname "$0")/.."
NAME=$1; shift
envs=()
while [ $# -gt 0 ] && [ "$1" != "--" ]; do envs+=("$1"); shift; done
[ "${1:-}" = "--" ] && shift
rm -rf tmp/data-scratch
cp -a "${SRC_DATA:-tmp/seed-data}" tmp/data-scratch
sed -e "s|^data *=.*|data = \"$PWD/tmp/data-scratch\"|" -e "s|^mods *=.*|mods = \"$PWD/tmp/data-scratch/mods\"|" bbhost.toml > tmp/debug-menu-drive.toml
cat > tmp/debug-menu-drive-run.sh <<EOS
#!/bin/bash
export SDL_AUDIO_DRIVER=\${SDL_AUDIO_DRIVER:-dummy}
exec "$PWD/build/bbhost" --config "$PWD/tmp/debug-menu-drive.toml" "\$@"
EOS
chmod +x tmp/debug-menu-drive-run.sh
out=build/$NAME
rm -rf "$out"
env "${envs[@]}" MENU_DRIVE_OUT="$out" MENU_DRIVE_TIMEOUT="${RUN_SECS:-300}" BBHOST_BIN="$PWD/tmp/debug-menu-drive-run.sh" \
  timeout $(( ${RUN_SECS:-300} + 60 )) tools/menu_drive.sh title sleep:4 clicknow:0 waitlog:area.glare.luminance sleep:6 "$@" > "$out.drive.txt" 2>&1
log=$out/run.log
grep -q 'area.glare.luminance' "$log" && echo "reached the world" || echo "did not reach the world"
grep -E "dumped core|Segmentation" "$log" | head -3
ls "$out"/*.png 2>/dev/null | wc -l
