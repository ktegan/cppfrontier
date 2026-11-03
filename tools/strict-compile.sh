#!/bin/bash
# Compile the Cpp1 generated for the corpus with warnings and sanitizers, run it,
# and compare its output. The corpus is upstream's pure2-* and mixed-* regression
# tests (expected output from $RESULTS, as corpus-pure2.sh) and tests/ports/**
# (expected output from the .out file, as run-port.sh); *-error tests and
# expect-error ports are skipped.
# usage: [CXX=g++] [STDLIB=libc++] [WERROR=1] [OPT=-O0 -g1] [OUT=...] [JOBS=...] [UPSTREAM_BIN=...]
#          tools/strict-compile.sh [--fast | --matrix | --unit | file.cpp2...]
#   (default)  every program, compiled with $CXX.
#   --fast     the FAST subset below with -Werror (the strict-compile ctest).
#   --matrix   every program with each installed compiler: g++, clang++-20 and
#              clang++-21, each also with libc++ when it is installed, and then --unit;
#              each run writes to $OUT/<compiler>[-libc++]. A libc++ run compares
#              with upstream's clang-19-c++23-libcpp results.
#   --unit     the unit tests (build/cppfrontier-tests, $TESTS), with every program
#              they compile built by $CXX with these flags; see "Unit tests" below.
# Each program is compiled with -Wall -Wextra -Wpedantic, -fsanitize=address,undefined
# and -D_GLIBCXX_ASSERTIONS, and run with sanitizer reports logged under its directory.
# $OUT/<name>.result says ok, tfail (translation), cfail (compilation), wrong (output
# or exit status) or sanitizer (a report, or a library assertion); failing programs
# keep their directory. A warning is "new" unless upstream's Cpp1 for the same program
# (translated by $UPSTREAM_BIN, compiled with the same flags) has a warning with the
# same message, in the same header for a header; otherwise it is "upstream". New
# warnings are noted when upstream rejects the program or its Cpp1 does not compile.
# Warnings are listed in $OUT/warnings.txt. Exits with status 1 on a failing program
# or a new warning that is not in KNOWN.
# $RESULTS may name several result directories; the output must match one. With
# clang and libstdc++, function signatures print as in upstream's clang results and
# library output as in its gcc results.
GIVEN_RESULTS=${RESULTS:-}
if [ -z "$GIVEN_RESULTS" ] && [[ $(basename "${CXX:-g++}") == clang* ]] && [ "${STDLIB:-}" != libc++ ]; then
  RESULTS="clang-19-c++23-libcpp gcc-14-c++2b"
fi
source "$(dirname "$0")/common.sh"
OUT=${OUT:-$REPO/build/corpus/strict-$(basename "$CXX")${STDLIB:+-$STDLIB}}

# Programs checked by --fast: one per main feature area, all warning-free.
FAST="pure2-hello pure2-types-basics pure2-types-inheritance pure2-types-ordering-via-meta-functions
  pure2-bugfix-for-memberwise-base-assignment pure2-inspect-fallback-with-variant-any-optional
  pure2-contracts pure2-bounds-safety-span pure2-union mixed-parameter-passing mixed-bounds-check
  mixed-string-interpolation ports/basics/hello ports/experiments/condition_bindings
  ports/experiments/comprehension ports/experiments/nonnegative_literal_comparisons"

# Known results, one per line: "<name> <kind>[ <compiler>]: <reason>", where the kind
# is tfail, cfail, wrong, sanitizer, or a new warning's flag (-Wname, or "warning"
# for one without a flag). An entry with a compiler applies to the compilers whose
# name starts with it. Programs that test violations on purpose (contract, bounds
# and null checks) print upstream's output without a sanitizer report.
KNOWN=$(cat << 'EOF'
ports/syntax/postfix_star_template_arguments cfail g++: g++ -fsanitize=undefined rejects comparing an inline variable's address with null in a constant expression
pure2-type-constraints -Wunused-parameter: user text: `r` is unused; upstream's requires clause names it
ports/compiletime/concept-order -Wunused-parameter: user text: an unused named parameter
ports/compiletime/concept-overloads -Wunused-parameter: user text: an unused named parameter
ports/compiletime/constraint-overloads -Wunused-parameter: user text: an unused named parameter
ports/compiletime/dependent-type-marker -Wunused-parameter: user text: an unused named parameter
ports/experiments/given_namespaces -Wunused-parameter: user text: an unused named parameter
ports/experiments/given_types -Wunused-parameter: user text: an unused named parameter
ports/language/function_type_returns -Wunused-parameter: user text: an unused named parameter
ports/language/requires_parameters -Wunused-parameter: user text: an unused named parameter
ports/language/autodiff_passive_loops -Wunused-variable: user text: an unused loop parameter `i`
ports/experiments/checked_arithmetic_wrapping warning g++: user text: an unsuffixed literal above INT64_MAX
ports/experiments/checked_arithmetic_wrapping -Wimplicitly-unsigned-literal clang++: user text: an unsuffixed literal above INT64_MAX
ports/language/assignment_early_return -Wself-assign-overloaded clang++: user text: `c = c;` tests self-assignment
ports/syntax/mixed_cpp1_forms -Wtautological-compare clang++: user text: `green == Color::green` compares an enumerator with itself
pure2-last-use cfail clang++: the Cpp1 does not compile with clang, as upstream's clang results show
EOF
)

