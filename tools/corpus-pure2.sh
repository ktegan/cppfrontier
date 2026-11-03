#!/bin/bash
# Translate, compile, and run each upstream pure2-*.cpp2 regression test, and
# compare its output with upstream's gcc-14 .execution results.
# usage: [BIN=...] [UPSTREAM=...] [OUT=...] [JOBS=...] tools/corpus-pure2.sh
# Writes the first error of each failing file to $OUT/tfail.txt (translation)
# and $OUT/cfail.txt (compilation), and lists files whose output differs from
# upstream's in $OUT/wrong.txt. Files without an upstream .execution result
# only need to compile; a program killed by a signal or the timeout is wrong. A
# *-error test must be rejected with status 1; one that translates is listed in
# $OUT/accepted.txt, and one that crashes in $OUT/tfail.txt. Exits with status 1
# on any failure other than the files named in $ALLOW (space-separated). Files
# named in $REWRITTEN are rewritten first where the rules differ
# (tools/upstream-rewrites.pl). A result in tests/corpus/ replaces upstream's where
# the output deliberately differs (see docs/UPSTREAM_AUDIT.md).
source "$(dirname "$0")/common.sh"
OUT=${OUT:-$REPO/build/corpus/pure2}; mkdir -p "$OUT"; OUT=$(realpath "$OUT"); cd "$OUT" || exit 1
export BIN CXX UPSTREAM RUNTIME REPO REWRITTEN R E=$R/test-results/$RESULTS
rm -f ./*.result ./*.execution

# Each file is checked in its own directory. As in upstream's harness, it is
# translated from the test directory and the program is named test.exe, since
# both appear in some tests' output.
check() {
  n=$(basename "$1" .cpp2); dir=$PWD/$n; mkdir -p "$dir"
  src=$R
  if [[ " ${REWRITTEN:-} " == *" $n "* ]]; then
    perl "$REPO/tools/upstream-rewrites.pl" "$1" > "$dir/$n.cpp2"; src=$dir
  fi
  (cd "$src" && timeout 20 "$BIN" "$n.cpp2" -o "$dir/$n.cpp") > "$dir/tr" 2>&1; status=$?
  if [ $status -ne 0 ]; then
    # A crash or a timeout is not a rejection.
    if [[ $n == *-error ]] && [ $status -eq 1 ]; then echo "rejected $n" > "$n.result"
    elif [ $status -ge 124 ]; then echo "tfail $n: crashed or timed out (status $status)" > "$n.result"
    else echo "tfail $n: $(grep -m1 ": error:" "$dir/tr")" > "$n.result"; fi
    rm -rf "$dir"; return
  fi
  if [[ $n == *-error ]]; then echo "accepted $n" > "$n.result"; rm -rf "$dir"; return; fi
  cd "$dir" || return
  if ! timeout 120 $CXX -std=c++23 -I"$RUNTIME" -DCPP2_INCLUDE_STD=Yes -pthread \
       -x c++ "$n.cpp" -o test.exe 2> cc; then
    echo "cfail $n: $(grep -m1 'error' cc)" > "../$n.result"; cd ..; rm -rf "$dir"; return
  fi
  timeout 10 ./test.exe > execution 2>&1; status=$?
  expected=$E/$n.cpp.execution
  [ -f "$REPO/tests/corpus/$n.cpp.execution" ] && expected=$REPO/tests/corpus/$n.cpp.execution
  # A program killed by a signal or by the timeout is wrong whatever it printed.
  if [ $status -ge 124 ] ||
     { [ -f "$expected" ] && ! diff -q <(sed -E 's/auto:[0-9]+/auto:N/g' "$expected") \
       <(sed -E 's/auto:[0-9]+/auto:N/g' execution) > /dev/null; }; then
    echo "exit status $status" >> execution
    echo "wrong $n" > "../$n.result"; cp execution "../$n.execution"
  else
    echo "ok $n" > "../$n.result"
  fi
  cd ..; rm -rf "$dir"
}
export -f check
ls "$R"/pure2-*.cpp2 | xargs -P "${JOBS:-$(nproc)}" -I{} bash -c 'check {}'
for kind in tfail cfail wrong accepted; do
  grep -h "^$kind " ./*.result | sed "s/^$kind //" | sort > "$kind.txt"
done
echo "ok=$(cat ./*.result | grep -c '^ok ') tfail=$(wc -l < tfail.txt) cfail=$(wc -l < cfail.txt)" \
  "wrong=$(wc -l < wrong.txt) (error tests: $(cat ./*.result | grep -c '^rejected ') rejected," \
  "$(wc -l < accepted.txt) accepted)"
failures=$(cat tfail.txt cfail.txt wrong.txt accepted.txt | cut -d: -f1 | sed 's/ .*//' |
  grep -vxF -f <(tr ' ' '\n' <<< "${ALLOW:-}" | grep .))
[ -z "$failures" ] || { echo "failing:" $failures; exit 1; }
