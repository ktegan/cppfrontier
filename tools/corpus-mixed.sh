#!/bin/bash
# Translate, compile, and run each upstream mixed-*.cpp2 regression test, and
# compare its output with upstream's gcc-14 .execution results. A result in
# tests/corpus/ replaces upstream's where the output deliberately differs.
# usage: [BIN=...] [UPSTREAM=...] [OUT=...] tools/corpus-mixed.sh
# Exits with status 1 on any failure other than the files named in $ALLOW
# (space-separated), including a program killed by a signal or the timeout, or
# when a *-error test is not rejected with status 1.
source "$(dirname "$0")/common.sh"
OUT=${OUT:-$REPO/build/corpus/mixed}; mkdir -p "$OUT"; OUT=$(realpath "$OUT"); cd "$OUT" || exit 1
E=$R/test-results/$RESULTS
: > tfail.txt; : > cfail.txt; : > wrong.txt; : > ok.txt; : > errtests.txt
for f in "$R"/mixed-*.cpp2; do
  n=$(basename "$f" .cpp2)
  # Upstream tests include their own headers relative to the test directory.
  if [[ $n == *-error ]]; then
    (cd "$R" && timeout 20 "$BIN" "$n.cpp2" -o "$OUT/$n.cpp") > "$n.tr" 2>&1; status=$?
    # A crash or a timeout is not a rejection.
    if [ $status -eq 0 ]; then echo "$n: translated (expected error)" >> errtests.txt
    elif [ $status -eq 1 ]; then echo "$n: rejected" >> errtests.txt
    else echo "$n: crashed or timed out (status $status)" >> errtests.txt
    fi
    continue
  fi
  if ! (cd "$R" && timeout 20 "$BIN" "$n.cpp2" -o "$OUT/$n.cpp") > "$n.tr" 2>&1; then
    echo "$n: $(grep -m1 ": error:" "$n.tr")" >> tfail.txt; continue
  fi
  if ! timeout 120 $CXX -std=c++23 -I"$RUNTIME" -pthread -x c++ "$n.cpp" -o test.exe \
       2> "$n.cc"; then
    echo "$n: $(grep -m1 'error' "$n.cc")" >> cfail.txt; continue
  fi
  timeout 10 ./test.exe > "$n.execution" 2>&1; status=$?; rm -f test.exe
  expected=$E/$n.cpp.execution
  [ -f "$REPO/tests/corpus/$n.cpp.execution" ] && expected=$REPO/tests/corpus/$n.cpp.execution
  # A program killed by a signal or by the timeout is wrong whatever it printed.
  if [ $status -ge 124 ] ||
     { [ -f "$expected" ] && ! diff -q <(sed -E 's/auto:[0-9]+/auto:N/g' "$expected") \
       <(sed -E 's/auto:[0-9]+/auto:N/g' "$n.execution") > /dev/null; }; then
    echo "exit status $status" >> "$n.execution"
    echo "$n" >> wrong.txt
  else
    echo "$n" >> ok.txt
  fi
done
rm -f ./*.cpp ./*.cc ./*.tr
echo "ok=$(wc -l < ok.txt) tfail=$(wc -l < tfail.txt) cfail=$(wc -l < cfail.txt)" \
  "wrong=$(wc -l < wrong.txt) (error tests: $(grep -c rejected errtests.txt)/$(wc -l < errtests.txt) rejected)"
failures=$(cat tfail.txt cfail.txt wrong.txt <(grep -v ': rejected$' errtests.txt) | cut -d: -f1 |
  grep -vxF -f <(tr ' ' '\n' <<< "${ALLOW:-}" | grep .))
[ -z "$failures" ] || { echo "failing:" $failures; exit 1; }
