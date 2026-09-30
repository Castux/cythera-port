#!/bin/sh
# After a failed CI job: repeat the build log's error lines and the scripted
# tests' failures as annotations, which are visible without log access.
{
  [ -f build.log ] && grep -iE 'error|undefined|cannot find|not found|denied|FAIL' build.log
  ls work/shots/*.log >/dev/null 2>&1 && grep -hE 'FATAL|EXPECT FAILED|stall:|unimplemented trap' work/shots/*.log
} | head -40 | sed 's/^/::error::/'
exit 0
