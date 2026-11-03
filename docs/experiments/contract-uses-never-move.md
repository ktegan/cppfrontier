# `contract-uses-never-move`

This is the default language ([`LANGUAGE.md` "Moves and last use"](../LANGUAGE.md#moves-and-last-use)).

Explored: `OPEN_QUESTIONS.md` L4, moves inside `assert`.

## What it enables

Cpp2 moves from a local or a `copy`/`move` parameter at its definite last
use. A mention in a precondition or postcondition is never moved, and
neither is a mention in an `assert` statement's condition, message or
flags. That mention is still a use: in `g(x); assert(f(x));` neither
statement moves `x`, and `g(x)` copies, since the assertion is a later use.

Upstream cppfront treats a mention in an assertion, a precondition or a
postcondition as an ordinary use. When the assertion holds the last use, the
assertion moves: in the same code the call `f(x)` inside the assertion
becomes `f(cpp2::move(x))`.

## Why it is useful

A contract check should not change the program's state, and it may not run
at all: the assertion is evaluated only while its contract group is active.
Under upstream's rule, moving inside an assertion is a side effect of
evaluating it, so whether `x` is moved from depends on contract checking
being enabled. In cppfrontier an assertion never moves, whichever
statement it follows and whether or not it is evaluated. `assert` also
follows the rule that `pre` and `post` follow.

The cost is a copy where upstream would move: an assertion that passes its
last use to a by-value parameter copies. A predicate that takes its
argument as `in` avoids the copy. A move-only argument passed by value in an
assertion does not compile.

## Prior art

- **C++26 contracts** ([P2900](https://wg21.link/p2900)) evaluate a
  contract assertion's predicate under one of several evaluation semantics,
  including `ignore`, so a program must not depend on the predicate's side
  effects. P2900 also makes
  the variables a predicate names implicitly `const`, which rules out
  moving from them. cppfrontier applies the same idea only to Cpp2's
  implicit last-use move.
- **C's `assert` and Rust's `debug_assert!`** are not evaluated in release
  builds, so a side effect in their condition is a well-known bug. This
  rule removes one side effect that the translator itself would add.
- **cppfrontier's preconditions and postconditions** never move a
  mention, though upstream's do. This rule extends that to `assert`.

## Example

```cpp
Ticket: type = {
    public id: int = 0;
    operator=: (out this, i: int) = { id = i; }
    operator=: (out this, that) = { id = that.id; std::cout << "copy "; }
    operator=: (out this, move that) = { id = that.id; std::cout << "move "; }
}

is_open: (copy t: Ticket) -> bool = t.id > 0;
archive: (copy t: Ticket) = { std::cout << "archive(t.id)$ "; }

close: (copy t: Ticket) = {
    archive(t);
    assert(is_open(t));
}

main: () = {
    close(Ticket(7));
    std::cout << "\n";
    cpp2::cpp2_default.set_handler();   // turn off checking of the default group
    close(Ticket(8));
    std::cout << "\n";
}
```

The assertion reads `t` and does not move it:

```cpp
auto close(Ticket t) -> void  {
    archive(t);
    if (cpp2::cpp2_default.is_active() && !(is_open(t)) ) { cpp2::cpp2_default.report_violation(""); }
}
```
```
copy archive7 copy
copy archive8
```

Upstream cppfront moves `t` in the assertion, but only while checking is on:

```cpp
auto close(Ticket t) -> void{
    archive(t);
    if (cpp2::cpp2_default.is_active() && !(is_open(cpp2::move(t))) ) { cpp2::cpp2_default.report_violation(""); }
}
```
```
copy archive7 move
copy archive8
```

In both translators `archive(t)` copies, because the assertion after it is a use.

## Rules and diagnostics

- A name mentioned in an `assert` statement's condition, message or flags
  is a use for last-use analysis, so an earlier statement that mentions it
  is not its definite last use.
- That mention is never moved, as in `pre` and `post`.
- A mention in a compile-time context is not a use at all (`LANGUAGE.md`
  "Moves and last use").
- The rule has no diagnostics.

Full entry: [`EXPERIMENTS.md`](../EXPERIMENTS.md#part-of-the-default-language).

Tests:
[`contract_uses_never_move.cpp2`](../../tests/ports/experiments/contract_uses_never_move.cpp2),
[`pipeline_test.cpp`](../../tests/unit/pipeline_test.cpp).
