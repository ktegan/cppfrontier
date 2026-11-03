# `type-operators`

Enable with `-experiment=type-operators`.
Explores: a new feature.

## What it enables

In a type position, `A | B` is a union type: a value that holds an `A` or
a `B`. It lowers to `std::variant<A, B>`. Type positions are object,
parameter and return types, type alias targets and typed values, so
`x: int | std::string`, `Cell: type == i64 | std::string;` and
`:int | double = 1` all work.

A template argument can be a type or a value, and Cpp2 tells them apart
without name lookup, so in a template argument `|` and `&` keep their Cpp1
meaning: `std::bitset<A | B>` and `std::array<int, N & 6>` are values. The
`type` marker, which Cpp2 already uses to say that a dependent name is a
type, makes the argument a union type: `std::vector<type int | std::string>`.
An unmarked argument with an operand that can only be a type, such as `int`
or `*T`, is `type-operator-argument`.

In a constraint, `&` and `|` combine concepts. `T: std::integral & small`
on a template parameter, or `x: _ is std::floating_point | std::integral`
on a parameter or local, requires both or either. Cpp1 has no
`A & B auto`, so the parameter becomes `typename T` (or `auto`) and the
check moves into a `requires` clause: `requires (std::integral<T> && small<T>)`.

## Why it is useful

`std::variant<int, std::string>` is the right type for "one of these", but
its spelling is long, and nested variants or repeated alternatives are easy
to write by accident. `int | std::string` reads as what it means, and the
translator flattens `(A | B) | C` and drops duplicates.

For constraints, Cpp2 lets a template parameter or `_ is` name one concept.
Two concepts need a separate `requires` clause, which moves part of the
parameter's description away from the parameter. `T: A & B` keeps it in
one place.

`&` is not a type operator: an intersection of types has no Cpp1 lowering,
so `x: int & std::string` is rejected. Operators in expressions, as in
`x is (A | B)` or an `as` target, keep their usual meaning.

## Prior art

- **TypeScript** and **Scala 3** spell union and intersection types `A | B`
  and `A & B`. Their unions are untagged and their intersections are
  types; here a union is a tagged `std::variant`, and `&` exists only for
  constraints.
- **Python's `int | str`** (PEP 604) is the same spelling for a union in a
  type annotation.
- **Rust's trait bounds** `T: A + B` combine constraints on a parameter, as
  `T: A & B` does here; Rust has no disjunction of bounds.
- **cppfront's `@union`** metafunction generates a tagged union type from a
  type body. A union type here needs no declaration and is interchangeable
  with any `std::variant` of the same alternatives.

## Example

```cpp
#include <concepts>
#include <iostream>
#include <string>
#include <vector>

Cell: type == i64 | double | std::string;

parse_cell: (text: std::string) -> Cell = {
    if !text.empty() && text.find_first_not_of("-0123456789") == text.npos { return std::stoll(text); }
    if !text.empty() && text.find_first_not_of("-0123456789.") == text.npos { return std::stod(text); }
    return text;
}

Number: <T> concept = std::integral<T> || std::floating_point<T>;

total: <T: Number & std::copyable> (values: std::vector<T>) -> T = {
    sum: T = 0;
    for values do (v) { sum += v; }
    return sum;
}

main: () = {
    counts: std::vector<i64> = ();
    weights: std::vector<double> = ();
    for (:std::vector<std::string> = ("12", "2.5", "n/a", "-4", "0.25")) do (text) {
        cell := parse_cell(text);
        if cell is i64 { counts.push_back(cell as i64); }
        else if cell is double { weights.push_back(cell as double); }
        else { std::cout << "skip '" << (cell as std::string) << "'\n"; }
    }
    std::cout << "counts " << total(counts) << ", weights " << total(weights) << "\n";
}
```

The alias names a `std::variant`, and the conjunction becomes a `requires`
clause on an unconstrained `typename T`:

```cpp
template<typename T> [[nodiscard]] auto total(std::vector<T> const& values) -> T requires (Number<T> && std::copyable<T>);
// ...
using Cell = std::variant<std::int64_t, double, std::string>;
```

