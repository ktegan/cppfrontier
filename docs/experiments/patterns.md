# `patterns`

Enable with `-experiment=patterns`.
Explores: a new feature.

## What it enables

An `is` alternative of `inspect` can do more than test a type or a value:

- destructure: `is T(a, _)` binds the elements of `subject as T`, and
  `is (a, b)` binds the elements of the subject itself;
- list alternatives: `is A | B` matches either, where each part is a type
  or a value;
- bind: `is T as c` names `subject as T`;
- guard: `is T(r) if r > 1.0` matches only when the condition holds, and
  otherwise falls through to the next alternative.

With the experiment, `is _` is not always required. An `inspect` whose subject is an
[`@adt`](adt.md) or `@enum` value (or, with [`sealed`](sealed.md), a sealed
hierarchy) may instead list every case, and the translator reports a
missing one.

## Why it is useful

Default Cpp2 `inspect` can only say which type or value matched. Using the
matched value means writing `v as T` again in the result expression, and
anything conditional on it needs a nested `inspect` or an `if` outside.
Pairs and tuples cannot be taken apart at all. Guards, bindings and
destructuring cover these cases in the alternative itself.

Exhaustiveness turns a missing case into a translation error instead of a
wildcard that silently returns a default. Without `is _`, an `inspect`
that does not match at run time calls `std::terminate()`; the coverage check
makes that unreachable for sum types.

## Prior art

- **P2392** (Herb Sutter), "Pattern matching using `is` and `as`", is the
  basis of Cpp2's `inspect`. **P2688** (Michael Park), "Pattern Matching:
  `match` Expression", proposes guards, bindings and structured-binding
  patterns for C++. This experiment adds similar forms within the existing
  `is`/`as` syntax.
- **Rust's `match`** has or-patterns `A | B`, tuple patterns `(a, _)`,
  bindings `name @ pattern` and `if` guards, and rejects a `match` on an
  enum that misses a variant. The experiment's check is the same but only
  for `@adt`, `@enum` and sealed types; other subjects still need `is _`.
- **OCaml's `match`** spells the binding the same way, `pattern as name`,
  and uses `when` for guards. OCaml reports a non-exhaustive match as a
  warning; here it is an error.

## Example

```cpp
#include <iostream>
#include <string>
#include <utility>
#include <variant>

Light: @enum type = { red; amber; green; }

next: (l: Light) -> Light = inspect l -> Light {
    is (Light::red) = Light::green;
    is (Light::green) = Light::amber;
    is (Light::amber) = Light::red;
};

Value: type == std::variant<int, double, std::string>;

describe: (v: Value) -> std::string = inspect v -> std::string {
    is int as n if n < 0 = "negative";
    is int | double = "number";
    is std::string as s if s.empty() = "empty text";
    is _ = "text";
};

where: (p: std::pair<int, int>) -> std::string = inspect p -> std::string {
    is (x, y) if x == y = "on the diagonal";
    is (x, _) if x == 0 = "on the y axis";
    is (_, y) = "off the diagonal, y = " + std::to_string(y);
};

main: () = {
    std::cout << next(Light::red).to_string() << "\n";
    std::cout << describe(-3) << ", " << describe(2.5) << ", " << describe(std::string()) << "\n";
    std::cout << where(std::pair(2, 2)) << "; " << where(std::pair(0, 5)) << "; " << where(std::pair(1, 7)) << "\n";
}
```

Each alternative becomes its own `if` inside the inspect's lambda, so a
failed guard falls through. Bindings are references to `__cpp2_expr as T`,
or to the elements of a structured binding. Bindings of `as T` are made only
where that conversion yields a value, so in a template whose subject does
not convert to `T` the alternative, whose test fails, is left out. A tuple
pattern is made only where the subject's type is tuple-like
(`std::tuple_size<T>` is complete), an array or an aggregate
(`cpp2::impl::destructurable`, in `runtime/cpp2util.h`), so in a template
it is left out for any other subject:

```cpp
    if (cpp2::impl::is<int>(__cpp2_expr)) { if constexpr( requires{cpp2::impl::as<int>(__cpp2_expr);} ) if constexpr( !std::is_same_v<CPP2_TYPEOF(cpp2::impl::as<int>(__cpp2_expr)), cpp2::nonesuch_> ) { [[maybe_unused]] auto&& n = cpp2::impl::as<int>(__cpp2_expr); if (cpp2::impl::cmp_less(n,0)) { // ...
    if (cpp2::impl::is<int>(__cpp2_expr) || cpp2::impl::is<double>(__cpp2_expr)) { // ...
    // ...
    { if constexpr( cpp2::impl::destructurable<CPP2_TYPEOF(__cpp2_expr)> ) { [[maybe_unused]] auto&& [x, y] = __cpp2_expr; if (x == y) { // ...
```

`next` has no `is _`, so its chain ends in `std::terminate()`, which the
coverage check makes unreachable. So does `where`'s, since its last tuple
pattern is left out for a subject it cannot take apart:

