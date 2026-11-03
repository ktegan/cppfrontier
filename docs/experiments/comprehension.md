# `comprehension`

Enable with `-experiment=comprehension`.
Explores: a new feature.

## What it enables

`for range do (x) yield value` is an expression: a for-comprehension. Over
a range it is a lazy view of `value` for each element `x`; an optional
`if guard` before `yield` keeps only the elements for which the guard
holds. In place of `yield value`, another `for ...` nests a generator, and
the result is flattened.

The same syntax works on `std::optional` and `std::expected`, where it is
a monadic chain: `for a do (x) for b do (y) yield x + y` is a value only if
both `a` and `b` have one.

The comprehension lowers to `cpp2::comprehend(range, [guard,] value)`, or
`cpp2::comprehend_bind` for a nested generator, with the guard and value as
lambdas over `auto const& x`. `runtime/cpp2comprehension.h`, included when
a file uses one, turns a range into `std::views::filter | transform`
(`| join` when nested) and an optional or expected into `.and_then` and
`.transform` calls.

## Why it is useful

Building a filtered, transformed view in Cpp2 today means spelling out the
range adaptors and two lambdas, and chaining optionals means nested
`and_then` calls with a lambda per step. Both bury a one-line idea in
punctuation. A comprehension states the element, the filter and the result
in the order they are read, and reuses the `for ... do (x)` shape of the
Cpp2 `for` loop.

The result over a range is a view, not a container: it is computed when
iterated, and it refers to the range and to the locals its lambdas
mention. A view that outlives them dangles, as any Cpp1 view would. To get
a container, pass it to `std::ranges::to` or a constructor.

## Prior art

- **Scala's for-comprehensions**, `for (x <- xs if p(x)) yield f(x)`, are the
  closest model: the same keywords, guards and nesting, desugared to
  `withFilter`, `map` and `flatMap`, so they work for collections and for
  `Option` alike. Here the desugaring targets C++ ranges and the C++23
  monadic members of `std::optional` and `std::expected` (P0798).
- **Haskell's list comprehensions** and `do` notation over `Maybe` cover
  the same two uses with two syntaxes.
- **Python's generator expressions**, `(f(x) for x in xs if p(x))`, are
  lazy like a range comprehension here, but put the value first.

## Example

```cpp
#include <iostream>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

digit: (c: char) -> std::optional<int> = {
    if c >= '0' && c <= '9' { return c - '0'; }
    return std::nullopt;
}

add_digits: (a: char, b: char) -> std::optional<int> =
    for digit(a) do (x) for digit(b) do (y) yield x + y;

main: () = {
    xs: std::vector<int> = (1, 2, 3, 4, 5, 6);
    even_squares := for xs do (x) if x % 2 == 0 yield x * x;
    for even_squares do (v) { std::cout << v << " "; }
    std::cout << "\n";

    for (for 1 ..= 3 do (i) for i ..= 3 do (j) yield std::pair(i, j)) do (p) {
        std::cout << "(" << p.first << "," << p.second << ") ";
    }
    std::cout << "\n";

    std::cout << add_digits('4', '5').value_or(-1) << " " << add_digits('4', 'x').value_or(-1) << "\n";
}
```

The inner lambda of a nested generator copies each outer bound name it uses (`[&, x]`,
`[&, i]`), since a lazy view calls it after the outer lambda has returned:

```cpp
[[nodiscard]] auto add_digits(cpp2::impl::in<char> a, cpp2::impl::in<char> b) -> std::optional<int> { return cpp2::comprehend_bind(digit(a), [&](auto const& x) { return cpp2::comprehend(digit(b), [&, x](auto const& y) { return x + y; }); }); }
// ...
    auto even_squares {cpp2::comprehend(xs, [&](auto const& x) { return x % CPP2_ASSERT_NOT_ZERO_LITERAL(CPP2_TYPEOF(x),2) == 0; }, [&](auto const& x) { return x * x; })};
// ...
    for (auto const& p : (cpp2::comprehend_bind(cpp2::range(1,3,true), [&](auto const& i) { return cpp2::comprehend(cpp2::range(i,3,true), [&, i](auto const& j) { return std::pair(i, j); }); }))) {
```

Output:

```
4 16 36 
(1,1) (1,2) (1,3) (2,2) (2,3) (3,3) 
9 -1
```

Rejected: a comprehension without `yield`.

```cpp
    ys := for xs do (x) x * 2;
```

```
r1.cpp2:3:11: error: a for-comprehension is 'for range do (name) [if guard] yield value' - 'yield' is missing [comprehension-yield]
```

## Grammar

```
primary-expression:
    ...
    comprehension-expression           // added

comprehension-expression:
    'for' expression 'do' '(' identifier ')' comprehension-guard? comprehension-result

comprehension-guard:
    'if' expression

comprehension-result:
    'yield' expression
    comprehension-expression
```

The hook is `primary` in `src/expression_parser.cpp`: with the experiment,
`for` at the start of a primary expression begins a comprehension. The
range ends at the top-level `do`, the guard at the next `for` or `yield`,
and the `yield` value extends as far as an assignment expression does, so a
comprehension used as an operand is written in parentheses. The bound name
is one identifier, with no type or passing style.

Statements change in one place (`src/statement_parser.cpp`). `x := for ...`
is an initializer, where by default `name :` before `for` would be a loop
label. A loop header always skips parenthesized and bracketed text when it
looks for its `do`, so `for (for xs do (x) yield x) do (y) ...` iterates a
comprehension.

`for` is already a keyword. `yield` is a contextual keyword: it has its
meaning only after a comprehension header, and `yield := 1;` still declares
a variable.

## Rules and diagnostics

- The guard and value lambdas capture by reference, and capture nothing
  outside a function. Names bound by enclosing generators are captured by
  copy.
- The bound name hides an outer name of the same spelling inside the
  comprehension. It is not a mention of the outer name for the safety
  analyses, and it is rendered unchanged.
- Nothing inside a comprehension is moved from at a last use, since the
  view may run later.
- A range source is held as `std::views::all` holds it, or referred to when
  it is an lvalue that cannot be held that way, such as a comprehension
  over a temporary container passed as an `in` parameter. A guarded
  comprehension over a range is a `filter_view`, which Cpp1 cannot iterate
  through a `const` reference.
- The element of an optional or expected source is passed to the guard
  and the value without being copied, so a move-only or
  non-default-constructible element works.
- A `?` (experiment `try-operator`) in the guard or value is
  `try-operator-context`, since they run inside lambdas.
- An `if` guard needs an optional source. On a `std::expected` the guard has
  no empty value to produce, which is a `static_assert` in the runtime
  header: "a for-comprehension 'if' guard needs an optional source".
- Without the experiment, `for` inside an expression is a syntax error,
  `statement-as-initializer` ("'for' begins a statement, not an
  expression"), as in `ys := for xs do (x) yield x * 2;` or
  `g(for xs do (x) yield x)`.

Diagnostics:

- `comprehension-yield`: a `for` in an expression that is not a complete
  comprehension, usually because `yield` is missing.

Full entry: [`docs/EXPERIMENTS.md`](../EXPERIMENTS.md#current-experiments).

Tests: [`comprehension.cpp2`](../../tests/ports/experiments/comprehension.cpp2),
[`comprehension_move_only.cpp2`](../../tests/ports/experiments/comprehension_move_only.cpp2),
[`experiment_comprehension_test.cpp`](../../tests/unit/experiment_comprehension_test.cpp).
