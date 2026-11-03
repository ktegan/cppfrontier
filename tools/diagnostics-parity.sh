#!/bin/bash
# Compare diagnostics on upstream *-error.cpp2 tests with upstream's expected output.
# Exits with status 1 unless every test is in category C.
# A: we accept, upstream rejects. B: both reject, but we report no error on
# some line upstream does. C: both reject, and we report an error on every
# line upstream does (upstream stops after some errors that we report with
# others). D: we reject, upstream accepts. E: the translator crashed or timed
# out. K: a known difference, in $KNOWN.
# usage: [BIN=...] [UPSTREAM=...] [OUT=...] tools/diagnostics-parity.sh
source "$(dirname "$0")/common.sh"
OUT=${OUT:-$REPO/build/corpus/diagnostics}; mkdir -p "$OUT"; OUT=$(realpath "$OUT")
# Upstream reports each `#else` one line after it. A `-> forward _` that
# returns a local by name deduces a by-value return (functions.md).
KNOWN=${KNOWN:-mixed-bugfix-for-double-pound-else-error.cpp2 pure2-forward-return-diagnostics-error.cpp2}
declare -A count
for f in "$R"/*-error.cpp2; do
  n=$(basename "$f")
  expected=$R/test-results/$n.output
  error_lines() { grep -oE "^$n\(([0-9]+),[0-9]+\): error:" "$1" | sed -E 's/.*\(([0-9]+),.*/\1/' | sort -un | tr '\n' ' '; }
  upstream_lines=$(error_lines "$expected")
  upstream_rejects=0; grep -q "error:" "$expected" && upstream_rejects=1
  (cd "$R" && timeout 20 "$BIN" "$n" -o "$OUT/$n.cpp") > "$OUT/$n.txt" 2>&1; status=$?
  our_lines=$(error_lines "$OUT/$n.txt")
  we_reject=0; [ $status -ne 0 ] && we_reject=1
  if [ $status -ge 124 ]; then c=E
  elif [ $upstream_rejects = 1 ] && [ $we_reject = 0 ]; then
    c=A; [[ " $KNOWN " == *" $n "* ]] && c=K
  elif [ $upstream_rejects = 0 ] && [ $we_reject = 1 ]; then c=D
  elif [ $upstream_rejects = 0 ]; then c=OK
  else
    c=C
    for line in $upstream_lines; do [[ " $our_lines " == *" $line "* ]] || c=B; done
    [ $c = B ] && [[ " $KNOWN " == *" $n "* ]] && c=K
  fi
  count[$c]=$((${count[$c]:-0} + 1))
  echo "$c $n upstream=[$upstream_lines] ours=[$our_lines] $(grep -m1 ': error:' "$OUT/$n.txt" | cut -c1-160)"
done
rm -f "$OUT"/*.cpp
for k in A B C D E K OK; do echo -n "$k=${count[$k]:-0} "; done; echo
(( ${count[A]:-0} + ${count[B]:-0} + ${count[D]:-0} + ${count[E]:-0} == 0 ))
