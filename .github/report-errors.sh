#!/bin/sh
# After a failed CI job: repeat the build log's error lines and the scripted
# tests' failures as annotations, which are visible without log access.
# Real errors first, so that warnings can't crowd them out.
{
  if [ -f build.log ]; then
    grep -E ': error|error:|\*\*\*|undefined reference|Undefined symbols|ld: |^FAIL' build.log | head -25
    grep -iE 'warning|not found|denied' build.log | grep -v 'pkg-config\|Package .*sdl2' | head -10
  fi
  ls work/shots/*.log >/dev/null 2>&1 && grep -hE 'FATAL|EXPECT FAILED|stall:|unimplemented trap' work/shots/*.log | head -15
} | head -40 | sed 's/^/::error::/'
exit 0
