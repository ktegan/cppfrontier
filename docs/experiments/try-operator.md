# `try-operator`

Enable with `-experiment=try-operator`.
Explores: a new feature.

## What it enables

A postfix `e?` tests a `std::expected` or `std::optional`. If `e` holds a
value, `e?` is that value. If it does not, the enclosing function returns
the failure at once: `std::unexpected` of the error for an `expected`, or
`std::nullopt` for an `optional`. The function must declare an `expected`
or `optional` return type.

Each `?` operand is hoisted into its own Cpp1 statement just before the
statement that contains it:
`auto&& __cpp2_try_N = cpp2::try_hold(e); if (!__cpp2_try_N) return cpp2::try_failure(...);`,
and the use reads `*CPP2_FORWARD(__cpp2_try_N)`. Both helpers come from
`runtime/cpp2try.h`, which is included when lowering hoists an operand.

## Why it is useful

Code that returns `std::expected` spends most of its lines forwarding
failures: call, test, return the error, then unwrap. Each step is a chance
to return the wrong thing or to dereference an empty result. With `?` the
happy path reads as straight-line code and every failure is forwarded the
same way.

Because the operand is hoisted, `?` can only appear where an early return
is possible and where the operand would have been evaluated anyway. The
translator rejects it in conditions, contracts, defer bodies, inspect
alternatives, parameter defaults and on the right of `&&` or `||`.

It also changes the meaning of `?` in Cpp2 source. By default any `?` is
reported as a Cpp1 conditional operator, `cpp1-conditional-operator`. With
the experiment, `c ? a : b` parses as `c?` followed by stray text and is a
parse error.

## Prior art

- **Rust's `?` operator** (RFC 243) is the model: postfix, on `Result` or
  `Option`, returning the error or `None` from the enclosing function. Rust
  converts the error with `From`; here `std::unexpected<E>` converts to the
  function's `std::expected<T, G>` when `G` is constructible from `E`, and
  otherwise the Cpp1 compiler rejects the `return`.
- **Zig's `try expr`** does the same for error unions as a prefix keyword.
- **P2561** (Barry Revzin) proposes an error-propagation operator for C++
  with these semantics; its spelling has changed between revisions. This
  experiment is a translator-only version that needs no language change,
  limited to the two standard types.

## Example

```cpp
#include <expected>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

digit: (c: char) -> std::expected<int, std::string> = {
    if c < '0' || c > '9' { return std::unexpected("not a digit: " + std::string(1, c)); }
    return c - '0';
}

number: (s: std::string) -> std::expected<int, std::string> = {
    if s.empty() { return std::unexpected<std::string>("empty number"); }
    n := 0;
    for s do (c) {
        n = n * 10 + digit(c)?;
    }
    return n;
}

point: (s: std::string) -> std::expected<std::pair<int, int>, std::string> = {
    comma := s.find(',');
    if comma == s.npos { return std::unexpected<std::string>("missing ','"); }
    return std::pair(number(s.substr(0, comma))?, number(s.substr(comma + 1))?);
}

main: () = {
    for (:std::vector<std::string> = ("3,14", "3,x", "34", ",1")) do (s) {
        p := point(s);
        if p.has_value() { std::cout << s << " -> " << p*.first << " " << p*.second << "\n"; }
        else { std::cout << s << " -> " << p.error() << "\n"; }
    }
}
```

The hoisted tests stay on the source line of their statement, so `#line`
mappings and debugger steps still match the Cpp2 lines:

```cpp
    for (auto const& c : s) {
        auto&& __cpp2_try_0 = cpp2::try_hold(digit(c)); if (!__cpp2_try_0) return cpp2::try_failure(CPP2_FORWARD(__cpp2_try_0)); n = n * 10 + (*CPP2_FORWARD(__cpp2_try_0));
    }
    // ...
    auto&& __cpp2_try_1 = cpp2::try_hold(number(CPP2_UFCS(substr)(s, 0, comma))); if (!__cpp2_try_1) return cpp2::try_failure(CPP2_FORWARD(__cpp2_try_1)); auto&& __cpp2_try_2 = cpp2::try_hold(number(CPP2_UFCS(substr)(s, comma + 1))); if (!__cpp2_try_2) return cpp2::try_failure(CPP2_FORWARD(__cpp2_try_2)); return std::pair((*CPP2_FORWARD(__cpp2_try_1)), (*CPP2_FORWARD(__cpp2_try_2)));
```

Output:

```
3,14 -> 3 14
3,x -> not a digit: x
34 -> missing ','
,1 -> empty number
```

Rejected: `?` in a condition, where no statement can be put before the test.

