# `let-else`

Enable with `-experiment=let-else`.
Explores: a new feature.

## What it enables

A local declaration `name := value as T else { statements }` tests
`value is T`. When the test holds, it declares `name` as `value as T` for
the rest of the scope; otherwise it runs the block, which must not complete
normally. With [`destructuring`](destructuring.md),
`(a, b) := value as T else { ... }` binds the elements of `value as T`.

## Why it is useful

Guard clauses are where most `optional`, `variant` and `@adt` checks live:
"if this is not a `T`, give up; otherwise carry on with it". Written with
`is` and `as` they repeat the test and the conversion, and written with an
`if` they nest the rest of the function. A let-else names the value once,
at the level of the code that uses it, and the translator checks that the
`else` path really leaves. The [`try-operator`](try-operator.md) covers
propagating a failure; a let-else covers doing something else: return a
default, log and `continue` a loop, or throw.

## Prior art

- **Rust** `let PATTERN = value else { diverge };` (stable since 1.65)
  requires the `else` block to diverge, checked by the type system.
- **Swift** `guard let x = optional else { return }` and
  `guard case .some(let x) = value else { ... }` require the same.
- **Kotlin** `val x = y ?: return` is the expression form.

## Example

```cpp
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

Shape: @adt type = {
    Circle: (r: double);
    Rect: (w: double, h: double);
}

width: (s: Shape) -> double = {
    (w, _) := s as Shape::Rect else { return 0.0; }
    return w;
}

number: (value: std::variant<int, std::string>) -> int = {
    n := value as int else { throw(std::invalid_argument("not a number")); }
    return n;
}

total: (values: std::vector<std::optional<int>>) -> int = {
    sum := 0;
    for values do (o) {
        n := o as int else { continue; }
        sum += n;
    }
    return sum;
}

main: () = {
    std::cout << width(Shape::Rect(2.5, 1.0)) << " " << width(Shape::Circle(1.0)) << "\n";
    std::cout << number(42) << "\n";
    values: std::vector<std::optional<int>> = (1, std::nullopt, 3);
    std::cout << total(values) << "\n";
}
```

(with `-experiment=let-else -experiment=destructuring -experiment=adt`)

Output:

```
2.5 0
42
4
```

## Lowering

The value is held in a reference, which keeps a temporary alive for the
rest of the scope; the block runs unless it is a `T`, and the names are
references to `value as T`:

```cpp
auto&& __cpp2_let_14_5 = s;
if (!cpp2::impl::is<Shape::Rect>(__cpp2_let_14_5)) {
    return 0.0;
}
[[maybe_unused]] auto&& [w, __cpp2_unused_14_9] = cpp2::impl::as<Shape::Rect>(__cpp2_let_14_5);
```

```cpp
for (auto const& o : values) {
    auto&& __cpp2_let_26_9 = o;
    if (!cpp2::impl::is<int>(__cpp2_let_26_9)) {
        continue;
    }
    auto&& n = cpp2::impl::as<int>(__cpp2_let_26_9);
    sum += CPP2_FORWARD(n);
}
```

## Grammar

```
statement:
    ...
    let-else-declaration                                         // added

let-else-declaration:
    identifier ':=' let-else-value 'else' compound-statement
    destructuring-names ':=' let-else-value 'else' compound-statement   // with destructuring

let-else-value:
    prefix-expression 'as' type-id
```

`destructuring-names` is the [`destructuring`](destructuring.md) name list
`'(' name ',' name ... ')'`. No `;` follows the block, as after every other
statement that ends in a block (`if`, `while`, `defer`); a `;` there is the
usual `empty-statement` error. There is no lexing change. The statement
parser (`parse_let_else` in `src/statement_parser.cpp`) recognizes the form
by a top-level `else {` after `:=`, parses the part before `else` as the
declaration it spells, and parses the block as a nested body.

## Rules and diagnostics

- The initializer is one `value as T`: an `as` cast at its top level.
- The block's last statement is `return`, `throw`, `break`, `continue`, or
  a call to `std::terminate`, `std::abort`, `std::exit`, `std::quick_exit`
  or `std::unreachable`, or an `if` with an `else` whose branches each end
  so. The check is syntactic: a nested block, a loop or a call to any other
  function does not count, since the translator cannot see `[[noreturn]]`.
- `name` is in scope after the statement only: not in its initializer and
  not in the block. The `defer` and `try-operator` rules apply in the block
  as in any block, so a `return` from a let-else inside a `defer` is
  `defer-control-flow`, and `?` may be used where the function can return
  early, including in the initializer.
- The names are references into the value, as condition and `inspect`
  bindings are: the value is not moved while a name is used, a definite
  last use of `name` forwards it (`CPP2_FORWARD(n)`), and destructured
  names are never moved.
- The flow analyses see the block as a path that leaves the scope: a local
  initialized only in the block is uninitialized after the statement, a
  move in the block does not reach the code after it, and a definite last
  use in the block moves.
- Without the experiment the form is an error, as before:
  `'else' is not part of an expression` (`incomplete-expression`).

Diagnostics:

- `let-else-initializer`: the initializer is not `value as T`.
- `let-else-falls-through`: the block may complete normally.

## Limits

- The test is `is T`, so a value whose type converts to `T` without being
  one (an `int` cast `as long`) runs the block.
- Only `:=` declares; `name: T = value as T else { ... }` is not a let-else.
- A name used in the block is not diagnosed by the translator; Cpp1
  reports it as undeclared.

Full entry: [`docs/EXPERIMENTS.md`](../EXPERIMENTS.md#current-experiments).

Tests: [`let_else.cpp2`](../../tests/ports/experiments/let_else.cpp2),
[`let_else_error.cpp2`](../../tests/ports/experiments/let_else_error.cpp2),
[`let_else_default.cpp2`](../../tests/ports/experiments/let_else_default.cpp2);
[`experiment_let_else_test.cpp`](../../tests/unit/experiment_let_else_test.cpp).