case ${1:-} in
--matrix)
  shift; status=0
  for cxx in g++ clang++-20 clang++-21; do
    command -v "$cxx" > /dev/null || { echo "$cxx: not installed"; continue; }
    for stdlib in "" libc++; do
      if [ -n "$stdlib" ] && ! echo 'int main() {}' |
           "$cxx" -stdlib=libc++ -x c++ -include vector - -o /dev/null 2> /dev/null; then
        echo "$cxx -stdlib=libc++: libc++ is not installed"; continue
      fi
      echo "== $cxx${stdlib:+ -stdlib=$stdlib}"
      # Upstream's gcc results are libstdc++'s output, and its clang results libc++'s.
      results=$GIVEN_RESULTS; [ -n "$stdlib" ] && results=clang-19-c++23-libcpp
      CXX=$cxx STDLIB=$stdlib RESULTS=$results OUT=$OUT/$cxx${stdlib:+-$stdlib} "$0" "$@" ||
        status=1
    done
  done
  echo "== unit tests ($CXX)"
  OUT=$OUT/unit "$0" --unit || status=1
  exit $status;;
esac

# The rewritten tests, as in CMakeLists.txt.
REWRITTEN=${REWRITTEN-pure2-last-use}
args=(); for f in "$@"; do [[ $f == --* ]] && args+=("$f") || args+=("$(realpath "$f")"); done
set -- "${args[@]}"
mkdir -p "$OUT"; OUT=$(realpath "$OUT"); cd "$OUT" || exit 1
rm -rf ./*.result ./*.warnings ./*/
[ "${1:-}" = --fast ] && WERROR=${WERROR-1}
FLAGS="-Wall -Wextra -Wpedantic${WERROR:+ -Werror} ${OPT:--O0 -g1}
  -fsanitize=address,undefined -D_GLIBCXX_ASSERTIONS -pthread${STDLIB:+ -stdlib=$STDLIB}"
SANITIZER_REPORT='runtime error:|==[0-9]+==ERROR:|Sanitizer|Assertion .* failed'

if [ "${1:-}" = --unit ]; then
  # Unit tests: CPPFRONTIER_TEST_CXX replaces the compiler the tests run (see
  # tests/unit/test_support.hpp) with a wrapper that adds the flags and logs each
  # compilation's diagnostics. The test sources are user text, so warnings are only
  # listed; failing tests and sanitizer reports fail the run.
  BUILD=$(realpath "${BUILD:-$REPO/build}")
  mkdir -p log
  # A compilation with a warning keeps its source as log/cc-*.cpp.
  cat > cxx << EOF
#!/bin/bash
out=\$(mktemp "$OUT/log/cc-XXXXXX")
$CXX "\$@" $(echo $FLAGS | sed 's/-Werror//') 2> "\$out"; status=\$?
cat "\$out" >&2
if grep -q ': warning:' "\$out"; then
  for a in "\$@"; do [[ \$a == *.cpp ]] && cp "\$a" "\$out.cpp"; done
fi
exit \$status
EOF
  chmod +x cxx
  export CPPFRONTIER_TEST_CXX=$OUT/cxx ASAN_OPTIONS=log_path=$OUT/log/asan
  export UBSAN_OPTIONS=log_path=$OUT/log/ubsan:print_stacktrace=1
  (cd "$BUILD" && timeout 2400 ctest -LE 'corpus|ports|scaling' -j "${JOBS:-4}" --timeout 1200) \
    > ctest.txt 2>&1
  sed -n 's/^ *[0-9]* - \(.*\) (.*)$/\1/p' ctest.txt > failed.txt
  grep -h ': warning:' log/cc-?????? 2> /dev/null | sed -E 's/^[^:]*\/([^/:]+):[0-9:]+: /\1: /' |
    sort | uniq -c | sort -rn > warnings.txt
  grep -lE "$SANITIZER_REPORT" log/asan* log/ubsan* 2> /dev/null > sanitizer.txt
  echo "$(grep -m1 'tests passed' ctest.txt)" \
    "compilations=$(ls log/cc-?????? 2> /dev/null | wc -l)" \
    "warnings=$(awk '{s += $1} END {print s + 0}' warnings.txt)" \
    "sanitizer-reports=$(wc -l < sanitizer.txt)"
  [ -s failed.txt ] && { echo "failing:"; cat failed.txt; }
  { [ -s failed.txt ] || [ -s sanitizer.txt ]; } && exit 1
  exit 0
