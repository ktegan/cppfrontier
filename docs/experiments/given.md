# `given`

Enable with `-experiment=given`. Builds on [`trait`](trait.md), which it enables.
Explores: a new feature.

## What it enables

The metafunction `@given<Trait, Type>` implements a [`@trait`](trait.md)
for a type from outside that type. This covers types the program cannot
change, such as `int` or a library type:

```cpp
Show_int: @given<Show, int> type = {
    show: (x: int) -> std::string = "int " + std::to_string(x);
}
```

Each function of a given takes the value as its first parameter instead of
`this`, and becomes a free function in the trait's namespace; the given
type itself is left an empty marker. Since the trait accepts a free
function that UFCS reaches, `int` now satisfies `Show`: a template
constrained with `<T: Show>` accepts an `int`, `x.show()` on an `int`
calls `show(x)` where lookup finds it, and `dyn_Show` can hold an `int`.

## Why it is useful

Without a given, a trait can only be satisfied by a type that has the
functions as members. To make `int` or `std::string` satisfy one, a program
would wrap it in a new type and convert at every call. A given adds the
implementation in one place and leaves the type alone.

The trade-offs: coherence, and reach. Two givens of the same trait for the
same type in one translation unit are a Cpp1 redefinition error, but
nothing checks that two translation units agree. And a given's functions
are ordinary free functions in the trait's namespace, so a UFCS call
outside that namespace finds them only as other free functions are found
(see the rules).

## Prior art

- **Scala 3's `given` instances** with `using` clauses are the source of
  the name: a `given` supplies a type class instance for a type, and the
  compiler finds it by type. Here the given's functions are found by
  ordinary name lookup, as UFCS finds any free function, rather than by
  implicit parameter search.
- **Rust's `impl Trait for Type`** may implement a local trait for a
  foreign type such as `i32`. Rust's orphan rule requires the trait or the
  type to be local, which keeps impls coherent across crates. This
  experiment has no orphan rule; it is limited to one translation unit's
  knowledge.
- **Haskell instance declarations** add a type class instance for an
  existing type in the same way.
- **C++0x concept maps** (`concept_map`) mapped an existing type onto a
  concept from outside it, which is the closest C++ precedent.

## Example

```cpp
#include <iostream>
#include <string>
#include <vector>

Show: @trait type = {
    show: (this) -> std::string;
}

Show_int: @given<Show, int> type = {
    show: (x: int) -> std::string = "int " + std::to_string(x);
}

Show_double: @given<Show, double> type = {
    show: (d: double) -> std::string = "double " + std::to_string(d);
}

Point: @impl<Show> @value type = {
    x: int = 0;
    y: int = 0;
    operator=: (out this, x_: int, y_: int) = { x = x_; y = y_; }
    show: (this) -> std::string = "(" + std::to_string(x) + ", " + std::to_string(y) + ")";
}

print: <T: Show> (v: T) = { std::cout << v.show() << "\n"; }

main: () = {
    print(42);
    print(Point(1, 2));
    items: std::vector<dyn_Show> = (7, 2.5, Point(3, 4));
    for items do (s) { print(s); }
}
```

A given currently lowers to free functions declared after it, and an empty
marker type:

```cpp
struct Show_int;
[[nodiscard]] auto show(cpp2::impl::in<int> x) -> std::string;
// ...
struct Show_int {
};
```

Output:

```
int 42
(1, 2)
int 7
double 2.500000
(3, 4)
```

Rejected: a function whose first parameter is not the given's type, and a
given that leaves out a trait function.

```cpp
Show_long: @given<Show, long> type = {
    show: (x: int) -> std::string = std::to_string(x);
}
Show_char: @given<Show, char> type = { }
```

```
bad.cpp2:6:5: error: the first parameter of a given function must have the type the given is for, 'long' [given-instance-parameter]
bad.cpp2:8:1: error: the given of 'Show' for 'char' does not define 'show' [given-missing-function]
```

In a namespace, the given is declared beside its trait, and a UFCS call
on an `int` outside the namespace does not find `lib::show`, as it would
not find any other free function there. Inside the namespace, through
`dyn_Show`, and by its qualified name, it works:

```cpp
lib: namespace = {
    Show: @trait type = {
        show: (this) -> std::string;
    }
    Show_int: @given<Show, int> type = {
        show: (x: int) -> std::string = "int " + std::to_string(x);
    }
    print: <T: Show> (v: T) = { std::cout << v.show() << "\n"; }
}
main: () = {
    lib::print(1);
    d: lib::dyn_Show = 2;
    std::cout << d.show() << "\n";
    n := 3;
    std::cout << lib::show(n) << "\n";    // n.show() does not compile here
}
```

```
int 1
int 2
int 3
```

A given declared in another namespace is rejected:

```cpp
app: namespace = {
    Show_long: @given<lib::Show, long> type = {
        show: (x: long) -> std::string = "long";
    }
}
```

```
gscope.cpp2:13:5: error: a given of 'lib::Show' must be declared in the namespace that declares the trait [given-scope]
```

## Rules and diagnostics

### Accepted form

```
given-declaration:
    identifier ':' '@given' '<' trait-name ',' type-id '>' 'type' '=' '{' given-function* '}'

given-function:
    identifier ':' '(' parameter-of-type-id ( ',' parameter )* ')' ( '->' type-id )? '=' body
```

The first parameter is spelled exactly as the given's `type-id`. For a
trait function taking `inout this`, it is an `inout` parameter.

### Rules

- A given has two template arguments, a trait found by name lookup and a
  type, is not itself a template, and is declared in the namespace that
  declares the trait.
- Its members are functions without `this`, one for each function of the
  trait, each taking the given's type first. They become free functions
  following the given, in that namespace.
- With the given declared, the type satisfies the trait's concept, and
  `dyn_Trait` can hold it. UFCS `x.f()` reaches the given's `f` where
  ordinary lookup finds a free function `f`: inside the namespace, through
  a `using`, or by argument-dependent lookup when the type is a class type
  of that namespace. Elsewhere, `n.show()` on an `int` does not find
  `lib::show`.
- A second given for the same trait and type defines the same free
  functions again, which is a Cpp1 redefinition error.

Diagnostics:

- `given-arguments`: not exactly two template arguments.
- `given-not-trait`: the first argument does not name a trait.
- `given-template-type`: the given is a template.
- `given-scope`: the given is not in the namespace that declares the trait.
- `given-member-kind`: a member that is not a function.
- `given-this`: a given function takes `this`.
- `given-instance-parameter`: the first parameter's type is not the given's
  type.
- `given-missing-function`: a function of the trait is not defined.

Full entry: [`docs/EXPERIMENTS.md`](../EXPERIMENTS.md#current-experiments).

Tests: [`given.cpp2`](../../tests/ports/experiments/given.cpp2),
[`given_namespaces.cpp2`](../../tests/ports/experiments/given_namespaces.cpp2),
[`given_types.cpp2`](../../tests/ports/experiments/given_types.cpp2),
[`given_missing_error.cpp2`](../../tests/ports/experiments/given_missing_error.cpp2),
[`given_scope_error.cpp2`](../../tests/ports/experiments/given_scope_error.cpp2),
[`experiment_given_test.cpp`](../../tests/unit/experiment_given_test.cpp).
