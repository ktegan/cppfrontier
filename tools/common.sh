# Shared settings for the corpus scripts; every value can be overridden.
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
UPSTREAM=$(realpath "${UPSTREAM:-$REPO/../cppfront}")
BIN=$(realpath "${BIN:-$REPO/build/cppfrontier}")
CXX=${CXX:-g++}
R=$UPSTREAM/regression-tests
# The Cpp2 runtime headers that generated code includes (see runtime/README.md).
RUNTIME=$(realpath "${RUNTIME:-$REPO/runtime}")
# Upstream expected-output directory for the compiler in use, e.g. clang-19-c++23-libcpp.
RESULTS=${RESULTS:-gcc-14-c++2b}