```cpp
    if (cpp2::impl::is(__cpp2_expr, (Light::amber))) { if constexpr( requires{Light::red;} ) if constexpr( std::is_convertible_v<CPP2_TYPEOF((Light::red)),Light> ) return Light::red; else return Light{}; else return Light{}; } std::terminate();
```

Output:

```
green
negative, number, empty text
on the diagonal; on the y axis; off the diagonal, y = 7
```

Rejected: `next` without the `amber` alternative.

```
r.cpp2:3:29: error: inspect of 'Light' does not cover Light::amber - add alternatives or 'is _' [inspect-not-exhaustive]
```

Without the experiment, the same `next` and any of the new forms are errors:

```
r.cpp2:3:29: error: an inspect expression must have an `is _` match-anything wildcard alternative [invalid-inspect]
d.cpp2:15:50: error: destructuring, alternatives, bindings and guards in an inspect alternative require '-experiment=patterns' [invalid-inspect]
```

## Grammar

```
alternative:
    alt-name? is-type-constraint '=' statement
    alt-name? is-value-constraint '=' statement
    alt-name? as-type-cast '=' statement
    'is' pattern pattern-types? pattern-binding? pattern-guard? '=' statement   // added

pattern:
    type-id tuple-pattern?
    tuple-pattern
    expression

tuple-pattern:
    '(' pattern-name ',' pattern-name-seq ')'

pattern-name-seq:
    pattern-name
    pattern-name-seq ',' pattern-name

pattern-name:
    identifier
    '_'

pattern-types:
    '|' pattern-alt
    pattern-types '|' pattern-alt

pattern-alt:
    type-id
    expression

pattern-binding:
    'as' identifier

pattern-guard:
    'if' logical-or-expression
```

The productions extend `alternative` in `inspect-expression`; the parser
is `alternative` in `src/expression_parser.cpp`, which reads the extra
parts after the pattern in this fixed order. As in the default language, a pattern that
starts with `(`, a sign or a literal is a value; otherwise it is a type-id.
Each `pattern-alt` after `|` is read by the same rule. A parenthesized list of two or more plain names is a `tuple-pattern`
rather than a value. The value is still `'=' expression ';'`.

There is no lexing change: `|` and `if` are existing tokens, and `as` is
read as the binding only after a pattern.

## Rules and diagnostics

- Alternatives are tried in order. An alternative's bindings are visible
  only in its guard and value, and there they hide outer names of the same
  spelling.
- `is T(a, b)` binds a structured binding of `subject as T`; `is (a, b)`
  binds one of the subject. `_` skips an element. In a template, an
  alternative whose subject does not convert to `T`, or, for `is (a, b)`,
  whose subject is not tuple-like, an array or an aggregate, is left out.
- `is A | B` matches if either test does. Each part is a type or a value,
  read by the same rule as the first pattern, so `is (1) | (2) | 3` lists
  values. With `as c` it binds the subject itself, since there is no single
  type to convert to. A destructuring is not combined with `|`, and `_`
  takes neither `|` nor a destructuring.
- A guard that is false moves on to the next alternative. A guard may be
  parenthesized, `if (n < 0)`. An alternative after an unguarded `is _`,
  with or without `as c`, is `unreachable-alternative`, as without the
  experiment; one after `is _ if c` or a tuple pattern is not.
- Coverage: only an unguarded `_` or tuple pattern, with or without
  `as c`, matches anything; `is _ if c` and `is (a, b) if c` do not. The
  base check still requires such an alternative in every inspect.
  `check_inspect_patterns` (`src/inspect_patterns.cpp`) removes that error
  when the subject is a name declared with an `@adt` or `@enum` type, or a
  `@sealed` base type with that experiment, or `this` in a member function of one, and the unguarded alternatives
  name every case: a type case as a type, `is Event::Key`, and an
  enumerator as a value, `is (Light::red)`. `is Light::red` reads the
  enumerator as a type-id, so it does not cover it, and the
  `inspect-not-exhaustive` message suggests `is (Light::red)`. A guarded alternative does not count toward coverage. A
  `@flag_enum` has no recorded cases, since its values combine, so an
  inspect of one needs `is _`.
- Without the experiment the new forms are `invalid-inspect`.

Diagnostics:

- `inspect-not-exhaustive`: an inspect of a sum type misses cases; the
  message lists them.
- `invalid-inspect`: no unguarded `is _` or tuple pattern, and the subject
  is not an `@adt`, `@enum` or `@sealed` sum type; or, without the
  experiment, an alternative uses one of the new forms.

Full entry: [`docs/EXPERIMENTS.md`](../EXPERIMENTS.md#current-experiments).

Tests: [`patterns.cpp2`](../../tests/ports/experiments/patterns.cpp2),
and through `sealed`, [`sealed.cpp2`](../../tests/ports/experiments/sealed.cpp2)
and [`sealed_error.cpp2`](../../tests/ports/experiments/sealed_error.cpp2);
[`experiment_patterns_test.cpp`](../../tests/unit/experiment_patterns_test.cpp).
