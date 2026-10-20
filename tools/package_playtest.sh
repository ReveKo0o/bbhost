#!/bin/bash
# The tester packages for a group playtest on the dev server, built by hand
# and never part of a release (tools/check_release_bundle.sh fails a release
# that carries one; releases go to the live server). The Windows zip
# (tools/win_package.sh's) and the Linux tarball (tools/package_linux.sh's),
# each with the playtest config, TESTING.md and a launcher that writes a log
# file, from tools/playtest/. Players' own game files never go in: they bring
# the dump and the decrypted 1.09 eboot.
#   tools/package_playtest.sh WIN_ZIP LINUX_TARBALL OUTDIR     (LINUX_TARBALL "-": Windows only)
# The general package's README is left out, so TESTING.md is the one set of
# instructions; the game's paths live in the per-user config, the playtest
# toml adds only the dev server.
set -eu
cd "$(dirname "$0")/.."
win_zip=$(realpath "$1"); lin_tgz=$2; [ "$lin_tgz" = - ] || lin_tgz=$(realpath "$2"); out=$(realpath -m "$3")
pt=$PWD/tools/playtest
mkdir -p "$out"
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

# Windows.
(cd "$work" && unzip -q "$win_zip")
wdir=$(ls -d "$work"/bbhost-win-* | head -1)
wname=$(basename "$wdir" | sed 's/^bbhost-win-/bbhost-playtest-/')-windows
rm -f "$wdir/README.txt"
mkdir -p "$wdir/logs"
# CRLF for Notepad and cmd.
sed 's/$/\r/' "$pt/bbhost-playtest.toml" > "$wdir/bbhost-playtest.toml"
sed 's/$/\r/' "$pt/TESTING.md" > "$wdir/TESTING.md"
cp "$pt/run-bbhost.bat" "$wdir/run-bbhost.bat"
mv "$wdir" "$work/$wname"
(cd "$work" && rm -f "$out/$wname.zip" && zip -qr "$out/$wname.zip" "$wname")

if [ "$lin_tgz" = - ]; then
    ls -la "$out/$wname.zip"
    exit 0
fi

# Linux.
(cd "$work" && tar xzf "$lin_tgz")
ldir=$(ls -d "$work"/bbhost-linux-* | head -1)
lname=$(basename "$ldir" | sed 's/^bbhost-linux-/bbhost-playtest-/')-linux-x86_64
cp "$pt/bbhost-playtest.toml" "$pt/TESTING.md" "$ldir/"
rm -f "$ldir/bbhost.example.toml"
sed -i 's/^cfg=\${BBHOST_CONFIG:-bbhost.toml}/cfg=${BBHOST_CONFIG:-bbhost-playtest.toml}/' "$ldir/run-bbhost.sh"
grep -q 'bbhost-playtest.toml' "$ldir/run-bbhost.sh" || { echo "run-bbhost.sh: config line not found"; exit 1; }
# The README's setup is for the general package; TESTING.md has the playtest's.
sed -i 's/^  bbhost.example.toml lists every setting./  bbhost.example.toml lists every setting; TESTING.md has the playtest steps./' "$ldir/README-linux.txt"
find "$ldir" -type d -exec chmod 755 {} + && find "$ldir" -type f -exec chmod 644 {} +
chmod 755 "$ldir/bbhost" "$ldir/run-bbhost.sh"
mv "$ldir" "$work/$lname"
(cd "$work" && rm -f "$out/$lname.tar.gz" && tar czf "$out/$lname.tar.gz" --owner=0 --group=0 "$lname")

ls -la "$out/$wname.zip" "$out/$lname.tar.gz"
