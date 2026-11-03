# `lazy`

Enable with `-experiment=lazy`.
Explores: a new feature.

## What it enables

`name: lazy T = init;` at namespace or type scope declares an object whose
initializer runs on its first use, exactly once, instead of during static
initialization. Every use of `name` reads the initialized object; the
first one, from whichever thread gets there first, runs `init`.

The object lowers to a `cpp2::lazy<T>` holder from `runtime/cpp2lazy.h`.
The holder is constant-initialized with a captureless lambda that returns
`T{init}` (`std::type_identity_t<T>{init}` for a type such as `int const*`
that is not a simple type specifier), and each mention of the name is
rendered as `name.value()`, which
runs the lambda under `std::call_once` the first time. A lazy member of a
type is a `static` member, one per program, not one per object.

## Why it is useful

Namespace-scope objects whose initializers read other namespace-scope
objects run into the static initialization order problem. Across
translation units the order is unspecified, and Cpp2's order-independent
declarations make it easy to write an initializer that uses an object
defined later in the same file. Either way, the initializer can read an
object that has not been constructed yet, with no diagnostic.

The usual Cpp1 fix is a function holding a local `static` and a call at
every use. `lazy` gives the same guarantee and keeps the use a plain name.
The costs: each use checks whether initialization has happened (an atomic
load inside `std::call_once`), and the initializer's side effects happen at
an unpredictable point, the first use.

## Prior art

- **Swift's global variables and static properties** are initialized
  lazily and atomically on first access, with no keyword. This experiment
  gives Cpp2 the same behavior, opt-in per object.
- **Kotlin's `by lazy { ... }`** delegated property runs its initializer on
  first access, synchronized by default. It also applies to local and
  member properties; here only namespace-scope and static objects can be
  lazy.
- **Rust's `std::sync::LazyLock`** (and the earlier `lazy_static!` macro and
  `once_cell::sync::Lazy`) is a library type for a lazily initialized
  `static`, as `cpp2::lazy<T>` is; the experiment adds the declaration
  syntax and rewrites uses so no `*` or `.value()` is needed.
- **C++ function-local statics** ("magic statics", thread-safe since C++11)
  are the Cpp1 idiom it replaces.

## Example

`marathon_km` is defined first but reads `units`, which is defined later.
Without `lazy`, `meters_per("km")` would read `units` before it is
constructed.

```cpp
#include <iostream>
#include <map>
#include <string>

marathon_km: lazy double = 42195.0 / meters_per("km");

meters_per: (unit: std::string) -> double = units.at(unit);

units: lazy std::map<std::string, double> = make_units();

make_units: () -> std::map<std::string, double> = {
    std::cout << "building the unit table\n";
    return (std::pair("m", 1.0), std::pair("km", 1000.0), std::pair("mile", 1609.344));
}

Race: type = {
    public marathon_miles: lazy double = marathon_km * 1000.0 / meters_per("mile");
}

main: () = {
    std::cout << "main starts\n";
    std::cout << marathon_km << " km\n";
    std::cout << Race::marathon_miles << " miles\n";
}
```

The holders are declared `extern` early and defined in source order; every
use goes through `.value()`:

```cpp
extern cpp2::lazy<double> marathon_km;
// ...
struct Race {
static cpp2::lazy<double> marathon_miles;
// ...
};

cpp2::lazy<double> marathon_km{[]{ return double{42195.0 / CPP2_ASSERT_NOT_ZERO(CPP2_TYPEOF(42195.0),meters_per("km"))}; }};

[[nodiscard]] auto meters_per(cpp2::impl::in<std::string> unit) -> double { return CPP2_UFCS(at)(units.value(), unit); }

cpp2::lazy<std::map<std::string, double>> units{[]{ return std::map<std::string, double>{make_units()}; }};
// ...
inline cpp2::lazy<double> Race::marathon_miles{[]{ return double{marathon_km.value() * 1000.0 / CPP2_ASSERT_NOT_ZERO(CPP2_TYPEOF(1000.0),meters_per("mile"))}; }};

auto main() -> int  {
    std::cout << "main starts\n";
    std::cout << marathon_km.value() << " km\n";
    // ...
}
```

Output. The table is built after `main` starts, on the first use:

```
main starts
building the unit table
42.195 km
26.2188 miles
```

Rejected: a lazy local.

```cpp
main: () = {
    x: lazy int = 1;
}
```

```
e.cpp2:5:5: error: a lazy object must be declared at namespace or type scope [lazy-scope]
```

## Grammar

```
unnamed-declaration:
    ...
    ':' 'lazy' type-id '=' statement            // added
```

`lazy` takes the slot before the type, as `final` does before `type`. The
parser reads it in `consume_lazy` (`src/parser.cpp`) for namespace-scope and
member declarations, and `src/statement_parser.cpp` recognizes it in a
function body to report `lazy-scope`.

`lazy` is a contextual keyword, lexed as an identifier. It is the
qualifier only when the experiment is enabled and a type follows it, so
`x: lazy = ();` still declares an object of a type named `lazy`. Without
the experiment, `lazy` is always an ordinary name.

## Rules and diagnostics

- The initializer runs once, on the first use from any thread; other
  threads block until it finishes. It is `T{init}`, so a narrowing
  initializer such as `n: lazy int = 2.5;` is a Cpp1 error.
- Every mention of the name, including a qualified one such as
  `Race::marathon_miles`, renders as `name.value()` when lookup finds the
  lazy object, in any position: a body, an initializer, a default argument
  or a signature. A local or member that hides it is left alone. In a
  signature, where there is no lookup, every mention of the name reads it.
- A namespace-scope lazy object keeps the usual `extern` declaration and
  late definition; a type-scope one is a `static` member defined `inline`
  after the types.

Diagnostics:

- `lazy-scope`: a lazy local in a function body.
- `lazy-initializer`: no `=` initializer, or an `==` alias.
- `lazy-type`: a deduced type, as in `c: lazy _ = 3;`.

Full entry: [`docs/EXPERIMENTS.md`](../EXPERIMENTS.md#current-experiments).

Tests: [`lazy.cpp2`](../../tests/ports/experiments/lazy.cpp2),
[`lazy_error.cpp2`](../../tests/ports/experiments/lazy_error.cpp2),
[`experiment_lazy_test.cpp`](../../tests/unit/experiment_lazy_test.cpp).
