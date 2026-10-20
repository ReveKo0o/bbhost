#!/bin/bash
# Put the debug menu's resources into the port's asset overlay.
#
# The developers' debug menu (engine/debug_menu.h, the Debug Menu plugin)
# draws with a font the retail disc does not ship: adhoc:/font/DbgFont14h.ccm
# and .tpf. The plugin makes its own from public-domain fonts; this puts the
# original instead - from the community's "Debug Menu Restoration" package,
# with the debug-font shaders in adhoc:/FontShader, which the patch skips (they
# are copied anyway) - into the mods folder, where it wins over the plugin's.
# The dump is never written.
#
#   tools/build_debug_assets.sh ["tmp/Debug Menu Restoration.7z"] [OUT]
#   (OUT: the adhoc directory to fill, default data/mods/dvdroot_ps4/adhoc)
set -eu
cd "$(dirname "$0")/.."
ARCHIVE=${1:-"tmp/Debug Menu Restoration.7z"}
[ -f "$ARCHIVE" ] || { echo "no archive at $ARCHIVE" >&2; exit 1; }
OUT=${2:-data/mods/dvdroot_ps4/adhoc}
WORK=tmp/debug-menu-assets.$$
rm -rf "$WORK"
mkdir -p "$WORK" "$OUT/font" "$OUT/FontShader"
TOP="Debug Menu Restoration/adhoc"
7z x -y -o"$WORK" "$ARCHIVE" \
    "$TOP/font/DbgFont14h.ccm" "$TOP/font/DbgFont14h.tpf" \
    "$TOP/FontShader/debugFont_vs.vpo" "$TOP/FontShader/debugFont_ps.ppo" > /dev/null
cp "$WORK/$TOP/font/DbgFont14h.ccm" "$WORK/$TOP/font/DbgFont14h.tpf" "$OUT/font/"
cp "$WORK/$TOP/FontShader/debugFont_vs.vpo" "$WORK/$TOP/FontShader/debugFont_ps.ppo" "$OUT/FontShader/"
rm -rf "$WORK"
for f in font/DbgFont14h.ccm font/DbgFont14h.tpf FontShader/debugFont_vs.vpo FontShader/debugFont_ps.ppo; do
    printf '  %-32s %8d bytes\n' "$f" "$(stat -c %s "$OUT/$f")"
done
echo "debug menu assets -> $OUT"