fi

# The upstream translator, built from $UPSTREAM/source when none is given.
if [ -z "${WERROR:-}" ] && [ -z "${UPSTREAM_BIN:-}" ]; then
  UPSTREAM_BIN=$OUT/../upstream-cppfront
  [ -x "$UPSTREAM_BIN" ] || timeout 300 g++ -std=c++20 -O2 "$UPSTREAM/source/cppfront.cpp" \
    -o "$UPSTREAM_BIN" || UPSTREAM_BIN=
fi
[ -n "${UPSTREAM_BIN:-}" ] && UPSTREAM_BIN=$(realpath "$UPSTREAM_BIN")
export BIN CXX FLAGS REWRITTEN UPSTREAM UPSTREAM_BIN RUNTIME REPO R OUT SANITIZER_REPORT RESULTS

# Prints each warning in the compiler output $1 as "<location> <message> [-Wflag]",
# where the location is the file name and line, or a header's name.
warning_keys() {
  grep -E ': warning: ' "$1" | sed -E 's/^([^:]*):([0-9]+)(:[0-9]+)?: warning: /\1:\2 /' |
    sed -E 's|^[^ ]*/||; s/^([^ ]*\.(h|hpp|h2)):[0-9]+ /\1 /' | sort -u
}
# The part of a warning that must match upstream's: the message, and for a header,
# its name.
warning_match() { sed -E 's/^[^ ]*\.(cpp|cpp2):[0-9]+ //'; }
export -f warning_keys warning_match

