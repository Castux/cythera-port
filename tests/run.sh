#!/bin/sh
# Run the scripted scenarios deterministically and report failures
# (crashes, unimplemented Toolbox calls, stalls). Screenshots go to work/shots.
#
# Runs are hermetic: each script gets a fresh copy of a temporary System
# Folder and Saved Games (never ~/.cythera-port). The copy starts from a
# fixture made by tests/fixture.txt, which creates the saved game "Hero"
# (a new game saved after the first conversation with the king).
# Extra emulator options can be passed in $CYTHERA_ARGS (e.g. "--trap-stats FILE").
cd "$(dirname "$0")/.."
mkdir -p work/shots
tmp=$(mktemp -d "${TMPDIR:-/tmp}/cythera-tests.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/fixture/System Folder"

run() { # run SCRIPT HOME LOG
  # optional first line "# app: NAME" selects another application
  app=$(sed -n '1s/^# app: //p' "$1")
  "${CYTHERA:-./build/cythera}" --deterministic --timeout 900 --sysdir "$2/System Folder" \
    ${app:+--app "$app"} $CYTHERA_ARGS --script "$1" > "$3" 2>&1
}
check() { # check NAME RC LOG
  bad=$(grep -E "FATAL|unimplemented trap|stall:|EXPECT FAILED" "$3" | head -3)
  if [ "$2" -ne 0 ] || [ -n "$bad" ]; then
    echo "FAIL $1 (exit $2)"; [ -n "$bad" ] && echo "$bad"; return 1
  fi
  echo "ok   $1"
}

status=0
run tests/fixture.txt "$tmp/fixture" work/shots/fixture.log
check tests/fixture.txt $? work/shots/fixture.log || exit 1
[ -f "$tmp/fixture/Saved Games/Hero" ] || { echo "FAIL fixture: no saved game"; exit 1; }

for s in ${@:-tests/scripts/*.txt}; do
  log="work/shots/$(basename "$s" .txt).log"
  rm -rf "$tmp/run" && cp -R "$tmp/fixture" "$tmp/run"
  run "$s" "$tmp/run" "$log"
  check "$s" $? "$log" || status=1
done
exit $status
