#!/bin/sh
# Run the scripted scenarios deterministically and report failures
# (crashes, unimplemented Toolbox calls, stalls). Screenshots go to work/shots.
#
# Runs are hermetic: each script gets a fresh copy of a temporary System
# Folder and Saved Games (never ~/.cythera-port). The copy starts from a
# fixture made by tests/fixture.txt, which creates the saved game "Hero"
# (a new game saved after the first conversation with the king). The game
# folder is copied too, without the files the game creates in it ("User
# Custom Data", Finder info), so earlier runs don't change later ones; the
# SoundFont, which only the port reads, stays where it is.
# Extra emulator options can be passed in $CYTHERA_ARGS (e.g. "--trap-stats FILE").
#
# Golden traces: each run is also compared with tests/golden/NAME.txt, a line
# per script command with hashes of the Toolbox calls made so far, their
# arguments and results, and the screen (src/golden.c). Any difference fails,
# naming the first script line where the run diverged. GOLDEN=update records
# them instead (after a deliberate change of behaviour); GOLDEN=off skips them.
cd "$(dirname "$0")/.."
# a Python 3 that runs (on Windows, python3 may be the Microsoft Store stub)
PY="${PYTHON:-}"
[ -n "$PY" ] || { python3 -c '' 2>/dev/null && PY=python3 || PY=python; }
mkdir -p work/shots
tmp=$(mktemp -d "${TMPDIR:-/tmp}/cythera-tests.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/fixture/System Folder" "$tmp/data/gamedata"
for f in gamedata/* gamedata/.manifest.csv; do
  case "$(basename "$f")" in "User Custom Data"|soundfont.sf2) continue ;; esac
  [ -e "$f" ] && cp -R "$f" "$tmp/data/gamedata/"
done
SF=; [ -f gamedata/soundfont.sf2 ] && SF="--soundfont gamedata/soundfont.sf2"

run() { # run SCRIPT HOME LOG
  # optional first line "# app: NAME" selects another application;
  # "# args: ..." adds emulator options
  app=$(sed -n '1s/^# app: //p' "$1")
  args=$(sed -n 's/^# args: //p' "$1")
  rm -rf "$tmp/gamedata" && cp -R "$tmp/data/gamedata" "$tmp/gamedata"
  "${CYTHERA:-./build/cythera}" --deterministic --timeout 900 --sysdir "$2/System Folder" \
    --data "$tmp/gamedata" $SF ${app:+--app "$app"} $args --golden "${3%.log}.golden" $CYTHERA_ARGS \
    --script "$1" > "$3" 2>&1
}
golden() { # golden NAME LOG: compare with (or record) tests/golden/NAME.txt
  want="tests/golden/$(basename "$1" .txt).txt" got="${2%.log}.golden"
  case "${GOLDEN:-check}" in
  off) return 0 ;;
  update) mkdir -p tests/golden; cp "$got" "$want"; return 0 ;;
  esac
  [ -f "$want" ] || { echo "FAIL $1 (no $want: GOLDEN=update records it)"; return 1; }
  cmp -s "$want" "$got" && return 0
  echo "FAIL $1 (golden trace differs; first difference:)"
  cp "$want" "${2%.log}.want" # next to the run's .golden, for the CI artifacts
  awk 'NR == FNR { w[FNR] = $0; n = FNR; next }
       w[FNR] != $0 { print "  want: " w[FNR]; print "  got:  " $0; d = 1; exit }
       END { if (!d && n != FNR) print "  (one trace is a prefix of the other: " n " vs " FNR " lines)"
             else if (!d) print "  (the same lines: line ends or trailing bytes differ)" }' "$want" "$got"
  return 1
}
check() { # check NAME RC LOG
  bad=$(grep -E "FATAL|unimplemented trap|stall:|EXPECT FAILED" "$3" | head -3)
  if [ "$2" -ne 0 ] || [ -n "$bad" ]; then
    echo "FAIL $1 (exit $2)"; [ -n "$bad" ] && echo "$bad"; return 1
  fi
  golden "$1" "$3" || return 1
  echo "ok   $1"
}

status=0
run tests/fixture.txt "$tmp/fixture" work/shots/fixture.log
check tests/fixture.txt $? work/shots/fixture.log || exit 1
[ -f "$tmp/fixture/Saved Games/Hero" ] || { echo "FAIL fixture: no saved game"; exit 1; }

for s in ${@:-tests/scripts/*.txt}; do
  log="work/shots/$(basename "$s" .txt).log"
  rm -rf "$tmp/run" && cp -R "$tmp/fixture" "$tmp/run"
  # "# save: NAME" starts from the stage save tests/saves/NAME instead of the fixture's
  stage=$(sed -n 's/^# save: //p' "$s")
  if [ -n "$stage" ]; then
    [ -f "tests/saves/$stage" ] || { echo "FAIL $s (no tests/saves/$stage)"; status=1; continue; }
    cp "tests/saves/$stage" "$tmp/run/Saved Games/Hero"
    cp "tests/saves/$stage.rsrc" "$tmp/run/Saved Games/Hero.rsrc"
  fi
  # "# save-edit: KEY=VALUE..." edits the fixture's saved game first (tools/delv_save.py set)
  edit=$(sed -n 's/^# save-edit: //p' "$s")
  if [ -n "$edit" ] && ! $PY tools/delv_save.py set "$tmp/run/Saved Games/Hero" $edit > "$log" 2>&1; then
    echo "FAIL $s (save-edit)"; tail -3 "$log"; status=1; continue
  fi
  run "$s" "$tmp/run" "$log"
  check "$s" $? "$log" || status=1
done
exit $status
