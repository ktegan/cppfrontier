# `nonnegative-literal-comparisons`

Enable with `-experiment=nonnegative-literal-comparisons`.
Explores: [`LANGUAGE.md` Comparisons](../LANGUAGE.md#safety-checks), literals
in mixed-sign comparisons.

## What it enables

Cpp2 rejects a `<`, `<=`, `>` or `>=` between a signed and an unsigned
integer. By default an unsuffixed integer literal counts as signed, so
`s > 0` with `s: std::size_t` is an error, and you write `s > 0u`. The
translator reports it when it knows both types, and otherwise
`cpp2::impl::cmp_greater` rejects it when the Cpp1 is compiled.

With the experiment, a decimal integer literal without a `u` suffix compares
against either signedness. `s > 0` is accepted, and the translator emits it
as `cpp2::impl::cmp_greater_literal(s,0)` instead of `cmp_greater(s,0)`.

## Why it is useful

The mixed-sign check catches a real bug: `-1 < 1u` is `false` in C++
because `-1` converts to a large unsigned value. A decimal literal cannot
cause that bug. It has no minus sign (`-1` is unary minus applied to `1`),
so its value is non-negative and converting it to either signedness keeps
that value. Requiring `0u` there adds noise without making anything safer,
and the most common comparisons, `size > 0` and `n >= 1`, are the ones it
affects.

Nothing that compiled before stops compiling. The cost is a special case in
the rule: the operand's spelling, not only its type, decides whether the
comparison is checked. Hexadecimal, octal and binary literals keep the
check, since in C++ such a literal may have an unsigned type.

## Prior art

- **Go's untyped constants.** In Go, `s > 0` with `s uint` is accepted
  because the constant `0` takes the type of the other operand, and
  `s > -1` is a compile error because `-1` does not fit `uint`. The
  experiment reaches the same result for decimal literals without a
  type system for constants.
- **Rust's integer literals** have no fixed type. They are inferred from
  context, so `s > 0` for `s: usize` makes `0` a `usize`. Rust has no
  implicit mixed-sign comparison at all.
- **GCC's and Clang's `-Wsign-compare`** do not warn when the signed operand
  is a non-negative constant, for the same reason.
- **C++20 `std::cmp_less` and its siblings** (P0586) compare mixed-sign
  integers by value. Cpp2's `cmp_*` helpers reject the mixed case instead.
  The experiment leaves them in place for every other operand.

## Example

```cpp
describe: (count: std::size_t) -> std::string = {
    if count > 1 { return "many"; }
    if count > 0 { return "one"; }
    return "none";
}

main: () = {
    v: std::vector<int> = (1, 2, 3);
    std::cout << describe(v.size()) << " " << describe(1u) << " " << describe(0u) << "\n";
    std::cout << (v.size() >= 3) << "\n";
}
```

Without the flag, the translator rejects both comparisons in `describe`:

```
nn.cpp2:2:5: error: 'count > 1' is a mixed signed/unsigned comparison, which is unsafe - write '1u' for an unsigned literal, use 'as' to change the signedness of one operand, or unchecked_cmp_greater to opt out [mixed-sign-comparison]
```

`v.size() >= 3` has no error from the translator, which does not know the
type of `v.size()`. It reaches `cmp_greater_eq`, whose `static_assert` fails
when the Cpp1 is compiled.

With the flag, every comparison calls a `_literal` helper:

```cpp
[[nodiscard]] auto describe(cpp2::impl::in<std::size_t> count) -> std::string  {
    if (cpp2::impl::cmp_greater_literal(count,1)) {
// ...
    if (cpp2::impl::cmp_greater_literal(count,0)) {
// ...
    std::cout << (cpp2::impl::cmp_greater_eq_literal(CPP2_UFCS(size)(cpp2::move(v)),3)) << "\n";
```

Output:

```
many one none
1
```

## Rules and diagnostics

- The rule covers an operand of `<`, `<=`, `>` or `>=` that is a single
  decimal integer literal without `u` or `U`, such as `0`, `42` or `7ll`.
  The safety pass treats it as compatible with signed and unsigned operands,
  and the renderer emits that comparison as `cpp2::impl::cmp_less_literal`,
  `cmp_less_eq_literal`, `cmp_greater_literal` or `cmp_greater_eq_literal`
  (`runtime/experiments.patch`). In a chain such as `0 <= s < 10`, the
  literal is not captured, only the other operands:
  `[_1 = s]{ return cpp2::impl::cmp_less_eq_literal(0,_1) && cpp2::impl::cmp_less_literal(_1,10); }()`.
- The `_literal` helpers reject a `bool` operand as `cmp_*` does, so
  `b < 1` still fails when the Cpp1 is compiled.
- Hexadecimal, octal and binary literals, `u`-suffixed literals, negated
  literals (`-1`) and named operands keep the check.
- `mixed-sign-comparison`: the translator knows both operand types and they
  differ in signedness.

Full entry: [`EXPERIMENTS.md`](../EXPERIMENTS.md#current-experiments).

Tests:
[`nonnegative_literal_comparisons.cpp2`](../../tests/ports/experiments/nonnegative_literal_comparisons.cpp2),
[`experiments_test.cpp`](../../tests/unit/experiments_test.cpp).
