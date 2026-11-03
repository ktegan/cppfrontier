# `trait`

Enable with `-experiment=trait`.
Explores: a new feature.

## What it enables

Two metafunctions. `@trait` declares a named set of member functions, such
as "has `area()` returning `double` and `name()` returning `std::string`".
A trait is not a type. It is used in three ways:

- as a constraint on a template parameter, `<T: Shape>`, which also checks
  the template's body against the trait at its definition;
- as `dyn_Shape`, a generated value type that holds any copyable type
  satisfying the trait and calls it through virtual dispatch;
- as a claim checked at a type, `Circle: @impl<Shape> type`, or from
  outside it, `Rect_is_Shape: @impl<Shape, Rect> type = { }`.

A type satisfies a trait structurally, by having the functions, either as
members or as free functions that UFCS reaches (`x.area()` tries
`area(x)`), such as those of a [`@given`](given.md). `@impl` only adds a
check that it does.

## Why it is useful

A Cpp1 concept checks only the uses of a template: a body that calls
something the concept does not promise compiles until some caller
instantiates it with a type that lacks it. A trait-constrained function is
instantiated once with an archetype, a type that has the trait's functions
and nothing else, so `x.perimeter()` in a body constrained by a trait
without `perimeter` fails at the definition.

`dyn_Shape` replaces the hand-written type-erasure boilerplate (an abstract
interface, a templated model, a `unique_ptr` and a clone function) for
runtime polymorphism over types that share no base class. It is a value
type: copying it copies the held object.

The costs: each trait generates a concept, a helper type, an archetype
and a `dyn_` type, and every `dyn_` call is a virtual call plus a heap allocation per
held value. A function template is checked against the archetype only when
every template parameter is trait-constrained.

## Prior art

- **Rust traits** are the main model. A generic function is type-checked
  against its trait bounds at the definition, which the archetype check
  imitates, and `dyn Trait` is the runtime-polymorphic form. Unlike Rust,
  satisfaction is structural, and `@impl` is an optional check, not what
  makes a type implement the trait.
- **Haskell type classes** and **Swift protocols** also check generic code
  against the declared requirements. Swift's existential `any P` is the
  counterpart of `dyn_Shape`.
- **C++0x concepts** (removed from the C++11 draft in 2009) type-checked
  constrained template definitions against archetypes, and concept maps
  adapted existing types to a concept. C++20 concepts check only uses, which is the gap
  this experiment fills for a restricted form of concept.

Traits are new to this project. Upstream Cpp2 has no equivalent
metafunction.

## Example

```cpp
#include <iostream>
#include <string>
#include <vector>

Shape: @trait type = {
    area: (this) -> double;
    name: (this) -> std::string;
}

Circle: @impl<Shape> @value type = {
    r: double = 0;
    operator=: (out this, r_: double) = { r = r_; }
    area: (this) -> double = 3.14159 * r * r;
    name: (this) -> std::string = "circle";
}

Rect: @value type = {
    w: double = 0;
    h: double = 0;
    operator=: (out this, w_: double, h_: double) = { w = w_; h = h_; }
    area: (this) -> double = w * h;
    name: (this) -> std::string = "rect";
}
Rect_is_Shape: @impl<Shape, Rect> type = { }

report: <T: Shape> (x: T) -> std::string = x.name() + ": " + std::to_string(x.area());

main: () = {
    std::cout << report(Rect(2.0, 3.0)) << "\n";
    shapes: std::vector<dyn_Shape> = (Circle(1.0), Rect(2.0, 3.0));
    total := 0.0;
    for shapes do (s) {
        std::cout << report(s) << "\n";
        total += s.area();
    }
    std::cout << "total " << total << "\n";
}
```

The trait currently lowers to these Cpp2 declarations beside it
(abridged). The concept asks `Shape_calls<T>` whether `T` has each
function, `Shape_calls` is defined after every other declaration so that
it sees functions declared anywhere, and `dyn_Shape` is an ordinary Cpp2
type with an inner `@interface`:

```cpp
Shape: <__cpp2_T> concept = Shape_calls<__cpp2_T>::__cpp2_satisfied();

Shape_calls: <__cpp2_T> type = {
    __cpp2_satisfied: () -> bool ==
        requires (__cpp2_T const& __cpp2_self) { { __cpp2_self.area() } -> std::convertible_to<double>; }
        && requires (__cpp2_T const& __cpp2_self) { { __cpp2_self.name() } -> std::convertible_to<std::string>; };
    __cpp2_area: (in_ref __cpp2_self: __cpp2_T) -> double = __cpp2_self.area();
    // ...
}

Shape_archetype: @cpp1_rule_of_zero type = {
    area: (this) -> double = { std::abort(); }
    name: (this) -> std::string = { std::abort(); }
}

dyn_Shape: type = {
    __cpp2_interface: @interface type = {
        __cpp2_clone: (this) -> std::unique_ptr<__cpp2_interface>;
        area: (this) -> double;
        name: (this) -> std::string;
    }
    __cpp2_model: <T: Shape> type = {
        this: __cpp2_interface = ();
        __cpp2_value: T;
        // ...
        area: (override this) -> double = Shape_calls<T>::__cpp2_area(__cpp2_value);
        // ...
    }
    __cpp2_self: std::unique_ptr<__cpp2_interface>;
    operator=: <T: Shape> (implicit out this, copy value: T) requires !std::same_as<T, dyn_Shape> && std::copy_constructible<T> = { /* ... */ }
    // ... copy clones the held value, move moves the pointer
    area: (this) -> double = __cpp2_self*.area();
    name: (this) -> std::string = __cpp2_self*.name();
}
```

