# `defer`

Enable with `-experiment=defer`.
Explores: a new feature.

## What it enables

`defer statement;` or `defer { ... }` inside a function body registers code
that runs when the enclosing block exits: by falling off its end, by
`return`, `break` or `continue`, or by an exception. Several defers in one
block run in reverse order of registration, so cleanup mirrors setup.

The body is not run where it is written. It lowers to a `cpp2::finally`
scope guard that holds a `[&]` lambda, so it sees the variables in scope at
the `defer` by reference, with whatever values they have when the block
exits.

## Why it is useful

Cleanup that must happen on every exit path is otherwise written once per
`return` and once more in a `catch`, or wrapped in a one-off RAII type. Both
are easy to get wrong when an early return is added later. A `defer` puts
the cleanup next to the setup it undoes and covers every path out of the
block, including exceptions.

The cost is one guard object per `defer`. The guard's destructor is
`noexcept`, so an exception that escapes a defer body calls
`std::terminate`, as it would from any destructor.

## Prior art

- **Swift's `defer` statement** is the closest model: it runs when the
  enclosing scope exits, defers run in reverse order, and the body may not
  `return`, `break` or throw out of itself. This experiment has the same
  scope and ordering rules.
- **Go's `defer` statement** runs at function return rather than block exit,
  and evaluates the call's arguments when the `defer` executes. Here the
  body runs at block exit and reads variables when it runs.
- **Zig's `defer`** is block-scoped like this one; Zig adds `errdefer`, which
  runs only on an error return. There is no such variant here.
- **D's `scope(exit)`** is the same idea, alongside `scope(success)` and
  `scope(failure)`.
- **GSL's `gsl::finally`** and `scope_exit` from P0052 (Library
  Fundamentals TS v3) are the library form. The experiment lowers to
  `cpp2::finally`, the Cpp2 runtime's version of `gsl::finally`; it adds
  the syntax and the checks on the body.

## Example

```cpp
#include <iostream>
#include <stdexcept>
#include <vector>

sum_until_zero: (items: std::vector<int>) -> int = {
    std::cout << "open\n";
    defer std::cout << "close\n";
    total := 0;
    for items do (item) {
        defer { std::cout << "  seen " << item << "\n"; }
        if item < 0 { throw(std::invalid_argument("negative item")); }
        if item == 0 { return total; }
        total += item;
    }
    return total;
}

main: () = {
    std::cout << sum_until_zero(:std::vector = (1, 2, 0, 4)) << "\n";
    try { _ = sum_until_zero(:std::vector = (5, -1)); }
    catch (e: std::exception) { std::cout << "error: " << e.what() << "\n"; }
}
```

Each `defer` becomes a guard named after its source position:

```cpp
[[nodiscard]] auto sum_until_zero(cpp2::impl::in<std::vector<int>> items) -> int  {
    std::cout << "open\n";
    auto __cpp2_defer_7_5 = cpp2::finally([&]{
        std::cout << "close\n";
    });
    auto total {0};
    for (auto const& item : items) {
        auto __cpp2_defer_10_9 = cpp2::finally([&]{
            std::cout << "  seen " << item << "\n";
        });
        // ...
        if (item == 0) {
            return total;
        }
        total += item;
    }
    return total;
}
```

Output. The loop's defer runs once per iteration, including the one that
returns and the one that throws, and the function's defer runs last:

```
open
  seen 1
  seen 2
  seen 0
close
3
open
  seen 5
  seen -1
close
error: negative item
```

A defer that reads the returned name sees it intact, because the `return`
copies instead of moving:

```cpp
label: (n: int) -> std::string = {
    s: std::string = "item";
    defer std::cout << "built '" << s << "'\n";
    s += std::to_string(n);
    return s;
}

main: () = {
    std::cout << label(7) << "\n";
}
```

```cpp
    auto __cpp2_defer_6_5 = cpp2::finally([&]{
        std::cout << "built '" << s << "'\n";
    });
    s += std::to_string(n);
    return std::as_const(s);
```

