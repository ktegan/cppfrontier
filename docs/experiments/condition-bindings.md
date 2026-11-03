# `condition-bindings`

Enable with `-experiment=condition-bindings` (enables [`patterns`](patterns.md)).
Explores: a new feature.

## What it enables

An operand of the top-level `&&` chain of an `if`, `else if` or `while`
condition may be a pattern test that binds names, with the binding forms
of a [`patterns`](patterns.md) alternative:

- `v is T as n` names `v as T`;
- `v is T(a, b)` binds the elements of `v as T`, and `_` skips one;
- `v is (a, b)` binds the elements of `v` itself, and always holds.

The names are in scope in the later operands of the chain and in the body
the condition guards, and nowhere else: not in the `else`, not in a
`while`'s `next` clause, and not after the statement. An `is` that binds
nothing stays the ordinary `bool` expression.

## Why it is useful

Default Cpp2 tests a variant, an optional or an `@adt` case with `is` and
then repeats the conversion with `as` in the body, where nothing ties the
two together. An `inspect` keeps them together but is an expression with a
required wildcard, which is heavy for one case. A binding in the condition
says "if this is a `T`, call it `n`" once, and the name only exists where
the test held.

## Prior art

- **C#** `if (o is int i && i > 0)` and **Java 16** `o instanceof String s`
  bind in a condition, visible where the test is known to hold.
- **Rust** `if let Some(x) = opt` and `while let`, with let-chains
  (`if let A(x) = a && x > 0`, stable in Rust 1.88 for edition 2024),
  are the model for the `&&` chain rule.
- **Swift** `if case .circle(let r) = shape` and `if let x = opt` bind the
  same way, also for `while`.

## Example

```cpp
#include <iostream>
#include <optional>
#include <queue>
#include <string>
#include <variant>

Shape: @adt type = {
    Circle: (r: double);
    Rect: (w: double, h: double);
    Empty: ();
}

area: (s: Shape) -> double = {
    if s is Shape::Circle(r) { return 3.0 * r * r; }
    else if s is Shape::Rect(w, h) && w > 0.0 { return w * h; }
    return 0.0;
}

describe: (v: std::variant<int, std::string>) -> std::string = {
    if v is int as n && n > 9 { return "big " + std::to_string(n); }
    else if v is int as n { return "small " + std::to_string(n); }
    else if v is std::string as s { return "text " + s; }
    return "?";
}

main: () = {
    std::cout << area(Shape::Circle(1.0)) << " " << area(Shape::Rect(2.0, 3.0)) << "\n";
    std::cout << describe(42) << ", " << describe(3) << ", " << describe("x") << "\n";
    queue: std::queue<std::optional<int>> = ();
    for (1, 2, 3) do (i) { queue.push(i); }
    queue.push(std::nullopt);
    sum := 0;
    while !queue.empty() && queue.front() is int as x next queue.pop() { sum += x; }
    std::cout << sum << "\n";
}
```

(with `-experiment=condition-bindings -experiment=adt`)

Output:

```
3 6
big 42, small 3, text x
6
```

## Lowering

The subject of each pattern test is evaluated once, into
`auto&& __cpp2_subject_L_C_k`, and each binding is a reference declared in
the branch where the test held: `auto&& n = cpp2::impl::as<int>(subject);`
or `auto&& [w, h] = cpp2::impl::as<Shape::Rect>(subject);`. A chain becomes
nested `if`s, consecutive plain operands joined by `&&`:

```cpp
if (auto&& __cpp2_subject_15_5_0 = s; cpp2::impl::is<Shape::Rect>(__cpp2_subject_15_5_0)) {
    [[maybe_unused]] auto&& [w, h] = cpp2::impl::as<Shape::Rect>(__cpp2_subject_15_5_0);
    if (cpp2::impl::cmp_greater(w,0.0)) {
        return w * h;
    }
}
```

With an `else` or an `else if`, a flag set before the body carries the
result, and the `else` runs when it is unset:

```cpp
{
    bool __cpp2_matched_14_5 = false;
    if (auto&& __cpp2_subject_14_5_0 = s; cpp2::impl::is<Shape::Circle>(__cpp2_subject_14_5_0)) {
        [[maybe_unused]] auto&& [r] = cpp2::impl::as<Shape::Circle>(__cpp2_subject_14_5_0);
        __cpp2_matched_14_5 = true;
        area = 3.0 * r * r;
    }
    if (!__cpp2_matched_14_5) {
        // the else-if statement
    }
}
```

