#!/bin/sh
# Extract the game from the original installer into gamedata/.
# The installer (Cythera Installer, an Installer VISE application) comes in
# orig/ both as MacBinary (Cythera.bin) and as a StuffIt archive
# (Cythera_Installer.sit); the installers inside are identical.
# Requires: python3 >= 3.10 (PYTHON to override), and unar for .sit only.
# Usage: tools/setup_gamedata.sh [orig/Cythera.bin | orig/Cythera_Installer.sit] [outdir]
set -e
IN="${1:-orig/Cythera.bin}"
OUT="${2:-gamedata}"
PY="${PYTHON:-python3}"
export PYTHONUTF8=1  # installer-vise prints Unicode progress (fails on Windows code pages)
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/in"
case "$IN" in
*.sit)
  command -v unar >/dev/null || { echo "unar not found (brew install unar / apt install unar)"; exit 1; }
  unar -q -k visible -o "$TMP/in" "$IN" ;;
*)
  # MacBinary: 128-byte header, then the data and resource forks, each padded to 128 bytes
  "$PY" - "$IN" "$TMP/in" <<'EOF'
import struct, sys
d = open(sys.argv[1], 'rb').read()
if len(d) < 128 or d[0] != 0 or d[74] != 0 or not 1 <= d[1] <= 63:
    sys.exit(sys.argv[1] + ': not a MacBinary file')
name = d[2:2 + d[1]].decode('mac_roman')
dlen, rlen = struct.unpack('>II', d[83:91])
roff = 128 + (dlen + 127) // 128 * 128
open(sys.argv[2] + '/' + name, 'wb').write(d[128:128 + dlen])
open(sys.argv[2] + '/' + name + '.rsrc', 'wb').write(d[roff:roff + rlen])
EOF
  ;;
esac
[ -f "$TMP/in/Cythera Installer" ] || { echo "$IN: no Cythera Installer inside"; exit 1; }

"$PY" -m venv "$TMP/venv"
BIN="$TMP/venv/bin"; [ -d "$BIN" ] || BIN="$TMP/venv/Scripts"  # Windows Python
"$BIN/pip" -q install "git+https://github.com/mrmidi/installer-vise"
"$BIN/installer-vise" extract "$TMP/in/Cythera Installer" -o "$TMP/vise" >/dev/null

mkdir -p "$OUT"
cp -R "$TMP/vise/files/." "$OUT/"
cp "$TMP/vise/manifest.csv" "$OUT/.manifest.csv"  # type/creator info
# General MIDI SoundFont for the QuickTime music (GeneralUser GS, free license)
if [ ! -f "$OUT/soundfont.sf2" ]; then
  echo "Downloading GeneralUser GS SoundFont (32 MB)..."
  curl -fsSL -o "$OUT/soundfont.sf2" \
    "https://github.com/mrbumpy409/GeneralUser-GS/raw/main/GeneralUser-GS.sf2" || echo "SoundFont download failed; music will use the built-in synth"
fi
echo "Game files extracted to $OUT/"
