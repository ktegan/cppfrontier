# `unsafe`

Enable with `-experiment=unsafe`.
Explores: a new feature.

## What it enables

`unsafe { ... }` inside a function body is a block translated as if the
`-no-null-checks`, `-no-div-zero-checks`, `-no-subscript-checks` and
`-no-comparison-checks` flags were given for it alone: subscripts are not
bounds-checked, `p*` is not null-checked, `/` and `%` do not check for a
zero divisor, and comparisons are plain Cpp1 comparisons, so a mixed-sign
comparison is accepted. With [`checked-arithmetic`](checked-arithmetic.md)
also enabled, its arithmetic is unchecked too. This covers everything
lexically inside the block, including function expressions and string
interpolations. The checks resume after the closing `}`.

Otherwise the block is an ordinary block. It lowers to a plain Cpp1 block
marked `// unsafe`, its names are local to it, and moves and definite
initialization work in it as in any block.

## Why it is useful

Cpp2 checks every subscript, dereference and division by default. The only
ways to turn a check off are the translator flags (`-no-subscript-checks` and
the others), which apply to the whole file, or rewriting each expression,
for example as Cpp1 text or with a `cpp2::unchecked_*` function. A hot loop
whose bounds are already validated is a common reason to want the checks
off, and it usually needs them off for a few lines, not a file.

An `unsafe` block scopes the opt-out to those lines and marks them in both
the Cpp2 source and the generated Cpp1, so a reviewer can find every place
where the checks are off. The cost is the usual one: inside the block an
out-of-range subscript or a null dereference is undefined behavior again.

## Prior art

- **Rust's `unsafe` blocks** use the same keyword and block form, but mean
  something else. A Rust `unsafe` block permits extra operations, such as
  dereferencing a raw pointer; it does not remove checks, and slice indexing
  is still bounds-checked inside it. Here nothing new is permitted; existing
  checks are dropped.
- **C#'s `unchecked` block** turns off integer overflow checking for the
  statements inside it, a scoped opt-out like this one but for arithmetic
  only. C#'s `unsafe` context, like Rust's, permits pointer operations.
- **Ada's `pragma Suppress`** (for example `Index_Check` or
  `Division_Check`) removes named runtime checks for a declarative region.
  It is closer in meaning and finer-grained: it names the checks to drop,
  where `unsafe` drops all of them.
- **cppfront** has the file-wide `-no-*-checks` flags and per-expression
  functions such as `cpp2::unchecked_cmp_less` and `unchecked_narrow`. The
  block form is new to this project.

## Example

```cpp
#include <iostream>
#include <vector>

mean_product: (a: std::vector<int>, b: std::vector<int>) -> int = {
    assert(a.ssize() == b.ssize());
    sum := 0;
    unsafe {
        i := 0;
        while i < a.ssize() next i++ { sum += a[i] * b[i]; }
    }
    return sum / a.ssize();
}

main: () = {
    a: std::vector<int> = (1, 2, 3);
    b: std::vector<int> = (4, 5, 6);
    std::cout << mean_product(a, b) << "\n";
    e: std::vector<int> = ();
    std::cout << mean_product(e, e) << "\n";
}
```

Inside the block the subscripts and the comparison are plain Cpp1; the
division after it is checked again:

```cpp
    auto sum {0};
    { // unsafe
        auto i {0};
        for (; i < CPP2_UFCS(ssize)(a); ++i) {
            sum += a[i] * b[i];
        }
    }
    return cpp2::move(sum) / CPP2_ASSERT_NOT_ZERO(CPP2_TYPEOF(cpp2::move(sum)),CPP2_UFCS(ssize)(a));
```

Without the block, the loop body would be
`sum += CPP2_ASSERT_IN_BOUNDS(a, i) * CPP2_ASSERT_IN_BOUNDS(b, i);` and the
condition `cpp2::impl::cmp_less(i,CPP2_UFCS(ssize)(a))`.

Output. The second call divides by zero, which the check after the block
catches (exit status 1):

```
10
x.cpp(18) int mean_product(cpp2::impl::in<std::vector<int> >, cpp2::impl::in<std::vector<int> >): Type safety violation: integer division by zero attempt detected
```

## Grammar

```
statement:
    ...
    unsafe-block               // added

unsafe-block:
    'unsafe' compound-statement
```

`unsafe-block` hooks into `statement` in the block parser
(`src/statement_parser.cpp`, `parse_unsafe`).

`unsafe` is a contextual keyword. It is lexed as an identifier, and it
begins a block only at the start of a statement, followed by `{`, while the
experiment is enabled. Anywhere else, as in `unsafe := 1;` or
`f(unsafe)`, it is an ordinary name, with or without the experiment.
The line classifier (`src/source.cpp`) treats a file-scope line beginning
`unsafe {` as Cpp2, so that it is diagnosed instead of passed through.

## Rules and diagnostics

- The block turns off exactly what the four `-no-*-checks` flags turn off,
  plus the `checked-arithmetic` checks, for everything lexically inside it:
  nested blocks, function expressions and string interpolations. Nested
  `unsafe` blocks are allowed.
- That includes the translator's mixed-sign comparison check: `i < u` with
  `i: int` and `u: u32` is an error outside the block and a plain Cpp1 `<`
  inside it. Other diagnostics, such as definite initialization, still
  apply.
- The block is a scope like `{ ... }`: names declared in it end at its `}`,
  and last-use moves and definite initialization treat it as a block.

Diagnostics:

- `unsafe-scope`: an `unsafe { ... }` at file, namespace or type scope, or
  as a function's whole body (`k: () = unsafe { m(); };`).

Full entry: [`docs/EXPERIMENTS.md`](../EXPERIMENTS.md#current-experiments).

Tests: [`unsafe.cpp2`](../../tests/ports/experiments/unsafe.cpp2),
[`experiment_unsafe_test.cpp`](../../tests/unit/experiment_unsafe_test.cpp).
