# `use-after-move`

Enable with `-experiment=use-after-move`.
Explores: a new feature.

## What it enables

A flow analysis, run beside definite initialization, that follows each
explicit `move x` argument. After `f(move x)`, a mention of `x` before it
is assigned again is an error: `use-after-move` when every path to the
mention moved it, `use-after-maybe-move` when some path did. A name
declared outside a loop and moved inside it, on a path that reaches its
use on the next iteration, is `move-in-loop`. Each move is reported once.

The analysis follows locals, destructuring bindings and non-`in`
parameters whose type is not a scalar. Assigning the name again, `x = ...`, or passing it as `out x`,
ends the moved state. The generated Cpp1 is unchanged; the experiment only
adds diagnostics.

## Why it is useful

Cpp2 moves a name implicitly only at its definite last use, which is safe
by construction. An explicit `move x` is the programmer's claim that `x`
is not needed afterwards, and nothing checks that claim by default. A
later read sees a valid but unspecified value, often an empty string or
container, so the bug shows up as wrong output far from its cause.

The trade-off: the analysis is conservative about paths. A name moved on
one branch and read after the join is rejected even when the program's
logic guarantees the branch is not taken; the fix is to reassign it or to
restructure the code.

## Prior art

- **Rust's borrow checker** rejects any use of a moved value (error E0382)
  unless it was assigned again, on every path, including moves in loops.
  This experiment is the same rule for explicit moves only; Cpp2's
  implicit last-use moves cannot be followed by a use.
- **clang-tidy's `bugprone-use-after-move`** finds the same bug in Cpp1
  after `std::move` or `std::forward`. It treats assignment, `clear()` and
  similar calls as reinitialization and reports a use on a later loop
  iteration. It is a separate tool and does not recognize mutually
  exclusive branches; here the check is part of translation and is joined
  over the paths of the function.

## Example

`msg` is moved into the outbox, assigned again, and moved again:

```cpp
#include <iostream>
#include <string>
#include <vector>

send: (inout outbox: std::vector<std::string>, move msg: std::string) = {
    outbox.push_back(move msg);
}

main: () = {
    outbox: std::vector<std::string> = ();
    msg: std::string = "hello";
    send(outbox, move msg);
    msg = "world";
    send(outbox, move msg);
    std::cout << outbox[0] << " " << outbox[1] << "\n";
}
```

The Cpp1 is the same with and without the flag:

```cpp
    send(outbox, std::move(msg));
    msg = "world";
    send(outbox, std::move(cpp2::move(msg)));
```

Output:

```
hello world
```

Rejected: a read after a move, and a read after a move on one branch.

```cpp
main: (args) -> int = {
    outbox: std::vector<std::string> = ();
    msg: std::string = "hello";
    send(outbox, move msg);
    std::cout << "sent " << msg << "\n";             // line 13
    note: std::string = "verbose";
    if args.ssize() > 1 {
        send(outbox, move note);
    }
    std::cout << note.size() << " " << outbox.size() << "\n";   // line 18
    return 0;
}
```

```
bad.cpp2:13:5: error: msg was moved on line 12 and is used here [use-after-move]
bad.cpp2:18:5: error: note may have been moved on line 16 and is used here [use-after-maybe-move]
```

Without the flag this program translates, compiles and prints the
moved-from string:

```
sent 
7 1
```

Rejected: a move inside a loop that the next iteration reads. The second
loop assigns `reply` before each move, so it is accepted.

```cpp
main: () = {
    outbox: std::vector<std::string> = ();
    line: std::string = "header";
    for (1, 2, 3) do (i) {
        line += std::to_string(i);
        send(outbox, move line);                     // line 14
    }
    reply: std::string = "";
    for (1, 2, 3) do (i) {
        reply = "reply " + std::to_string(i);
        send(outbox, move reply);
    }
    std::cout << outbox.ssize() << "\n";
}
```

```
loop.cpp2:14:9: error: line is moved inside a loop and is used moved-from on the next iteration [move-in-loop]
```

A loop that refills a name by passing it on, such as
`std::getline(in, line)` after `lines.push_back(move line)`, is also
`move-in-loop`: the analysis does not know that `getline` assigns `line`.
Declare `line` inside the loop instead, so each iteration has its own:

```cpp
while in.good() {
    line: std::string = ();
    std::getline(in, line);
    lines.push_back(move line);
}
```

## Rules and diagnostics

- Followed: locals, destructuring bindings, and parameters other than
  `in` (of functions, statements, range `for` loops and handlers), of a
  type known not to be a scalar. Moving an `int` or a pointer leaves it unchanged, so those are
  not followed, and an `in` parameter is a const reference that a move
  copies.
- A type is known when it is written, or deduced from an initializer of
  known type: a literal, `:T = v`, `x as T`, `T(...)` for a type of this
  translation unit, a call of a function of it with a written return type,
  or a name or member of known type. A range `for` parameter has the
  element type of a standard container, `std::vector<T>` and the like, and
  a destructuring binding the element of a `std::pair` or `std::tuple`. A
  name of any other deduced type, such as `n := args.ssize()`, is not
  followed.
- A move is an explicit `move x` argument. Cpp2's implicit moves at a
  definite last use need no check.
- `x = value` and an `out x` argument make `x` usable again.
- At a join of paths, a name moved on every incoming path is moved, and on
  some path maybe moved.
- `return`, `break`, `continue` and `throw` end a path, so a move followed
  by `return` does not affect code after the `if`. Returning, by `return`
  or the end of the body, reads the named return values.
- The alternatives of an `inspect` are separate paths, like the branches
  of an `if`.
- A `try` handler starts from the join of the state before the `try` and
  the state after each statement of the try block. After the `try`
  statement, the state is the join of the end of the try block and of each
  handler.
- Under [`try-operator`](try-operator.md), the operand of a `?` is
  evaluated before the rest of its statement, and the function may return
  there: the defers run and the named return values are read on that path.
- A loop is analysed again from the join of the state entering it and the
  state at the end of its body. After the loop, a name moved inside it may
  have been moved.
- A [`defer`](defer.md) body runs at the end of its block, so it sees the
  moves made after the `defer` statement.
- Each move is reported at most once, at the first mention after it. For
  a move in a loop that is the `move-in-loop` at the next iteration, so a
  later mention after the loop is not reported again.
- A name moved and mentioned again in the same statement, `take(move y, y)`,
  is `use-after-move`.

Diagnostics:

- `use-after-move`: a mention after a move on every path, or in the same
  statement as the move.
- `use-after-maybe-move`: a mention after a move on some path.
- `move-in-loop`: a name declared outside a loop is moved inside it and
  used, without being assigned again, on the next iteration.

[`linear`](linear.md) uses the same analysis for `@linear` values, with or
without this experiment.

Full entry: [`docs/EXPERIMENTS.md`](../EXPERIMENTS.md#current-experiments).

Tests: [`use_after_move.cpp2`](../../tests/ports/experiments/use_after_move.cpp2),
[`use_after_move_error.cpp2`](../../tests/ports/experiments/use_after_move_error.cpp2),
[`experiment_use_after_move_test.cpp`](../../tests/unit/experiment_use_after_move_test.cpp).