Cpp2 has no namespace-scope `static_assert`, so the checks for each
`@impl` and for `report` are emitted as Cpp1 after every function
definition:

```cpp
static_assert(::Shape<::Circle>, "Circle does not satisfy Shape");
// ...
static_assert(::Shape<Rect>, "Rect does not satisfy Shape");
// ...
static_assert((static_cast<void>(&report<::Shape_archetype>), true), "the body of report is checked against Shape_archetype");
```

Output:

```
rect: 6.000000
circle: 3.141590
rect: 6.000000
total 9.14159
```

Rejected: a body that uses more than its trait. This compiles in the
translator, but the Cpp1 compiler reports it at the definition, with no
caller:

```cpp
twice: <T: Shape> (x: T) -> double = x.area() + x.perimeter();
```

```
bad_body.cpp:93:106: error: ‘const struct Shape_archetype’ has no member named ‘perimeter’
```

Rejected by the translator: a trait used as a type.

```cpp
total: (s: Shape) -> double = s.area();
```

```
bad_type.cpp2:4:12: error: 'Shape' is a trait, not a type; use 'dyn_Shape' for a value of any type satisfying it, or constrain a template parameter with it [trait-as-type]
```

## Rules and diagnostics

### Accepted form

```
trait-body:
    trait-function*            // at least one

trait-function:
    identifier ':' '(' ( 'this' | 'inout this' ) ( ',' parameter )* ')' ( '->' 'forward'? type-id )? ';'
```

Each parameter after `this` has a declared type, any passing style other
than `forward`, and no default argument. A function without `->` returns
`void`.

### Rules

- A `@trait` type is declared at namespace scope, is not a template, and
  has only bodyless, non-template functions taking `this` or `inout this`,
  other than `operator=`, without contracts.
- A type satisfies the trait when, for each function, `x.f(args)` is valid
  through a member or a free function that UFCS reaches, and its result
  converts to the declared return type. For a `void` function, any result
  is discarded. For `-> forward T`, the result is an lvalue that binds to
  `T const&`, or to `T&` for `inout this`, as a member function returning
  `forward T` returns.
- `@impl<Trait>` on a type, or `@impl<Trait, Type>` on an empty marker
  type, checks `Trait<Type>` with a `static_assert`. The trait is found by
  name lookup, and the check is in Cpp1, so a failure is a Cpp1 compile
  error naming the type.
- A namespace-scope function template whose template parameters are all
  trait-constrained, with no `requires` clause, no overloads and no
  deduced (`_`) or variadic parameters, is instantiated with the
  archetypes.
- `dyn_Trait` holds any type satisfying the trait that is
  `std::copy_constructible`, since it copies by cloning; a move-only type is
  rejected at the conversion. It moves cheaply and has the trait's functions.
- The names `Trait_calls`, `Trait_archetype` and `dyn_Trait` belong to the
  trait in its scope.

Diagnostics:

- `trait-scope`: the trait is not declared at namespace scope.
- `trait-template`: the trait is a template.
- `trait-member-kind`: a member that is not a function.
- `trait-function-body`: a trait function has a body.
- `trait-function-contract`: a trait function has a contract.
- `trait-function-this`: a function without `this` or `inout this`.
- `trait-function-form`: a function template or `operator=`.
- `trait-deduced-return`: a deduced or named return; return `void`, or
  declare `-> T` or `-> forward T`.
- `trait-generic-parameter`: a parameter without a declared type, a
  variadic one, or a `forward` one.
- `trait-default-argument`: a parameter with a default argument.
- `trait-empty`: a trait with no functions.
- `trait-generated-name`: another declaration in the trait's scope uses
  one of the generated names.
- `trait-as-type`: a trait used as the type of an object, parameter or
  return.
- `impl-arguments`: `@impl` without arguments, or with more than two.
- `impl-not-trait`: the first argument of `@impl` does not name a trait.
- `impl-template-type`: `@impl` on a template type.
- `impl-external-member`: an `@impl<Trait, Type>` marker type with members.

Full entry: [`docs/EXPERIMENTS.md`](../EXPERIMENTS.md#current-experiments).

Tests: [`trait.cpp2`](../../tests/ports/experiments/trait.cpp2),
[`given.cpp2`](../../tests/ports/experiments/given.cpp2),
[`trait_namespaces.cpp2`](../../tests/ports/experiments/trait_namespaces.cpp2),
[`trait_order.cpp2`](../../tests/ports/experiments/trait_order.cpp2),
[`trait_returns.cpp2`](../../tests/ports/experiments/trait_returns.cpp2),
[`trait_function_error.cpp2`](../../tests/ports/experiments/trait_function_error.cpp2),
[`trait_generated_name_error.cpp2`](../../tests/ports/experiments/trait_generated_name_error.cpp2),
[`trait_scope_error.cpp2`](../../tests/ports/experiments/trait_scope_error.cpp2),
[`experiment_trait_test.cpp`](../../tests/unit/experiment_trait_test.cpp).