```cpp
first_positive: (o: std::optional<int>) -> std::optional<int> = {
    if o? > 0 { return o; }
    return std::nullopt;
}
```

```
t4.cpp2:3:9: error: `?` is allowed only in a function body statement that can return early; not in a condition [try-operator-context]
```

A Cpp1 conditional, without and with the experiment:

```cpp
larger: (a: int, b: int) -> int = {
    return a > b ? a : b;
}
```

```
t3.cpp2:2:18: error: the conditional operator 'c ? a : b' is not valid Cpp2; use an 'if' statement or an 'inspect' expression [cpp1-conditional-operator]
```

```
t3.cpp2:2:18: error: `?` returns early from a function whose declared return type is std::expected or std::optional, not `int` [try-operator-return-type]
t3.cpp2:2:20: error: unexpected text 'a' after an expression - is an operator or ';' missing? [juxtaposed-operands]
```

## Grammar

```
postfix-operator:
    one of  '++' '--' '*' '&' '~' '$' '...' '?'     // '?' added
```

The change is in the expression parser's postfix loop
(`src/expression_parser.cpp`), so `?` takes part in
`postfix-expression: postfix-expression postfix-operator` and can be
followed by further postfix forms, as in `r?.value_or(0)` or `f(x)?[0]`.

Lexing does not change: `?` was already a token. What changes is its role.
By default every `?` is the Cpp1 conditional's and is diagnosed. With the
experiment, a `?` right after an operand is a postfix operator that ends
the operand (`roles_of` in `src/cpp2_validation.cpp` agrees), and there is
no conditional-expression production. `c ? a : b` therefore reads as `c?`
followed by `a`, a `juxtaposed-operands` parse error. A `?` that follows no
operand, as in `return ? n;`, is `try-operator-context`.

## Rules and diagnostics

- The operand must be a `std::expected` or `std::optional`;
  `cpp2::try_failure` has overloads for those two only.
- The enclosing function must declare its return type as
  `std::expected<...>` or `std::optional<...>`, spelled that way. A
  deduced return type, `-> _`, no return type and named return values are
  rejected. Whether the failure converts to the declared type is checked by
  the Cpp1 compiler.
- `?` may appear in an expression statement, a `_ =` discard, a `return`, a
  local object's initializer, a destructuring initializer, an
  expression-bodied function (`f: (a) -> std::expected<...> = g(a)? + 1;`)
  and the body of a function expression with a suitable declared return
  type, where it returns from the function expression.
- Each operand is hoisted once, inner operands first, as
  `auto&& __cpp2_try_N = cpp2::try_hold(e);`, and the use reads
  `*CPP2_FORWARD(__cpp2_try_N)`. `try_hold` keeps an lvalue operand a
  reference, so it is not copied, and moves an rvalue into a value, so an
  xvalue into a temporary, as in `std::get<0>(make())?`, does not dangle.
- Hoisting evaluates every `?` operand of a statement before the rest of
  the statement. In `std::cout << step() << get()?;`, `get()` is called
  before `step()`, and `step()` is not called at all when `get()` fails.
- A `defer` body runs inside a scope guard, and a comprehension's guard and
  value run inside a lambda, neither of which can return from the
  function, so `?` there is `try-operator-context`. A `static` local's
  initializer runs once, not on every call, so `?` there is also
  `try-operator-context`. The comprehension's range is evaluated in the
  statement and may hold `?`.
- The flow analyses see the early return after the hoisted operands, as a
  `return`: an `out` parameter must be initialized before it, the defers
  run on it, and under [`linear`](linear.md) each linear value must be
  consumed before it.

Diagnostics:

- `try-operator-context`: `?` in an `if`, loop condition, `for` range or
  `next` clause, a contract, a `defer` body, an inspect alternative, a
  comprehension's guard or value, a `static` local, a statement parameter, a parameter default, a requires clause, a named
  return value, a local function or type declaration, or an initializer
  outside a function; or a `?` that follows no operand.
- `try-operator-return-type`: the function's declared return type is not
  `std::expected` or `std::optional`.
- `try-operator-short-circuit`: `?` on the right of `&&` or `||`, where
  hoisting would evaluate it unconditionally.

Full entry: [`docs/EXPERIMENTS.md`](../EXPERIMENTS.md#current-experiments).

Tests: [`try_operator.cpp2`](../../tests/ports/experiments/try_operator.cpp2),
[`try_operator_destructuring.cpp2`](../../tests/ports/experiments/try_operator_destructuring.cpp2),
[`experiment_try_operator_test.cpp`](../../tests/unit/experiment_try_operator_test.cpp).
