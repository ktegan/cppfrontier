# `checked-arithmetic`

Enable with `-experiment=checked-arithmetic`.
Explores: a new feature.

## What it enables

Integer `+`, `-`, `*`, unary `-`, `+=`, `-=`, `*=`, `++` and `--` are checked
for overflow at run time, when the translator knows that every operand is an
integer. They lower to the `cpp2::checked_*` helpers of
`runtime/cpp2checked.h`, which compute the same result as the plain operator
and report a violation of the `cpp2::arithmetic_safety` contract group when
the mathematical result does not fit the result type. The default handler
prints a message and terminates.

Unsigned arithmetic is checked too: `stop - start` with `stop < start` is a
violation, not a wrap. Arithmetic meant to wrap, such as a hash, calls
`wrapping_add`, `wrapping_sub` or `wrapping_mul`, written as a function or
with UFCS: `h.wrapping_mul(p)`.

## Why it is useful

Signed overflow is undefined behavior in C++, and unsigned wraparound is
defined but is almost always a bug where it happens by accident: an elapsed
time or a size difference that should have been small becomes about four
billion. Cpp2 already checks subscripts, null dereferences and division by
zero by default. This experiment adds the remaining common arithmetic error.

Checking unsigned arithmetic is deliberate, as in Rust's debug builds. Code
that relies on wraparound says so with a `wrapping_*` call, which also
documents the intent for the reader.

The costs:

- a branch on each checked operation (GCC's and Clang's
  `__builtin_*_overflow`, or portable checks elsewhere);
- existing code that wraps on purpose terminates until it uses `wrapping_*`.

The check is also partial. A call result, a member of an object other than
`this` or `that` (`t.v`), a range-`for` variable, a deduced parameter
(`x: _`) or a floating-point operand keeps the plain operator, as does arithmetic inside an `unsafe` block. A chain of
literals such as `2 + 3` is a constant and is left to the Cpp1 compiler.

## Prior art

- **Rust** panics on integer overflow when `overflow-checks` is on, the
  default for debug builds, and wraps otherwise (RFC 560). It offers
  `wrapping_add`, `wrapping_sub` and `wrapping_mul` methods, along with
  `checked_*`, `saturating_*` and `overflowing_*`. The experiment
  checks in every build and borrows the `wrapping_*` names and their UFCS
  spelling.
- **Swift** traps on overflow of `+`, `-` and `*` in normal builds and
  provides the wrapping operators `&+`, `&-` and `&*`. The experiment uses
  functions instead of new operators.
- **C#** has `checked` and `unchecked` expressions and blocks, and a
  project-wide setting, that make integer overflow throw
  `OverflowException`. The experiment is per translation unit, with
  `unsafe { }` as the unchecked block.
- **Zig** treats overflow as illegal behavior that is checked in its safe
  build modes and has wrapping operators `+%`, `-%` and `*%`.

## Example

```cpp
// Milliseconds from `start` to `stop` on a 32-bit clock.
elapsed: (start: u32, stop: u32) -> u32 = stop - start;

// The 32-bit FNV-1a hash, which wraps on purpose.
fnv1a: (s: std::string_view) -> u32 = {
    h: u32 = 2166136261;
    for s do (c) {
        h = (h ^ (c as u8)).wrapping_mul(16777619);
    }
    return h;
}

main: () = {
    std::cout << fnv1a("hello") << "\n";
    std::cout << elapsed(10, 25) << "\n";
    std::cout << elapsed(25, 10) << "\n";
}
```

It lowers to:

```cpp
#include "cpp2checked.h"
// ...
[[nodiscard]] auto elapsed(cpp2::impl::in<unsigned int> start, cpp2::impl::in<unsigned int> stop) -> unsigned int { return cpp2::checked_sub(stop, start); }
// ...
        h = cpp2::wrapping_mul((h ^ (cpp2::impl::as_<unsigned char>(c))), 16777619);
```

The third call is reported, and the program stops:

```
1335831723
15
ca.cpp(14) unsigned int elapsed(cpp2::impl::in<unsigned int>, cpp2::impl::in<unsigned int>): Arithmetic safety violation: integer subtraction overflow
```

Without the flag, with the hash written `h = (h ^ (c as u8)) * 16777619;`,
the last line silently prints `4294967281`. With the flag, that
spelling of the hash is reported on the first character:

```
p2.cpp(19) unsigned int fnv1a(cpp2::impl::in<std::basic_string_view<char> >): Arithmetic safety violation: integer multiplication overflow
```

## Rules and diagnostics

- An operation is checked when all its operands are known integers:
  - integer literals;
  - names declared with an integer type, or locals deduced from a known
    integer initializer (`i := 1`);
  - data members declared with an integer type, named in a member function
    as `x`, `this.x` or `that.x`, so the `+`, `-`, `+=` and `-=` that
    `@quantity` generates are checked like its `*`. A member of any other
    object (`t.v`) is not known. A range-`for` variable is deduced from
    the range, whose element type is not known, so it is not known either;
  - `x as T` and `: T = x` with an integer `T`;
  - integer operators applied to known operands.
- `a + b - c` folds from the left: `cpp2::checked_sub(cpp2::checked_add(a, b),
  c)`. After the first unknown operand, the rest of the chain is plain.
  `/` and `%` keep their division-by-zero check.
- The result type is the plain operator's type after the usual arithmetic
  conversions. A compound assignment, `++` or `--` checks against the type
  of the object it stores to, so `c: i8 = 127; c++;` is a violation.
- `wrapping_add`, `wrapping_sub` and `wrapping_mul` return the plain
  operator's result modulo 2^N in that same type, so for `n: u8 = 200`,
  `wrapping_mul(n, n)` is `40000` (an `int`). They are support names, like
  `unchecked_div`, and lower to `cpp2::wrapping_*`, also when called with
  UFCS. They exist only with the experiment.
- Code in an `unsafe` block (experiment [`unsafe`](unsafe.md)) uses the
  plain operators. The block turns off the checks that the `-no-*-checks`
  flags turn off, and checked arithmetic as well. No command-line flag
  turns off checked arithmetic alone.
- A run-time violation reports `integer addition overflow`,
  `integer subtraction overflow`, `integer multiplication overflow` or
  `integer negation overflow` through `cpp2::arithmetic_safety`. The
  translator adds no diagnostics.

Full entry: [`EXPERIMENTS.md`](../EXPERIMENTS.md#current-experiments).

Tests:
[`checked_arithmetic.cpp2`](../../tests/ports/experiments/checked_arithmetic.cpp2),
[`checked_arithmetic_wrapping.cpp2`](../../tests/ports/experiments/checked_arithmetic_wrapping.cpp2),
[`experiment_checked_arithmetic_test.cpp`](../../tests/unit/experiment_checked_arithmetic_test.cpp).