check() {
  kind=${1%%:*} f=${1#*:}
  if [ "$kind" = reg ]; then n=$(basename "$f" .cpp2); else
    n=ports/$(realpath --relative-to="$REPO/tests/ports" "${f%.cpp2}"); fi
  dir=$OUT/$n; mkdir -p "$dir"; cd "$dir" || return
  result() { echo "$1 $n${2:+: $2}" > "$OUT/${n//\//-}.result"; }
  base=$(basename "$f" .cpp2) std=c++23 tflags=() defines=() status_expected=0
  if [ "$kind" = reg ]; then
    src=$R
    if [[ " ${REWRITTEN:-} " == *" $n "* ]]; then
      perl "$REPO/tools/upstream-rewrites.pl" "$f" > "$dir/$n.cpp2"; src=$dir
    fi
    [[ $n == pure2-* ]] && defines=(-DCPP2_INCLUDE_STD=Yes)
    expected=()
    for results in $RESULTS; do
      [ -f "$R/test-results/$results/$n.cpp.execution" ] &&
        expected+=("$R/test-results/$results/$n.cpp.execution")
    done
    [ -f "$REPO/tests/corpus/$n.cpp.execution" ] && expected=("$REPO/tests/corpus/$n.cpp.execution")
  else
    src=$(dirname "$f") expected=${f%.cpp2}.out
    setting() { sed -n "s|^[[:space:]]*// $1:[[:space:]]*||p" "$f" | head -1; }
    std=$(setting cxx-std); std=${std:-c++23}
    read -ra tflags <<< "$(setting flags)"
    status_expected=$(setting exit); status_expected=${status_expected:-0}
  fi
  if ! (cd "$src" && timeout 60 "$BIN" "${tflags[@]}" "$base.cpp2" -o "$dir/test.cpp") > tr 2>&1; then
    result tfail "$(grep -m1 ': error:' tr)"; return
  fi
  if ! timeout 120 $CXX $FLAGS -std=$std "${defines[@]}" -I"$RUNTIME" -x c++ test.cpp -o test.exe 2> cc; then
    result cfail "$(grep -m1 'error' cc)"; return
  fi
  # Warnings, each marked by whether upstream's Cpp1 has the same one.
  if grep -q ': warning: ' cc; then
    upstream_keys=; note=" (upstream rejects it)"; mkdir -p upstream
    # Upstream writes cpp2_post_build.sh in the current directory.
    if [ -n "$UPSTREAM_BIN" ] && { (cd upstream && timeout 60 "$UPSTREAM_BIN" "${tflags[@]}" \
         "$src/$base.cpp2" -o test.cpp) > upstream/tr 2>&1; } 2> /dev/null; then
      note=
      (cd upstream && timeout 120 $CXX $FLAGS -std=$std "${defines[@]}" -I"$UPSTREAM/include" \
        -I"$src" -x c++ test.cpp -c -o /dev/null 2> cc) || note=" (upstream's Cpp1 does not compile)"
      upstream_keys=$(warning_keys upstream/cc | warning_match)
    fi
    while read -r key; do
      if grep -qxF "$(warning_match <<< "$key")" <<< "$upstream_keys"; then echo "upstream $n $key"
      else echo "new $n $key$note"; fi
    done < <(warning_keys cc) > "$OUT/${n//\//-}.warnings"
  fi
  ASAN_OPTIONS=log_path=$dir/asan UBSAN_OPTIONS=log_path=$dir/ubsan:print_stacktrace=1 \
    timeout 30 ./test.exe > execution 2>&1
  status=$?
  if grep -qE "$SANITIZER_REPORT" execution asan* ubsan* 2> /dev/null; then
    result sanitizer "$(grep -hm1 -E "$SANITIZER_REPORT" execution asan* ubsan* 2> /dev/null | head -1)"
    return
  fi
  if [ "$kind" = reg ]; then
    # As corpus-pure2.sh: a program killed by a signal or the timeout is wrong.
    matches=$(( ${#expected[@]} == 0 ))
    for file in "${expected[@]}"; do
      diff -q <(sed -E 's/auto:[0-9]+/auto:N/g' "$file") \
        <(sed -E 's/auto:[0-9]+/auto:N/g' execution) > /dev/null && matches=1
    done
    if [ $status -ge 124 ] || [ $matches = 0 ]; then result wrong "exit status $status"; return; fi
  elif ! cmp -s "$expected" execution || [ $status -ne "$status_expected" ]; then
    result wrong "exit status $status"; return
  fi
  result ok
  # A program with a new warning keeps its Cpp1 and compiler output.
  if grep -qs '^new ' "$OUT/${n//\//-}.warnings"; then rm -f test.exe; else cd "$OUT" && rm -rf "${dir:?}"; fi
}
export -f check

if [ "${1:-}" = --fast ]; then
  for n in $FAST; do
    if [[ $n == ports/* ]]; then echo "port:$REPO/tests/$n.cpp2"; else echo "reg:$R/$n.cpp2"; fi
  done
elif [ $# -gt 0 ]; then
  for f in "$@"; do
    if [[ $f == "$REPO/tests/ports/"* ]]; then echo "port:$f"
    else echo "reg:$R/$(basename "$f")"; fi
  done
else
  ls "$R"/pure2-*.cpp2 "$R"/mixed-*.cpp2 | grep -v -- '-error\.cpp2$' | sed 's/^/reg:/'
  grep -rL --include='*.cpp2' '// expect-error:' "$REPO/tests/ports" | sort | sed 's/^/port:/'
fi > programs.txt
xargs -P "${JOBS:-4}" -I{} bash -c 'check {}' < programs.txt

cat ./*.warnings 2> /dev/null | sort > warnings.txt
for kind in tfail cfail wrong sanitizer; do
  grep -h "^$kind " ./*.result | sed "s/^$kind //" | sort > "$kind.txt"
done
echo "programs=$(wc -l < programs.txt) ok=$(cat ./*.result | grep -c '^ok ')" \
  "tfail=$(wc -l < tfail.txt) cfail=$(wc -l < cfail.txt) wrong=$(wc -l < wrong.txt)" \
  "sanitizer=$(wc -l < sanitizer.txt) warnings: new=$(grep -c '^new ' warnings.txt)" \
  "upstream=$(grep -c '^upstream ' warnings.txt)"
known=$(sed -nE 's/^([^ ]+) ([^ :]+)( ([^:]+))?:.*/\1 \2 \4/p' <<< "$KNOWN" |
  while read -r name kind compiler; do
    [[ -z $compiler || $(basename "$CXX") == "$compiler"* ]] && echo "$name $kind"
  done)
failures=$( { for kind in tfail cfail wrong sanitizer; do sed -E "s/^([^:]*).*/\1 $kind/" "$kind.txt"; done
              grep '^new ' warnings.txt | sed -E 's/ \(upstream.*\)$//' |
                awk '{f = "warning"; if (match($0, /\[-W[^]]*\]$/)) f = substr($0, RSTART + 1, RLENGTH - 2)
                      print $2 " " f}'; } | sort -u | grep -vxF -f <(echo "$known" | grep . || echo //))
[ -z "$failures" ] || { echo "failing:"; sed 's/^/  /' <<< "$failures"; exit 1; }
