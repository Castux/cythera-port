#!/bin/sh
# Extract the game from the original StuffIt installer into gamedata/.
# Requires: unar (brew install unar), python3 >= 3.10.
# Usage: tools/setup_gamedata.sh [path/to/Cythera_Installer.sit] [outdir]
set -e
SIT="${1:-Cythera_Installer.sit}"
OUT="${2:-gamedata}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

command -v unar >/dev/null || { echo "unar not found (brew install unar)"; exit 1; }
unar -q -k visible -o "$TMP/sit" "$SIT"

python3 -m venv "$TMP/venv"
"$TMP/venv/bin/pip" -q install "git+https://github.com/mrmidi/installer-vise"
"$TMP/venv/bin/installer-vise" extract "$TMP/sit/Cythera Installer" -o "$TMP/vise" >/dev/null

mkdir -p "$OUT"
cp -R "$TMP/vise/files/." "$OUT/"
cp "$TMP/vise/manifest.csv" "$OUT/.manifest.csv"  # type/creator info
echo "Game files extracted to $OUT/"