```
built 'item7'
item7
```

Rejected: a defer body that leaves itself, and a `defer` outside a
function.

```cpp
first_line: (path: std::string) -> std::string = {
    defer { if path.empty() { return ""; } }
    return path;
}
defer std::cout << "x";
```

```
bad.cpp2:2:31: error: a defer body cannot return, break or continue out of the deferred statement [defer-control-flow]
bad.cpp2:5:1: error: a defer statement must be inside a function body [defer-scope]
```

## Grammar

```
statement:
    ...
    defer-statement            // added

defer-statement:
    'defer' statement
```

`defer-statement` hooks into `statement` in the block parser
(`src/statement_parser.cpp`, `parse_defer`). When the next token is `{`,
the body is a `compound-statement`; otherwise it is one statement, ended by
its own `;`.

`defer` is a contextual keyword, not a lexer keyword. It is still lexed as
an identifier, and it begins a defer statement only at the start of a
statement while the experiment is enabled. Outside a function body such a
statement is diagnosed; the line classifier (`src/source.cpp`) treats a
file-scope line beginning `defer` as Cpp2 so that it is. Elsewhere in
an expression it remains an ordinary name. Without the experiment, `defer`
is always an identifier.

## Rules and diagnostics

- The body runs when the enclosing block exits by any path, in reverse
  order of registration. Inner blocks, including loop bodies, run their
  own defers when they exit.
- The body can mention the names in scope at the `defer`, by reference.
  Names declared after the `defer` are not in scope in the body.
- The body may contain loops whose `break` and `continue` stay inside it,
  but it may not leave itself.
- A name the body mentions has no definite last use in the defer's block,
  so it is not moved from there; the deferred code still reads it after
  the block's last statement.
- The safety analyses (definite initialization, and `use-after-move` and
  `linear` when enabled) analyze the body where a path leaves the block:
  at its end and at each `return`, `break` and `continue`, in reverse
  order of registration. Only these normal exits are checked: a path that
  leaves by an exception, from a `throw` or from any statement that throws,
  is not analyzed. So a defer body may read a deferred local that a later
  statement initializes, and if an earlier statement throws, the read
  finds it uninitialized and fails `deferred_init`'s contract check at run
  time:

  ```cpp
  f: () = {
      x: int;
      defer std::cout << x;
      may_throw();    // if this throws, the defer reads `x` uninitialized
      x = 1;
  }
  ```
- `return s;` of a name that a defer registered earlier in an enclosing
  block mentions lowers to `return std::as_const(s);`, and named return
  values are copied rather than moved. The defer then sees `s` intact, and
  the result is the value at the `return`. A move-only `s` that a defer
  mentions therefore cannot be returned. Falling off the end of the body
  returns there too, so a named return value must be initialized before
  the defers run: `f: () -> (r: int) = { defer { r = 1; } }` is
  `use-before-initialization`.
- In a function with a defer, postconditions are checked after the defers
  have run, on every exit path.
- A `?` (experiment [`try-operator`](try-operator.md)) in a defer body is
  `try-operator-context`: the body runs inside a scope guard, where it
  cannot return.

Diagnostics:

- `defer-control-flow`: the body contains a `return`, or a `break` or
  `continue` whose loop is outside the body.
- `defer-scope`: a `defer` at file, namespace or type scope, or as a
  function's whole body (`k: () = defer m();`).
- `defer-name`: a local declared as `defer: T = ...` or `defer := ...` while
  the experiment is enabled. A namespace-scope or member `defer` can still
  be declared and used, and without the experiment so can a local.
- `expected-body`: `defer` with no statement after it.

Full entry: [`docs/EXPERIMENTS.md`](../EXPERIMENTS.md#current-experiments).

Tests: [`defer.cpp2`](../../tests/ports/experiments/defer.cpp2),
[`experiment_defer_test.cpp`](../../tests/unit/experiment_defer_test.cpp).
