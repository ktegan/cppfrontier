# cppfrontier

cppfrontier is an independent Cpp2-to-C++ translator based on Herb Sutter's
[cppfront](https://github.com/hsutter/cppfront). Its cleanly layered design
(lexer → parser → IR → lowering) is built for clarity and experimentation.

## Goals

- **A reference implementation that is easy to understand, modify, and
  extend.** Each pipeline stage has one clear input and output, semantic
  transforms are independent of the CLI and editor adapters, and data flow is
  explicit rather than global.
- **A testbed for experiments.** The main use is trying possible new Cpp2
  features or changes to existing ones. Code should be structured so that an
  experiment touches as few stages as possible and is easy to remove.
- **Full Cpp2 compatibility.** The default behavior follows the Cpp2
  language documentation, and the whole upstream regression corpus is part of
  the test suite. Matching upstream cppfront exactly is not a goal: where the
  documentation leaves a rule open, it is chosen by the
  [design principles](#design-principles) and stated in
  [docs/LANGUAGE.md](docs/LANGUAGE.md). Choices that differ from upstream are
  listed in [docs/UPSTREAM_DIFFERENCES.md](docs/UPSTREAM_DIFFERENCES.md).
  Experimental changes are opt-in, so they never change the default
  language; see [docs/EXPERIMENTS.md](docs/EXPERIMENTS.md).
- **Fairly performant and useful.** The command line accepts upstream's
  flags, so `cppfrontier` can stand in for `cppfront` in a build. It also has a
  language server (`cppfrontier-lsp`) and source maps. Translation should stay
  within a small factor of upstream's speed.

Generated Cpp1 is meant to have upstream's semantics. It does not have to
match upstream byte for byte.

## Design principles

cppfrontier is meant to be a clean reference implementation of Cpp2 that is
easy to understand, modify and extend, fairly performant and useful, and
mostly a testbed for experiments. Where the Cpp2 documentation leaves a rule
open, the rule is chosen by these criteria, in order:

0. an option that is clearly superior;
1. the simplest and most consistent rule, one that applies the same way to
   every input;
2. what Cpp2 and upstream cppfront do;
3. the more ergonomic for the user;
4. the more consistent with Cpp1.

The translator gathers no type information. A rule that depends on a type is
left to the Cpp1 compiler, through a `static_assert` in the generated Cpp1
where the translator states the rule. A check that only reports earlier what
the Cpp1 compiler already reports clearly is not added.

## Building and testing

The tests compare against upstream cppfront, so check it out next to this
repository, at `../cppfront` (the audit used commit `66eb614`). CMake fetches
lexy and Catch2.

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
(cd build && ctest -j6)             # everything; needs g++
(cd build && ctest -j6 -LE corpus)  # unit and runtime tests only
./build/cppfrontier-tests "[cli]"   # one tag: [cli], [lsp], [source-map], ...
```

The build produces `build/cppfrontier`, which takes upstream cppfront's flags
(`cppfrontier -help`), and `build/cppfrontier-lsp`, a Language Server Protocol
server on standard input and output. `-experiment=list` lists the opt-in
language experiments.

The `corpus` label runs the `tools/` scripts, each of which fails on any
regression:

- `corpus-pure2.sh` and `corpus-mixed.sh`: every upstream regression test
  translates, compiles and prints upstream's output, and every `*-error` test
  is rejected.
- `diagnostics-parity.sh`: every `*-error` test is rejected with an error on
  each line upstream reports one.
- `fuzz-truncation.py` and `fuzz-mutation.py`: no crashes or hangs on
  truncated or randomly edited inputs.
- `self-host.sh`: upstream cppfront, built from our translation of its own
  `reflect.h2`, `cpp2regex.h2` and `cpp2taylor.h2`, builds without new
  warnings and translates every regression test exactly as upstream's own
  build does; `--full` also runs the regex and autodiff tests against the
  translated headers.
- `strict-compile.sh`: the Cpp1 generated for every regression test and port
  compiles under `-Wall -Wextra -Wpedantic` with no warning that upstream's
  Cpp1 lacks, and prints the expected output under AddressSanitizer, UBSan
  and `_GLIBCXX_ASSERTIONS`. The `strict-compile` test checks a subset with
  `-Werror`; `--matrix` runs every program with g++, clang++-20 and
  clang++-21 (and libc++ when it is installed), and the unit tests' programs.
- `diff-fuzz.py`: random Cpp2 programs translated by cppfrontier and upstream
  compile, run and print the same output (`--seeds A:B` for a longer run).
- `wild-corpus.py`: Cpp2 written outside the project (the repositories at the
  pinned commits in `wild-corpus.json`, and the code blocks of upstream's
  documentation) is accepted, and its Cpp1 compiles, wherever upstream's is,
  unless its `KNOWN` list explains why not. It downloads the code, so only
  the `wild-corpus-manifest` test, which checks the manifest, runs in ctest.

To check with Clang, configure with `-DCMAKE_CXX_COMPILER=clang++`. For a
sanitizer build, configure with `-DCPPFRONTIER_SANITIZE=ON`. The Debug build is
unoptimized, so measure performance with a Release build
(`-DCMAKE_BUILD_TYPE=Release`, target `cppfrontier-bin`).

Generated code includes the headers in `runtime/`: upstream's runtime with the
patches listed in `runtime/README.md`, under upstream's license
(`runtime/LICENSE`).

## Documentation

- [docs/LANGUAGE.md](docs/LANGUAGE.md): the language rules where the Cpp2
  documentation is silent or open to more than one reading.
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): the pipeline, modules, Cpp2
  coverage, and verification layers.
- [docs/UPSTREAM_DIFFERENCES.md](docs/UPSTREAM_DIFFERENCES.md): every known
  way cppfrontier differs from upstream cppfront.
- [docs/UPSTREAM_AUDIT.md](docs/UPSTREAM_AUDIT.md): comparison with upstream
  behavior, issues, and deliberate differences.
- [docs/OPEN_QUESTIONS.md](docs/OPEN_QUESTIONS.md): language questions still
  open.
- [docs/EXPERIMENTS.md](docs/EXPERIMENTS.md): opt-in language experiments, and
  how to add and remove one; [docs/experiments/](docs/experiments/) has a page
  per experiment with its motivation, prior art and an example.

## License

cppfrontier is released under the MIT License (`LICENSE`). The runtime headers
in `runtime/` are derived from upstream cppfront and keep its Apache 2.0 with
LLVM Exceptions license (`runtime/LICENSE`).
