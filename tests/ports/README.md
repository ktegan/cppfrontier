# Hand-ported programs

Programs written in Cpp2 from Cpp1 tutorials, reference examples and tricky
compiler cases. Each was written from the Cpp2 documentation, so it also checks
that the documentation is enough to write correct code.

- `<topic>/<name>.cpp2` with `<name>.out`: translated, compiled with
  `-std=c++23` (or `// cxx-std: c++26`), run, and compared with the `.out` file,
  trailing newlines included. The program must exit 0, or with the status a
  `// exit: N` line gives.
- `<topic>/<name>.cpp2` with `// expect-error: text` lines: must be rejected,
  with each text in the diagnostic lines (not the status line).
- `// flags: ...` adds translator flags. Each of `flags`, `cxx-std` and `exit`
  may appear once. `experiments/` holds the ports of
  opt-in experiments, each with `// flags: -experiment=name`
  (`docs/EXPERIMENTS.md`); `<name>_default.cpp2` is the program without
  the flag, changed only where the default requires, showing the default
  behavior.

A comment at the top of each file gives its source and what it exercises, and
any difference from upstream cppfront. Run them with `ctest -L ports`.
