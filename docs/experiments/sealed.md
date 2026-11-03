# `sealed`

Enable with `-experiment=sealed`. Builds on [`patterns`](patterns.md), which it enables.
Explores: a new feature.

## What it enables

The metafunction `@sealed`, added to a `@polymorphic_base` or `@interface`
type, closes its hierarchy: the types that derive from it directly are
exactly those declared in the same scope of this translation unit. Two
things follow.

- The base's constructors become private, and each derived type is
  declared its friend, so code elsewhere, such as another translation unit,
  cannot derive a constructible type from it.
- An `inspect` of the base, `inspect p` for `p: Payment` or `inspect p*`
  for `p: *Payment`, that names every derived type needs no `is _`, and one
  that misses a derived type is an error naming it.

## Why it is useful

A closed set of alternatives is often modeled as a class hierarchy, and
then every `inspect` or `dynamic_cast` chain over it needs a default branch
for subclasses that might exist somewhere. That branch hides the case
where a new subclass is added and some dispatch is not updated. With
`@sealed`, the translator knows the full set, so adding `Wire` to
`Payment` makes each `inspect` that does not handle it fail to translate.

The cost is extensibility: a sealed base cannot be extended outside its
scope, which is the point. An `inspect` that covers every case still ends
in `std::terminate()` in the Cpp1, for a value that matches none.

## Prior art

- **Kotlin's sealed classes** require direct subclasses to be in the same
  package, and a `when` expression over one that covers every subclass
  needs no `else`. This experiment's boundary is narrower: the base's own
  scope in one translation unit.
- **Scala's `sealed` traits and classes** restrict direct subclasses to the
  same source file, and the compiler warns when a `match` is not
  exhaustive. Here a missing case is an error.
- **Java's sealed classes** (JEP 409, Java 17) name their permitted
  subclasses with `permits`, or infer them from the same file, and
  `switch` with patterns (Java 21) can be exhaustive over them. Here the
  set is always inferred from the scope.
- **[`@adt`](adt.md)** is this project's value-type sum type. `@sealed` gives
  the same exhaustiveness to a polymorphic hierarchy whose cases have
  virtual functions and are used through a base reference or pointer.

## Example

```cpp
#include <iostream>
#include <string>

Payment: @sealed @polymorphic_base type = {
    amount: (virtual this) -> int;
}

Card: type = {
    this: Payment = ();
    cents: int;
    operator=: (out this, c: int) = { cents = c; }
    amount: (override this) -> int = cents;
}

Cash: type = {
    this: Payment = ();
    cents: int;
    operator=: (out this, c: int) = { cents = c; }
    amount: (override this) -> int = cents;
}

fee_percent: (p: Payment) -> int = inspect p -> int {
    is Card = 2;
    is Cash = 0;
};

fee: (p: Payment) -> int = p.amount() * fee_percent(p) / 100;

main: () = {
    card: Card = (1000);
    cash: Cash = (500);
    std::cout << fee(card) << " " << fee(cash) << "\n";
}
```

The base's default constructor is private, with the derived types as
friends:

```cpp
struct Payment {
[[nodiscard]] virtual auto amount() const -> int = 0;
virtual ~Payment() noexcept { }
private: Payment() = default;
Payment(Payment const&) = delete;
friend Card; friend Cash;
public: auto operator=(Payment const&) -> void = delete;
};
// ...
[[nodiscard]] auto fee_percent(cpp2::impl::in<Payment> p) -> int { return [&] () -> int { [[maybe_unused]] auto&& __cpp2_expr = p;
    if (cpp2::impl::is<Card>(__cpp2_expr)) { /* ... */ return 2; /* ... */ }
    if (cpp2::impl::is<Cash>(__cpp2_expr)) { /* ... */ return 0; /* ... */ } std::terminate();
}(); }
```

Output:

```
20 0
```

Rejected: an `inspect` that misses `Cash`.

```cpp
fee_percent: (p: Payment) -> int = inspect p -> int {
    is Card = 2;
};
```

```
miss.cpp2:6:36: error: inspect of 'Payment' does not cover Cash - add alternatives or 'is _' [inspect-not-exhaustive]
```

Rejected: a derived type in another scope, and `@sealed` on a type that is
not polymorphic.

```cpp
bank: namespace = {
    Wire: type = { this: Payment = (); amount: (override this) -> int = 100; }
}
Plain: @sealed type = { x: int = 0; }
```

```
scope.cpp2:6:5: error: 'Wire' derives from the @sealed type 'Payment' but is not declared in the same scope [sealed-derived-scope]
scope.cpp2:8:1: error: a @sealed type must also be @polymorphic_base or @interface [sealed-not-polymorphic]
```

A Cpp1 type written elsewhere, `struct Voucher : Payment { ... };`, fails
to compile when it is constructed:

```
other.cpp:38:8: error: ‘constexpr Payment::Payment()’ is private within this context
```

## Rules and diagnostics

- `@sealed` applies to a non-template `@polymorphic_base` or `@interface`
  type at namespace or file scope.
- Its cases are the types in the same scope with a base `this: Base`
  naming it, written plainly or qualified. Only direct derivation counts;
  a type deriving from a case is matched by that case's `is`.
- Every constructor of the base, generated or written, becomes private, and
  every case is a friend; a class template case through its template head,
  `template<typename, int> friend struct Scaled;`.
- An `inspect` whose subject is the sealed type, or the dereference `p*`
  of a pointer to it, is exhaustive when it names every case. A pointer
  subject `inspect p` is not the sealed type and stays `invalid-inspect`.

Diagnostics:

- `inspect-not-exhaustive`: an `inspect` without `is _` that misses one or
  more cases, all of which it lists.
- `sealed-not-polymorphic`: the type is neither `@polymorphic_base` nor
  `@interface`.
- `sealed-template`: the type is a template.
- `sealed-scope`: the type is nested in another type, where a friend
  declaration could not name derived types declared after it.
- `sealed-derived-scope`: a type in another scope derives from it.
- `sealed-no-derived` (warning): nothing derives from it in this
  translation unit.

Full entry: [`docs/EXPERIMENTS.md`](../EXPERIMENTS.md#current-experiments).

Tests: [`sealed.cpp2`](../../tests/ports/experiments/sealed.cpp2),
[`sealed_error.cpp2`](../../tests/ports/experiments/sealed_error.cpp2),
[`experiment_sealed_test.cpp`](../../tests/unit/experiment_sealed_test.cpp).
