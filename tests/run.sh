#!/bin/sh
# Run the scripted scenarios deterministically and report failures
# (crashes, unimplemented Toolbox calls, stalls). Screenshots go to work/shots.
cd "$(dirname "$0")/.."
mkdir -p work/shots
status=0
for s in ${@:-tests/scripts/*.txt}; do
  log="work/shots/$(basename "$s" .txt).log"
  # optional first line "# app: NAME" selects another application
  app=$(sed -n '1s/^# app: //p' "$s")
  ./build/cythera --deterministic --timeout 900 ${app:+--app "$app"} --script "$s" > "$log" 2>&1
  rc=$?
  bad=$(grep -E "FATAL|unimplemented trap|stall:" "$log" | head -3)
  if [ $rc -ne 0 ] || [ -n "$bad" ]; then
    echo "FAIL $s (exit $rc)"; echo "$bad"; status=1
  else
    echo "ok   $s"
  fi
done
exit $status
