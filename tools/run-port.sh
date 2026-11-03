#!/bin/bash
# Checks one hand-ported program in tests/ports. A file with `// expect-error: text`
# lines must be rejected with each text in its diagnostic lines; any other file is
# translated, compiled and run, and its output, trailing newlines included, compared
# with the .out file next to it. The program must exit 0, or the status that a
# `// exit: N` line gives. `// cxx-std: c++26` selects the Cpp1 standard (default
# c++23), and `// flags: -experiment=name` adds translator flags. Each of `flags`,
# `cxx-std` and `exit` may appear once.
# usage: run-port.sh <cppfrontier> <c++ compiler> <cppfront include dir> <file.cpp2> <scratch dir>
set -u
bin=$(realpath "$1") cxx=$2 include=$(realpath "$3") source=$(realpath "$4") scratch=$5
name=$(basename "$source" .cpp2)
mkdir -p "$scratch"
setting() {
  local values
  mapfile -t values < <(sed -n "s|^[[:space:]]*// $1:[[:space:]]*||p" "$source")
  if (( ${#values[@]} > 1 )); then echo "more than one '// $1:' line" >&2; return 1; fi
  printf '%s' "${values[0]-}"
}
mapfile -t errors < <(sed -n 's|^[[:space:]]*// expect-error:[[:space:]]*||p' "$source")
std=$(setting cxx-std) && flag_text=$(setting flags) && expected_exit=$(setting exit) || exit 1
read -ra flags <<< "$flag_text"
[[ ${expected_exit:=0} =~ ^[0-9]+$ ]] || { echo "invalid '// exit:' status '$expected_exit'"; exit 1; }
output=$(cd "$scratch" && timeout 30 "$bin" "${flags[@]}" "$source" -o "$name.cpp" 2>&1)
status=$?
if (( ${#errors[@]} )); then
  if (( status == 0 )); then echo "accepted, expected rejection:"; printf '  %s\n' "${errors[@]}"; exit 1; fi
  diagnostics=$(grep -E ': (error|warning): ' <<< "$output")
  for text in "${errors[@]}"; do
    [[ $diagnostics == *"$text"* ]] || { echo "missing diagnostic: $text"; echo "$output"; exit 1; }
  done
  exit 0
fi
(( status == 0 )) || { echo "translation failed:"; echo "$output"; exit 1; }
timeout 120 "$cxx" "-std=${std:-c++23}" -I "$include" -o "$scratch/$name" "$scratch/$name.cpp" 2>&1 ||
  { echo "Cpp1 compilation failed"; exit 1; }
expected=${source%.cpp2}.out
[[ -f $expected ]] || { echo "missing $expected"; exit 1; }
(cd "$scratch" && timeout 10 "./$name" > "$name.actual" 2>&1)
status=$?
cmp -s "$expected" "$scratch/$name.actual" ||
  { echo "output differs"; diff "$expected" "$scratch/$name.actual"; exit 1; }
(( status == expected_exit )) || { echo "exit status $status, expected $expected_exit"; exit 1; }
