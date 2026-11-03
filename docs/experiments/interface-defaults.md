# `interface-defaults`

Enable with `-experiment=interface-defaults`.
Explores: a new feature.

## What it enables

By default, every function of an `@interface` type must be bodyless, and
each one becomes a pure virtual function; a function with a body is the
error `interface-function-body`. With the experiment, a function of an
`@interface` may have a body. It becomes a virtual function with that body
as its default implementation, which a derived type may override or
inherit. A bodyless function stays pure virtual.

When no function is pure, the generated default constructor is
`protected`, so the interface still cannot be created on its own.

## Why it is useful

An interface often has a few functions that every implementation must
supply and others that are naturally written in terms of them, such as a
`format` built from `prefix` and `digits`. Without defaults, either every
implementation repeats that code, or the interface is written as a
`@polymorphic_base` and loses `@interface`'s guarantees: no data members,
all functions public and virtual, and protected copy and move.

Adding a function with a default to an interface also no longer breaks
every existing implementation.

## Prior art

- **Java's default methods** (Java 8) let an interface method have a body
  marked `default`. Here no keyword is needed: the body marks it.
- **Rust's default trait methods** give a trait function a body that an
  `impl` may override, the same split between required and provided
  functions.
- **C# default interface methods** (C# 8) are the same feature in another
  language that had pure interfaces first.
- In Cpp1 this is an abstract base class with some non-pure virtual
  functions, which is what the experiment lowers to. Upstream cppfront's
  `@interface` allows only pure virtual functions, as the default here
  does.

## Example

```cpp
#include <format>
#include <iostream>
#include <string>

Formatter: @interface type = {
    digits: (this, n: int) -> std::string;
    prefix: (this) -> std::string = "";
    format: (this, n: int) -> std::string = prefix() + digits(n);
}

Decimal: type = {
    this: Formatter = ();
    digits: (override this, n: int) -> std::string = std::to_string(n);
}

Hex: type = {
    this: Formatter = ();
    digits: (override this, n: int) -> std::string = std::format("{:x}", n);
    prefix: (override this) -> std::string = "0x";
}

print: (f: Formatter, n: int) = std::cout << f.format(n) << "\n";

main: () = {
    d: Decimal = ();
    h: Hex = ();
    print(d, 255);
    print(h, 255);
}
```

`digits` stays pure, and the two functions with bodies are virtual with
out-of-line definitions:

```cpp
struct Formatter {
[[nodiscard]] virtual auto digits(cpp2::impl::in<int> n) const -> std::string = 0;
[[nodiscard]] virtual auto prefix() const -> std::string;
[[nodiscard]] virtual auto format(cpp2::impl::in<int> n) const -> std::string;
Formatter() = default;
protected: Formatter(Formatter const& that);
// ...
};
// ...
[[nodiscard]] auto Formatter::prefix() const -> std::string { return ""; }
[[nodiscard]] auto Formatter::format(cpp2::impl::in<int> n) const -> std::string { return prefix() + digits(n); }
```

Output:

```
255
0xff
```

Without the flag, the same program is rejected:

```
fmt.cpp2:7:5: error: interface functions must not have a function body; remove the '=' initializer [interface-function-body]
fmt.cpp2:8:5: error: interface functions must not have a function body; remove the '=' initializer [interface-function-body]
```

## Rules and diagnostics

- A function with `this` and a body is `virtual` with that body as its
  default; one without a body is pure virtual. An interface without bodies
  translates exactly as it does by default.
- When every function has a body, the generated default constructor is
  `protected`; otherwise it is public, as by default, and the pure
  functions keep the type abstract.
- The other `@interface` rules are unchanged: functions are public, there
  are no data members, and copy and move are protected and not
  user-declared.
- A constructor cannot be virtual, so one with a body is still rejected.

Diagnostics:

- `interface-function-body`: a constructor with a body (with the
  experiment), or any function with a body (without it).
- `interface-function-this`: a function without `this`, with or without a
  body.
- `interface-function-access`, `interface-data-member` and
  `interface-copy-or-move`: as by default.

Full entry: [`docs/EXPERIMENTS.md`](../EXPERIMENTS.md#current-experiments).

Tests: [`interface_defaults.cpp2`](../../tests/ports/experiments/interface_defaults.cpp2),
[`experiment_interface_defaults_test.cpp`](../../tests/unit/experiment_interface_defaults_test.cpp).
