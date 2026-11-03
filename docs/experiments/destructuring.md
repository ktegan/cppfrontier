# `destructuring`

Enable with `-experiment=destructuring`.
Explores: a new feature.

## What it enables

`(a, b) := value;` in a function body declares one local per element of a
tuple-like value: a `std::pair`, `std::tuple` or `std::array`, an aggregate
struct, or the result struct of a Cpp2 function with named return values.
It lowers to a C++17 structured binding, `auto [a, b] = value;`. A `_` in
the list skips that element.

Each name is then an ordinary Cpp2 local: lookup finds it and definite
initialization knows it is initialized. Its definite last use is forwarded,
`CPP2_FORWARD(a)`: an element held by value is moved, as any local's last
use is, and one bound by reference, as through `std::tie`, is not, since a
move would empty the object it refers to.

## Why it is useful

Cpp2 has no spelling for a structured binding. Without one, a function that
returns two values through a pair or a named-return struct makes the caller
write `r := f();` and then `r.first` or `r.key`, or copy each element into a
local by hand. Cpp1 text `auto [a, b] = f();` in a Cpp2 function body passes
through, but the translator does not see `a` and `b` as locals, so, for
example, their last uses are not forwarded.

The cost is small. The form is a statement, not an expression, and the
types are always deduced: there is no `(a, b): T = ...` and no binding by
reference. To bind references, destructure a tuple of references, such as
`std::tie(x, y)`.

## Prior art

- **C++17 structured bindings** (P0144) are the semantics: the experiment is
  a Cpp2 spelling of `auto [a, b] = e;`, with the same tuple-like protocol
  (`std::tuple_size` and `get`, arrays, and public data members).
- **Go's short variable declaration** `q, r := divmod(17, 5)` is the closest
  syntax: `:=` declares several deduced locals, and the blank identifier `_`
  discards one. Go has no parentheses there, and its right side is a
  multiple-value call rather than a tuple object.
- **Rust's `let (a, b) = pair;`** and **Swift's `let (a, b) = pair`** use a
  tuple pattern with `_` as a wildcard. Their patterns can nest and bind by
  reference; this experiment's name list is flat.
- **P2169** (C++26) makes `_` a placeholder that may be repeated in a
  structured binding. Here `_` lowers to a generated name and the
  declaration gets `[[maybe_unused]]`, so it works in C++20 and C++23.

## Example

```cpp
#include <iostream>
#include <map>
#include <string>
#include <vector>

split: (line: std::string) -> (key: std::string, value: std::string) = {
    eq := line.find('=');
    key = line.substr(0, eq);
    value = line.substr(eq + 1);
}

main: () = {
    settings: std::map<std::string, std::string> = ();
    for (:std::vector<std::string> = ("color=red", "size=10", "color=blue")) do (line) {
        (key, value) := split(line);
        (_, added) := settings.try_emplace(key, value);
        if !added { std::cout << "ignored " << key << "=" << value << "\n"; }
    }
    for settings do (entry) {
        (key, value) := entry;
        std::cout << key << ": " << value << "\n";
    }
}
```

Each declaration becomes a structured binding, the `_` becomes a generated
name, and each name's last use is forwarded:

```cpp
    for (auto const& line : (std::vector<std::string>{"color=red", "size=10", "color=blue"})) {
        auto [key, value] = split(line);
        [[maybe_unused]] auto [__cpp2_unused_16_10, added] = CPP2_UFCS(try_emplace)(settings, key, value);
        if (!CPP2_FORWARD(added)) {
            std::cout << "ignored " << CPP2_FORWARD(key) << "=" << CPP2_FORWARD(value) << "\n";
        }
    }
    for (auto const& entry : cpp2::move(settings)) {
        auto [key, value] = entry;
        std::cout << CPP2_FORWARD(key) << ": " << CPP2_FORWARD(value) << "\n";
    }
```

Output:

```
ignored color=blue
color: red
size: 10
```

Rejected: a parenthesized list is not one tuple-like value.

```cpp
    (a, b) := (1, 2);
```

```
b1.cpp2:2:15: error: a destructuring initializer is one tuple-like value - write std::tuple(...) for a list [destructuring-initializer]
```

## Grammar

```
statement:
    ...
    destructuring-declaration          // added

destructuring-declaration:
    '(' destructuring-name-seq ')' ':=' expression ';'

destructuring-name-seq:
    destructuring-name
    destructuring-name-seq ',' destructuring-name

destructuring-name:
    identifier
    '_'
```

The block parser (`src/statement_parser.cpp`, `parse_destructuring`) tries
this form for a statement that starts with `(`, after a statement-parameter
list such as `(copy x := v) stmt` and before an expression statement. It
matches only a list of plain names followed by `:=` or `:`, so
`(a, b) = f();` and `(x) := y` in other positions are not affected. A
single name parses but is diagnosed, as is `:` with a type.

There is no lexing change. The line classifier (`src/source.cpp`) also
treats a line beginning `(a, b) :=` or `(a, b):` as Cpp2, so a destructuring
declaration at file scope is diagnosed instead of being passed through as
Cpp1.

## Rules and diagnostics

- The initializer is evaluated once into a hidden object, as with
  `auto [...] = e;`, and each name refers to one of its elements. The
  hidden object is a copy of a named value, or the result itself when the
  initializer is a call or other temporary.
- Each name is a local of the enclosing block for lookup and
  initialization. Its definite last use, including `return a;`, is
  `CPP2_FORWARD(a)`, which moves an element held by value and copies one
  bound by reference. So `(a, b) := std::tie(s, t); consume(a);` copies
  `s` instead of emptying it.
- `_` may appear more than once and binds a `[[maybe_unused]]` generated
  name.
- Without the experiment the statement is not recognized and is emitted
  unchanged, so the Cpp1 compiler rejects it.

Diagnostics:

- `destructuring-scope`: a destructuring declaration at file, namespace or
  type scope.
- `destructuring-arity`: fewer than two names.
- `destructuring-duplicate-name`: a name other than `_` appears twice.
- `destructuring-type`: `(a, b): T = ...`; the types are always deduced.
- `destructuring-initializer`: the initializer is a parenthesized list.
- `missing-initializer`: nothing after `:=`.

Full entry: [`docs/EXPERIMENTS.md`](../EXPERIMENTS.md#current-experiments).

Tests: [`destructuring.cpp2`](../../tests/ports/experiments/destructuring.cpp2),
[`try_operator_destructuring.cpp2`](../../tests/ports/experiments/try_operator_destructuring.cpp2),
[`experiment_destructuring_test.cpp`](../../tests/unit/experiment_destructuring_test.cpp).