A body that ends with `return`, `break` or `continue` needs no flag: the
`else` follows the nested `if`s in a plain block, which only a failed test
reaches, so the Cpp1 compiler sees that both branches return.

A `while` tests each operand in turn and leaves the loop when one fails;
the `next` clause becomes the `for`'s increment, so it runs on every path
that continues:

```cpp
for (;; CPP2_UFCS(pop)(queue)) {
    if (!(!CPP2_UFCS(empty)(queue))) break;
    auto&& __cpp2_subject_33_5_1 = CPP2_UFCS(front)(queue);
    if (!(cpp2::impl::is<int>(__cpp2_subject_33_5_1))) break;
    [[maybe_unused]] auto&& x = cpp2::impl::as<int>(__cpp2_subject_33_5_1);
    sum += CPP2_FORWARD(x);
}
```

## Grammar

```
condition:                               // of 'if', 'else if' and 'while'
    condition-operand
    condition '&&' condition-operand     // added

condition-operand:
    inclusive-or-expression
    condition-binding                    // added

condition-binding:
    prefix-expression 'is' type-id tuple-pattern? pattern-binding?
    prefix-expression 'is' tuple-pattern
```

`tuple-pattern` and `pattern-binding` are the `patterns` productions. A
`condition-binding` binds at least one name; one that does not is the
ordinary `is-as-expression`. There is no lexing change. The expression
parser (`pattern_test` in `src/expression_parser.cpp`) reads `as name`
after `is T` as a binding when the name is unqualified and has no template
arguments, so `v is T as U::V` stays the `mixed-is-as` error; it reads
`(..)` after `is T` and a tuple pattern after `is` as in an alternative.

## Rules and diagnostics

- Only an operand of the top-level `&&` chain binds. A pattern test under
  `||`, `!`, parentheses (including a parenthesized whole condition), the
  conditional operator or any other operator, and one in a `do ... while`
  condition, a `next` clause, an `if constexpr` condition, a `for` range or
  any other expression, is `condition-binding-position`.
- A binding is a name; `_` skips a destructured element, `as _` is
  rejected, and a condition binds each name once
  (`invalid-condition-binding`). A binding hides an outer name of the same
  spelling.
- Bindings are `auto&&` references into the subject, as in `patterns`. The
  subject is bound by reference, so its last use is not moved into the
  binding; a use of a binding counts as a use of the names the subject
  mentions, so the subject is not moved while a binding is still used. The
  definite last use of an `as` binding forwards it (`CPP2_FORWARD(n)`);
  destructured names are never moved.
- The flow analyses see a binding as initialized where it is declared and
  the guarded body as a branch, so a move in the body is a maybe-move after
  the statement.
- Without the experiment the forms are errors as before: `v is T as n` is
  `mixed-is-as`, and `v is T(a, b)` is `juxtaposed-operands`; `v is (a, b)`
  is a value test.

Diagnostics:

- `condition-binding-position`: a pattern test that binds is not an operand
  of an `if` or `while` condition's top-level `&&` chain.
- `invalid-condition-binding`: a destructuring lists something other than
  names and `_`, or no names; a binding is not a name, is `_`, or is bound
  twice.

## Limits

- A binding's type is not known to the translator (it is `auto&&`), so
  `use-after-move` and `linear` do not track moves out of a binding, and a
  binding passed to a `move` parameter needs an explicit `move`.
- An `if` with an `else` whose every branch returns still ends in Cpp1
  that `g++ -Wreturn-type` cannot see through, since the `else` is behind
  the flag.

Full entry: [`docs/EXPERIMENTS.md`](../EXPERIMENTS.md#current-experiments).

Tests: [`condition_bindings.cpp2`](../../tests/ports/experiments/condition_bindings.cpp2),
[`condition_bindings_error.cpp2`](../../tests/ports/experiments/condition_bindings_error.cpp2),
[`condition_bindings_default.cpp2`](../../tests/ports/experiments/condition_bindings_default.cpp2);
[`experiment_condition_bindings_test.cpp`](../../tests/unit/experiment_condition_bindings_test.cpp).