Output:

```
skip 'n/a'
counts 8, weights 2.75
```

Rejected: an intersection of types.

```cpp
main: () = {
    x: int & std::string = 1;
}
```

```
e.cpp2:2:8: error: '&' combines constraints, not types: an intersection of types has no Cpp1 lowering, so it is allowed only in a template parameter or '_ is' constraint [type-operator-constraint]
```

## Grammar

With the experiment, `type-id` gains two binary operators, `|` binding
looser than `&`, and a parenthesized operand:

```
type-id:
    union-type-id                                         // changed

union-type-id:
    intersection-type-id
    union-type-id '|' intersection-type-id                // added

intersection-type-id:
    type-operand
    intersection-type-id '&' type-operand                 // added

type-operand:
    type-qualifier-seq? 'type_of' '(' expression ')' is-type-constraint?
    type-qualifier-seq? 'decltype' '(' expression ')' is-type-constraint?
    type-qualifier-seq? qualified-id is-type-constraint?
    type-qualifier-seq? unqualified-id is-type-constraint?
    type-qualifier-seq? function-type is-type-constraint?
    type-qualifier-seq? '(' type-id ')'                   // added
```

These are the old `type-id` alternatives, now operands. A
`type-qualifier-seq` applies to one operand, so `*int | std::string` is
`std::variant<int*, std::string>`, while `*(int | std::string)` points to
a variant. The `type-id` of an `is-type-constraint` (`_ is A | B`) and of
a template parameter (`T: A & B`) is a constraint, where both operators are
allowed.

The type model parses the operators (`src/type_model.cpp`, `combination`),
and the parser finds where a type ends with `type_end`
(`src/parser_context.cpp`), which with the experiment continues past a `|`
or `&` and past a `(` that follows one. There is no lexing change.

## Rules and diagnostics

- A union lowers to `std::variant` of its alternatives in order. A written
  nested union is flattened, alternatives spelled alike in Cpp1 (`i32` and
  `int`) are kept once, and a union left with one alternative is that type.
  A union named by an alias stays one alternative of an enclosing union.
- `is`, `as` and `inspect` work on a union as on any `std::variant`.
- On a template parameter, `T: A & B` becomes `typename T` with
  `requires (A<T> && B<T>)`, joined by `&&` with a written `requires`
  clause; `|` gives `||`. A pack checks each element with a fold.
- On a function parameter, `x: _ is A | B` becomes `auto` with the check
  on `CPP2_TYPEOF(x)` in the `requires` clause. Where there is no
  `requires` clause to extend, the constraint is kept as a `static_assert`
  on `CPP2_TYPEOF(x)`: after the declaration of a local or of a
  namespace-scope constant (`m: _ is A | B == v;`), and as the first statement
  of the body for a `for` loop parameter. A single-concept loop parameter,
  `(x: _ is std::integral)`, stays Cpp1 `std::integral auto const& x`.
- Without the experiment `|` or `&` between two types or constraints, where
  the experiment would read a type operator, is the error
  `type-operator-experiment`, which names the experiment; `int & long` is
  one too. A template argument's `|` and `&` stay Cpp1 operators.

Diagnostics:

- `type-operator-single`: `|` or `&` with an operand missing, as in
  `x: int | = ...`. Outside a constraint, a `&` that ends a type, as in
  `x: int& = ...`, is the default `cpp1-reference-type` instead.
- `type-operator-constraint`: `&` in a type position.
- `constraint-not-concept`: an operand of a constraint that is not a
  concept.

Full entry: [`docs/EXPERIMENTS.md`](../EXPERIMENTS.md#current-experiments).

Tests: [`type_operators.cpp2`](../../tests/ports/experiments/type_operators.cpp2),
[`type_operators_error.cpp2`](../../tests/ports/experiments/type_operators_error.cpp2),
[`experiment_type_operators_test.cpp`](../../tests/unit/experiment_type_operators_test.cpp).
