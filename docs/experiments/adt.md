# `adt`

Enable with `-experiment=adt`.
Explores: a new feature.

## What it enables

The metafunction `@adt` declares a sum type, a value that is exactly one of
several cases, each with its own fields. A case is written as a bodyless
function whose parameters are the fields:

```cpp
Event: @adt type = {
    Key: (code: int);
    Click: (x: int, y: int);
    Quit: ();
}
```

Each case becomes a nested `@struct`, `Event::Click`, with those fields
and `==` (and `<=>` when the type is ordered, as a `@value` is), built as
`Event::Click(10, 20)`. An `Event` holds one case in a
`std::variant`, converts implicitly from each case, and has `is_Click()`
and `as_Click()` accessors. `is`, `as` and `inspect` see through it to the
case, so `e is Event::Quit` and `inspect e { is Event::Key = ...; }` work
unchanged. The type may also declare ordinary member functions.

## Why it is useful

The same type written by hand is a struct per case, a `std::variant`
member, a constructor and an accessor pair per case, `==`, and `is`/`as`
support for `inspect`. `@adt` writes all of it from one line per case, and
the cases cannot drift out of sync with the variant.

With the [`patterns`](patterns.md) experiment, an `inspect` of an `@adt`
can bind a case's fields, `is Event::Click(x, y)`, and an `inspect` that
names every case needs no `is _`; a missing case is reported at translation
time.

Copy and move are left to Cpp1 unless the type asks for them, so a case
may hold a move-only field, including a `std::unique_ptr` to the type
itself, which is how a recursive type such as an expression tree is
written. The cost is that of `std::variant`: the size of the largest case
plus a tag.

## Prior art

- **ML and Haskell algebraic data types** and **Rust's `enum` with fields**
  are the model: named cases with fields, constructed by case name and taken
  apart by pattern matching. Here the cases are also ordinary nested types
  with names of their own.
- **`std::variant`** is the Cpp1 library form and the lowering. It has no
  case names, only types, and `std::visit` instead of a `match`.
- **cppfront's `@union` metafunction** generates a tagged union whose
  alternatives are single data members, with `is_name()`, `name()` and
  `set_name()` functions. `@adt` cases may have several fields, are types
  of their own, and work with `is`, `as` and `inspect`.

## Example

```cpp
#include <iostream>
#include <string>
#include <vector>

Event: @adt type = {
    Key: (code: int);
    Click: (x: int, y: int);
    Quit: ();
}

describe: (e: Event) -> std::string = inspect e -> std::string {
    is Event::Key = "key " + std::to_string((e as Event::Key).code);
    is Event::Click = "click at " + std::to_string((e as Event::Click).x);
    is _ = "quit";
};

main: () = {
    events: std::vector<Event> = (Event::Key(65), Event::Click(10, 20), Event::Quit());
    for events do (e) { std::cout << describe(e) << "\n"; }
    std::cout << events[1].is_Click() << (events[2] is Event::Quit)
              << (events[0] == Event(Event::Key(65))) << "\n";
}
```

The relevant part of the Cpp1. The runtime's `is` and `as` overloads for
any type with `cpp2_adt_value()` see through to the case:

```cpp
struct Event {
struct Key { int code;
[[nodiscard]] auto operator==(Key const& that) const& -> bool = default;
};
struct Click { int x; int y;
[[nodiscard]] auto operator==(Click const& that) const& -> bool = default;
};
// ...
}; private: std::variant<Key, Click, Quit> _value;
public: Event(Key _case);
[[nodiscard]] auto is_Key() const& -> bool;
[[nodiscard]] auto as_Key() const& -> Key const&;
[[nodiscard]] auto as_Key()& -> Key&;
// ...
[[nodiscard]] auto operator==(Event const& that) const& -> bool = default;
[[nodiscard]] auto cpp2_adt_value() const& -> std::variant<Key, Click, Quit> const&;
[[nodiscard]] auto cpp2_adt_value()& -> std::variant<Key, Click, Quit>&;
[[nodiscard]] auto cpp2_adt_value()&& -> auto&&;
};
```

Output:

```
key 65
click at 10
quit
111
```

With `-experiment=adt -experiment=patterns`, the same function binds the
fields and covers every case, so it needs no `is _`:

```cpp
describe: (e: Event) -> std::string = inspect e -> std::string {
    is Event::Key(c) = "key " + std::to_string(c);
    is Event::Click(x, y) = "click " + std::to_string(x) + "," + std::to_string(y);
    is Event::Quit = "quit";
};
```

`describe(Event::Click(10, 20))` returns `click 10,20`. Leaving out
`Event::Click` there is rejected:

```
event3.cpp2:7:39: error: inspect of 'Event' does not cover Event::Click - add alternatives or 'is _' [inspect-not-exhaustive]
```

A recursive expression tree, with `-experiment=adt -experiment=patterns`.
The cases hold `std::unique_ptr<Expr>`, so `Expr` is move-only:

```cpp
#include <iostream>
#include <memory>

Expr: @adt type = {
    Num: (value: int);
    Neg: (operand: std::unique_ptr<Expr>);
    Add: (left: std::unique_ptr<Expr>, right: std::unique_ptr<Expr>);
}

eval: (e: Expr) -> int = inspect e -> int {
    is Expr::Num(v) = v;
    is Expr::Neg(x) = -eval(x*);
    is Expr::Add(l, r) = eval(l*) + eval(r*);
};

num: (v: int) -> std::unique_ptr<Expr> = std::make_unique<Expr>(Expr::Num(v));

main: () = {
    e: Expr = Expr::Add(num(2), std::make_unique<Expr>(Expr::Neg(num(5))));
    std::cout << eval(e) << "\n";
}
```

Output:

```
-3
```

Rejected: a duplicate case, a field with a passing style, and a data
member.

```cpp
Shape: @adt type = {
    Circle: (r: double);
    Circle: (d: double);
    Poly: (copy n: int);
    size: int = 0;
}
```

```
bad.cpp2:3:5: error: 'Circle' is already a case of Shape [adt-duplicate-case]
bad.cpp2:4:5: error: an @adt case is a list of typed fields: 'Poly: (name: type, ...);' [adt-case-form]
bad.cpp2:5:5: error: an @adt case is a list of typed fields: 'size: (name: type, ...);' [adt-case-form]
```

## Rules and diagnostics

### Accepted form

```
adt-body:
    ( adt-case | member-function )*      // at least one adt-case

adt-case:
    identifier ':' '(' ( field ( ',' field )* )? ')' ';'

field:
    identifier ':' type-id
```

A case is a function member with no `this`, no body and no return type.

### Rules

- Each field is a plain `in` parameter with a declared type, no default and
  a name other than `_`. Cases are not templates and have no contracts.
- Case names are unique within the type, and there is at least one.
- A data member is not allowed; the state is the active case.
- Each case is a nested `@struct` with the fields and `==`, and `<=>` in
  an ordered type. The type converts implicitly from each case and has
  `is_Case()`, `as_Case()` (const and non-const) and `==`.
- Copy and move are Cpp1's, so they exist exactly when every field has
  them, unless the type declares `operator=: (out this, that)` or is
  `@copyable` or a value type. A `@value` default-constructs to its first
  case.
- `is` and `as` with a case type test and extract the active case; `as`
  on an rvalue, `(move e) as E::Case`, moves the case out. `as` any other
  type is the general conversion. `as_Case()` on a value holding another
  case throws `std::bad_variant_access`, as `std::get` does.
- A member may not be named `_...`, like a case's accessor (`is_Case`,
  `as_Case`), `cpp2_adt_value`, or the type itself
  (`metafunction-reserved-name`).

Diagnostics:

- `adt-case-form`: a case that is not a list of typed `in` fields, or a
  data member.
- `adt-duplicate-case`: a case name used twice.
- `adt-empty`: no cases.
- `metafunction-reserved-name`: a member named as above.
- `inspect-not-exhaustive` (with `patterns`): an `inspect` without `is _`
  that does not cover every case.

Full entry: [`docs/EXPERIMENTS.md`](../EXPERIMENTS.md#current-experiments).

Tests: [`adt.cpp2`](../../tests/ports/experiments/adt.cpp2),
[`adt_as.cpp2`](../../tests/ports/experiments/adt_as.cpp2),
[`adt_move.cpp2`](../../tests/ports/experiments/adt_move.cpp2),
[`patterns.cpp2`](../../tests/ports/experiments/patterns.cpp2),
[`experiment_adt_test.cpp`](../../tests/unit/experiment_adt_test.cpp).
