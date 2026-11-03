# `cpp2-requires-parameters`

This is the default language ([`LANGUAGE.md` "Functions and parameters"](../LANGUAGE.md#functions-and-parameters)).
Explored: `OPEN_QUESTIONS.md` Y4,
requires-expressions.

## What it enables

cppfrontier accepts C++20 requires-expressions in Cpp2 code, such as the
body of a concept. Their parameters use Cpp2 syntax, `requires (inout a: T,
b: T) { a += b; }`, and the passing style gives the value category of the
parameter: `in` (the default) is `T const&`, `inout` and `out` are `T&`,
`move` and `forward` are `T&&`, and `copy` is `T`. A Cpp1 parameter, as
in `requires (T& a, T const& b)`, is an error. The alternative, passing the
parameter list through to Cpp1 as written, would make it the only place in
Cpp2 code where a parameter is written the Cpp1 way.

## Why it is useful

Cpp2 code uses one declaration syntax, `name: type`, everywhere else. Inside
a requires-expression a reader has to switch to Cpp1 declarators, where
`&`, `&&` and `const` placement decide what the requirement tests. The
passing styles say the same thing in the words Cpp2 functions already use:
`inout c: C` asks whether the operations work on a modifiable lvalue, and
`move m: T` on a named rvalue reference.

Requires-expressions copied from Cpp1 code need their parameter lists
rewritten. Each parameter is a mechanical change.

## Prior art

- **C++20 requires-expressions** (added with concepts in
  [P0734](https://wg21.link/p0734)) take a Cpp1
  parameter-declaration-clause. The parameters are never initialized and
  only declare names with a type and value category for the requirements. cppfrontier lowers to exactly that form.
- **Cpp2's parameter passing styles** (`in`, `inout`, `out`, `copy`,
  `move`, `forward`, in the Cpp2 documentation on functions) describe a
  parameter by what the function does with it. This rule reuses them
  for a parameter that exists only to be type-checked.

Writing requires-expression parameters in Cpp2 syntax is specific to this
project. Upstream cppfront does not parse requires-expressions in Cpp2
code.

## Example

```cpp
#include <memory>

// A container that can grow at the back and be measured.
growable: <C> concept = requires (inout c: C, v: C::value_type, move m: C::value_type) {
    c.push_back(v);
    c.push_back(m);
    { c.size() } -> std::convertible_to<std::size_t>;
};

// Copyable from a named rvalue reference, which is an lvalue.
copies_named: <T> concept = requires (move a: T) { T(a); };

main: () = {
    std::cout << growable<std::vector<int>> << growable<std::string> << growable<int> << "\n";
    std::cout << copies_named<std::string> << copies_named<std::unique_ptr<int>> << "\n";
}
```

It lowers to:

```cpp
template<typename C> concept growable = requires (C& c, typename C::value_type const& v, typename C::value_type&& m) {
// ...
template<typename T> concept copies_named = requires (T&& a) { T(a); };
```
```
110
10
```

Upstream cppfront does not translate the program:

```
d.cpp2(2,0): error: parse failed for section starting here
```

A parameter written the Cpp1 way is rejected:

```cpp
addable: <T> concept = requires (T a, T b) { a + b; };
```
```
c1.cpp2:1:34: error: a requires-expression parameter uses Cpp2 syntax, as in 'a: T' or 'inout a: T' [cpp1-requires-parameter]
```

## Grammar

A requires-expression is a primary expression in cppfrontier, a
form not in upstream's `parse.h`. Its parameters are Cpp2 parameters:

```
primary-expression:
    ...
    requires-expression

requires-expression:
    'requires' requires-parameter-list? '{' requirement-seq? '}'

requires-parameter-list:
    '(' requires-parameter-seq? ')'

requires-parameter-seq:
    requires-parameter
    requires-parameter-seq ',' requires-parameter

requires-parameter:
    parameter-direction? identifier ':' type-id      // not a Cpp1 parameter-declaration
```

`parameter-direction` is the one in `parse.h` (`in`, `copy`, `inout`,
`out`, `move`, `forward`), plus `in_ref`, which is lowered like `in`.
The `type-id` may be a function type, `f: (n: int) -> T`, which is
declared as written (`T f(cpp2::impl::in<int> n)`) without a reference.
Lexing does not change.

## Rules and diagnostics

- Each parameter is `direction? name: type`. The direction gives the Cpp1
  declarator: none or `in` or `in_ref` is `T const&`, `inout` and `out` are
  `T&`, `move` and `forward` are `T&&`, `copy` is `T`.
- `requires { ... }` without parameters is lowered as written.
- A requires-expression may begin a requires clause, `requires requires
  (a: T) { ... }`, and be an `if constexpr` condition.
- `cpp1-requires-parameter`: a parameter of a requires-expression is not
  in Cpp2 form, such as `T a`.

Full entry: [`EXPERIMENTS.md`](../EXPERIMENTS.md#part-of-the-default-language).

Tests:
[`requires_parameters.cpp2`](../../tests/ports/language/requires_parameters.cpp2),
[`requires_parameters_error.cpp2`](../../tests/ports/language/requires_parameters_error.cpp2),
[`cpp2_validation_test.cpp`](../../tests/unit/cpp2_validation_test.cpp).
