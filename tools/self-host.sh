#!/bin/bash
# Self-hosting check: build upstream cppfront from cppfrontier's translation of
# upstream's own Cpp2 sources, and check that it translates the upstream
# regression corpus exactly as upstream's cppfront does.
# usage: [BIN=...] [UPSTREAM=...] [OUT=...] [JOBS=...] [CXX=...] tools/self-host.sh [--full]
# 1. Copies upstream's source/ and include/ twice under $OUT: ref/ as checked
#    in, and self/, where cppfrontier translates source/reflect.h2,
#    include/cpp2regex.h2 and include/cpp2taylor.h2. self/ uses cppfrontier's
#    runtime/cpp2util.h, which generated code needs (runtime/README.md).
# 2. Builds cppfront.cpp in both with upstream's CI flags; self/ must give no
#    warning that ref/ does not give ($OUT/warnings.txt).
# 3. Translates every regression test with both binaries, with upstream's
#    harness options, and requires identical Cpp1 and messages
#    ($OUT/diff.txt). The self-hosted binary must also translate upstream's
#    three .h2 files to upstream's checked-in headers.
# --full also compiles and runs the regression tests that use @regex or
# @autodiff against the headers in self/include, and compares their output
# with upstream's gcc-14 .execution results ($OUT/wrong.txt).
source "$(dirname "$0")/common.sh"
OUT=${OUT:-$REPO/build/corpus/self-host}; mkdir -p "$OUT"; OUT=$(realpath "$OUT"); cd "$OUT" || exit 1
JOBS=${JOBS:-4}
FLAGS="-std=c++20 -Wall -Wextra -Wold-style-cast -Wunused-parameter -Wpedantic -pthread
  -Wno-unknown-warning -Wno-unknown-warning-option"
rm -rf ref self out; mkdir -p out/ref out/self
for side in ref self; do mkdir "$side"; cp -r "$UPSTREAM/source" "$UPSTREAM/include" "$side/"; done
cp "$RUNTIME/cpp2util.h" self/include/

# A `next` clause that is a list `(a, b)` is a Cpp1 comma operator upstream and
# an error in cppfrontier (docs/UPSTREAM_AUDIT.md, "Assignments as values"); the
# loop's increments move to the end of its body, which has no `continue`.
perl -0pi -e 's/\n    next \(group_pos \+= group_inc, cur\+\+\)\n/\n\n/ or die;
  s/(\n            if group_pos\* != cur\* \{\n                return false;\n            \}\n        \}\n    )\}/$1group_pos += group_inc; cur++; }/ or die' \
  self/include/cpp2regex.h2 || { echo "cpp2regex.h2 rewrite no longer applies"; exit 1; }
for h2 in source/reflect.h2 include/cpp2regex.h2 include/cpp2taylor.h2; do
  if ! (cd "self/$(dirname $h2)" && timeout 60 "$BIN" "$(basename $h2)" -o "$(basename "${h2%2}")") \
       > translate.txt 2>&1; then
    cat translate.txt; echo "cppfrontier fails to translate $h2"; exit 1
  fi
done

for side in ref self; do
  (cd $side && timeout 300 $CXX source/cppfront.cpp $FLAGS -o cppfront > build.txt 2>&1
   echo $? > status) &
done
wait
for side in ref self; do
  [ "$(cat $side/status)" = 0 ] || { grep -m5 error $side/build.txt; echo "$side build fails"; exit 1; }
  grep 'warning:' $side/build.txt | sed -E 's/^([^:]+:[0-9]+):[0-9]+:/\1:/' | sort -u > $side/warnings
done
# Known: Clang's -Wbraced-scalar-init for `diff = { ctx }` in the converting
# assignment generated from a constructor that initializes `diff = (ctx)`; a
# parenthesized value is assigned as a braced list there as in any assignment
# (docs/UPSTREAM_DIFFERENCES.md), where upstream writes `diff = ctx`.
comm -13 ref/warnings self/warnings | grep -v -- '-Wbraced-scalar-init' > warnings.txt

# Translates one test with both binaries, as upstream's run-tests.sh does.
translate() {
  n=$(basename "$1" .cpp2); opt=""
  [[ $n == pure2* ]] && opt="-p -include-std"
  for side in ref self; do
    (cd "$R" && timeout 60 "$OUT/$side/cppfront" "$n.cpp2" -o "$OUT/out/$side/$n.cpp" $opt) \
      > "$OUT/out/$side/$n.output" 2>&1
    echo "status $?" >> "$OUT/out/$side/$n.output"
  done
}
export -f translate; export OUT R
ls "$R"/*.cpp2 | xargs -P "$JOBS" -I{} bash -c 'translate {}'
diff -r out/ref out/self > diff.txt

# The output name and directory appear in the generated header.
mkdir -p out/boot/source out/boot/include
for h2 in source/reflect.h2 include/cpp2regex.h2 include/cpp2taylor.h2; do
  cp "$UPSTREAM/$h2" "out/boot/$h2"
  (cd "out/boot/$(dirname $h2)" && timeout 60 "$OUT/self/cppfront" "$(basename $h2)") \
    > /dev/null 2>&1
  cmp -s "out/boot/${h2%2}" "$UPSTREAM/${h2%2}" || echo "bootstrap differs: ${h2%2}" >> diff.txt
done

: > wrong.txt
if [ "${1:-}" = --full ]; then
  # Runs one test that uses the translated regex or autodiff runtime.
  run() {
    n=$(basename "$1" .cpp); dir=$OUT/run/$n; mkdir -p "$dir"; cd "$dir" || return
    if ! timeout 120 $CXX -std=c++23 -I"$OUT/self/include" -pthread -x c++ "$1" -o test.exe \
         > cc 2>&1; then
      echo "$n: compile: $(grep -m1 error cc)"; return
    fi
    timeout 10 ./test.exe > execution 2>&1; status=$?
    expected=$R/test-results/$RESULTS/$n.cpp.execution
    if [ $status -ge 124 ] || { [ -f "$expected" ] && ! diff -q "$expected" execution > /dev/null; }; then
      echo "$n: output differs (exit status $status)"
    fi
    cd "$OUT" && rm -rf "$dir"
  }
  export -f run; export CXX RESULTS
  grep -l 'cpp2regex.h\|cpp2taylor.h' "$OUT"/out/self/*.cpp | xargs -P "$JOBS" -I{} bash -c 'run {}' \
    | sort > wrong.txt
  echo "regex and autodiff tests run: $(grep -l 'cpp2regex.h\|cpp2taylor.h' out/self/*.cpp | wc -l)," \
    "wrong: $(wc -l < wrong.txt)"
fi

echo "tests translated: $(ls out/self/*.output | wc -l), differing: $(grep -c '^diff\|^Only\|^bootstrap' diff.txt)," \
  "new warnings: $(wc -l < warnings.txt) (upstream's build: $(wc -l < ref/warnings))"
[ -s diff.txt ] || [ -s warnings.txt ] || [ -s wrong.txt ] || exit 0
head -40 diff.txt warnings.txt wrong.txt; exit 1
