# `exclusivity`

Enable with `-experiment=exclusivity`.
Explores: a new feature.

## What it enables

A check that a call does not pass an object `inout` or `out` while another
argument of the same call also mentions that object, or a part of it. In
`append(a, a)`, where `append` takes its first parameter `inout`, the
second argument reads `a` while the callee may be changing it, and the call
is rejected.

The passing comes from the callee's declaration, since Cpp2 does not mark
`inout` at the call site. So the check covers calls to functions declared
in this translation unit, and skips calls it cannot resolve. It only adds
diagnostics; the Cpp1 is unchanged.

## Why it is useful

An `inout` parameter promises the callee that it may modify the object.
When another parameter refers to the same object, the callee's reads and
writes interfere: a loop over `src` that appends to `dst` invalidates its
own iterators when both are the same vector, which is undefined behavior.
A function written for distinct arguments rarely guards against this,
and the bug depends on the caller's arguments, not the function's code.

The cost: some calls that would be harmless are rejected, such as passing
`x` both `inout` and as a small value copied before the call. The fix is to
make the copy explicit, as `snapshot` below does. Calls to library
functions, including UFCS calls on a library type, are not checked.

## Prior art

- **Swift's Law of Exclusivity** (SE-0176, Swift 4) forbids overlapping
  accesses to a variable unless both are reads. Passing a variable `inout`
  twice, `swap(&x, &x)`, or reading it while a mutating access is in
  progress, is diagnosed at compile time for locals and enforced at run
  time for class properties and globals. This experiment is the static,
  per-call part of that rule.
- **Rust's aliasing XOR mutability** rule: while a `&mut` borrow of a value
  exists, no other reference to it may be used, so `f(&mut v, &v)` does not
  compile. Rust applies it everywhere through lifetimes; here it applies to
  the arguments of one call.

Cpp2's parameter passing makes the check possible: `inout` and `out` say
which arguments are written, so the translator knows what Cpp1 can only
see as a non-const reference.

## Example

```cpp
#include <iostream>
#include <vector>

append: (inout dst: std::vector<int>, src: std::vector<int>) = {
    for src do (x) {
        dst.push_back(x);
    }
}

main: () = {
    a: std::vector<int> = (1, 2);
    b: std::vector<int> = (3, 4);
    append(a, b);
    snapshot := a;
    append(a, snapshot);
    std::cout << a.ssize() << " " << b.ssize() << "\n";
}
```

Output:

```
8 2
```

Rejected: appending a vector to itself.

```cpp
main: () = {
    a: std::vector<int> = (1, 2);
    append(a, a);
    _ = a.ssize();
}
```

```
bad.cpp2:11:15: error: 'a' is passed inout and also used by another argument of the same call [exclusivity-violation]
```

Without the flag this call translates and, at run time, pushes into the
vector it is iterating over.

## Rules and diagnostics

- The check applies to a call whose callee resolves to functions declared
  in this translation unit. When the callee is overloaded, every overload
  viable for the argument count must make the argument `inout` or `out`;
  the diagnostic says `out` when each makes it `out`. A call it cannot
  resolve, such as `lib(out x, x.size())`, is not checked.
- Objects are locals, parameters, members written `m` or `this.m`, and
  `this` itself. An argument binds an object as a whole when it is a name
  or a member or subscript path rooted at one, such as `p.x` or `v[0]`.
- Distinct parts of an object are disjoint: `add(p.a, p.b)`,
  `add(p.a.x, p.a.y)`, `addb(p.b, p.a < 3)` and, inside a member function,
  `add(m, n)` are accepted. A part is the member path `p.a.x` of the
  expression tree. A part conflicts with itself, with the parts it contains
  and with the whole object: `add(p.a, p.a)`, `add(p.a.x, p.a)` and
  `add(p.a, p)` conflict, and `this` conflicts with each of its members.
- The receiver of a member or UFCS call, `x.f()` or `(x).f()`, is the
  first argument, so `t.bump(t.m)` conflicts when `bump` takes
  `inout this`. Inside a member function, an unqualified call to another
  member takes the implicit `this` as its receiver. A qualified callee,
  `N::g(n, n)`, is checked like an unqualified one. The receiver binds
  `this` of a member function of its type when it has one, and otherwise
  the first parameter of the function that lookup finds, as in
  [`linear`](linear.md#rules-and-diagnostics). Its type is known as in
  [`use-after-move`](use-after-move.md#rules-and-diagnostics), so a
  deduced `q := P();` is a `P`.
- The texts of a statement are checked once each: an expression, an
  initializer, including a named return value's and a destructuring
  declaration's, a condition and a statement parameter's initializer.
- Any mention in another argument counts, at any depth, including a
  capture `n$` in a lambda and a string interpolation: `add(x, x * 2)`,
  `add(v[0], size(v))` and `apply(n, :(v) -> int = v + n$)` all conflict.
  A mention in a compile-time context does not: `add(n, sizeof(n))`, and
  mentions inside `decltype` or template arguments, are accepted. The
  diagnostic is at the later argument.

Diagnostics:

- `exclusivity-violation`: an object passed `inout` or `out` is also
  mentioned by another argument of the same call.

Full entry: [`docs/EXPERIMENTS.md`](../EXPERIMENTS.md#current-experiments).

Tests: [`exclusivity_error.cpp2`](../../tests/ports/experiments/exclusivity_error.cpp2),
[`experiment_exclusivity_test.cpp`](../../tests/unit/experiment_exclusivity_test.cpp).
