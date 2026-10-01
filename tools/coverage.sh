#!/bin/sh
# Coverage of the scripted tests (clang/LLVM): builds build-cov/, runs
# tests/run.sh, and writes to work/cov/:
#   report.txt        line and function coverage per source file
#   uncalled.txt      Toolbox calls the applications import but no test calls
#   unimported.txt    TRAP definitions neither application imports (the
#                     port may still call some natively: check before removing)
#   html/             annotated sources (open html/index.html)
# Usage: tools/coverage.sh [SCRIPT...]   (default: all of tests/scripts)
set -e
cd "$(dirname "$0")/.."
OUT=work/cov
rm -rf "$OUT" && mkdir -p "$OUT"
PROFDATA=$(command -v llvm-profdata || xcrun --find llvm-profdata)
COV=$(command -v llvm-cov || xcrun --find llvm-cov)
make -j4 BUILD=build-cov CC="${CC:-clang}" CFLAGS="-O1 -g -fprofile-instr-generate -fcoverage-mapping" \
  LDFLAGS="-fprofile-instr-generate" > "$OUT/build.log"
LLVM_PROFILE_FILE="$PWD/$OUT/%p.profraw" CYTHERA=build-cov/cythera \
  CYTHERA_ARGS="--trap-stats $PWD/$OUT/traps.txt" tests/run.sh "$@" | tee "$OUT/suite.txt" || true
"$PROFDATA" merge -sparse "$OUT"/*.profraw -o "$OUT/all.profdata"
rm -f "$OUT"/*.profraw
IGNORE='third_party|build-cov/gen'
"$COV" report build-cov/cythera -instr-profile="$OUT/all.profdata" -ignore-filename-regex="$IGNORE" > "$OUT/report.txt"
"$COV" show build-cov/cythera -instr-profile="$OUT/all.profdata" -ignore-filename-regex="$IGNORE" \
  -format=html -output-dir="$OUT/html"
# --trap-stats lines: NAME CALLS IMPLEMENTED CALLER, appended by every run
awk '{ c[$1] += $2; f[$1] = $3 } END { for (n in c) if (!c[n] && f[n]) print n }' "$OUT/traps.txt" | sort > "$OUT/uncalled.txt"
grep -ho '^TRAP([A-Za-z0-9_]*)' src/os/*.c | sed 's/^TRAP(//; s/)$//' | sort -u > "$OUT/defined.txt"
awk '{ print $1 }' "$OUT/traps.txt" | sort -u > "$OUT/imported.txt"
comm -23 "$OUT/defined.txt" "$OUT/imported.txt" > "$OUT/unimported.txt"
grep TOTAL "$OUT/report.txt"
echo "$(wc -l < "$OUT/uncalled.txt") imported calls never made, $(wc -l < "$OUT/unimported.txt") TRAPs never imported (work/cov/)"
