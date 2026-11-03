# `reject-postfix-star-template-arguments`

This is the default language ([`LANGUAGE.md` "Types"](../LANGUAGE.md#types)).
Explored: `OPEN_QUESTIONS.md` Y3,
`std::vector<Foo*>` in a template argument.

## What it enables

In Cpp2 a pointer type is written with a prefix star, `*Foo`, and a postfix
star dereferences: `p*` is Cpp1's `*p`. In a template argument the
translator cannot always tell a type from a value, so `std::vector<Foo*>` is
ambiguous. Passed through unchanged, Cpp1 would read it as the pointer type
`Foo*`. Upstream cppfront reads it as a dereference instead.

cppfrontier makes a template argument whose last token ends in a postfix
`*` an error. The diagnostic gives both spellings that have only one
reading: `*Foo` for a pointer type and `(Foo*)` for a dereference.

## Why it is useful

The two readings give the same text different meanings, and neither is
what all programs mean. Programmers coming from C++ write
`std::vector<Node*>` and expect a pointer type, which passing the argument
through would give them, though `Node*` in any other Cpp2 expression is a
dereference. A dereference used as a value argument, such as
`std::array<int, n_ptr*>`, would then pass through as Cpp1 `n_ptr*`, which
the Cpp1 compiler rejects with an unhelpful message. Upstream's reading has
the opposite problem: `std::vector<Node*>` becomes a dereference of a type.
Rejecting the form makes the code mean the same thing to every reader and
every translator.

The cost is that `std::vector<Node*>`, familiar from Cpp1, is an error. The
fix is mechanical and the diagnostic spells it out. A `*` inside an
argument, as in `n * 2`, is not affected.

## Prior art

- **C++ resolves the same ambiguity toward the type**: in a template
  argument, a construct that could be a type-id or an expression is a type-id
  ([[temp.arg]](https://eel.is/c++draft/temp.arg)). Passing the argument
  through behaves like that.
- **Pascal** writes a pointer type `^Integer` and a dereference `p^`, and
  **Zig** writes `*T` and `ptr.*`. In both, the prefix form is the type and
  the postfix form is the dereference, as in Cpp2. These languages parse
  types and expressions in separate contexts, so the ambiguity does not
  arise there.
- **Cpp2's declaration syntax** puts `*` and `const` before the
  type, as in `*const Foo`, and `LANGUAGE.md` "Types" makes template arguments
  use Cpp2 type syntax. This rule enforces it for the one form the
  translator cannot classify without context.

## Example

```cpp
#include <array>
#include <vector>

Node: @struct type = { value: int = 0; }

capacity: int == 4;
capacity_ptr: *const int == capacity&;

main: () = {
    a: Node = (1);
    b: Node = (2);
    nodes: std::vector<Node*> = (a&, b&);
    buffer: std::array<int, capacity_ptr*> = ();
    for nodes do (n) std::cout << n*.value << " ";
    std::cout << buffer.size() << "\n";
}
```

The translator rejects both arguments:

```
ps.cpp2:12:24: error: 'Node*' is a dereference in Cpp2; write '*Node' for a pointer type, or '(Node*)' for a dereference [postfix-star-in-template-argument]
ps.cpp2:13:29: error: 'capacity_ptr*' is a dereference in Cpp2; write '*capacity_ptr' for a pointer type, or '(capacity_ptr*)' for a dereference [postfix-star-in-template-argument]
```

Upstream cppfront reads both as dereferences, and the first one becomes
invalid Cpp1:

```cpp
    std::vector<*cpp2::impl::assert_not_null(Node)> nodes {&a, &b};
    std::array<int,*cpp2::impl::assert_not_null(capacity_ptr)> buffer {};
```
```
ps.cpp2:12:51: error: template argument 1 is invalid
```

With `std::vector<*Node>` and `std::array<int, (capacity_ptr*)>`, the
program translates:

```cpp
    std::vector<Node*> nodes {&(a), &(b)};
    std::array<int, (*cpp2::impl::assert_not_null(capacity_ptr))> buffer {};
```
```
1 2 4
```

## Rules and diagnostics

- The check covers each top-level argument of a template argument list
  `name<...>`, and the default of a type template parameter, `<T = X>` or
  `<T: type = X>`. It does not apply to `operator<` or to the group and
  flags of `assert`, `pre` and `post`, which are values.
- An argument whose last token ends in `*` (`Foo*`, `V**`, `ns::T*`) is
  rejected. Parentheses make the dereference explicit and are accepted.
- An argument that is a Cpp1 declarator, such as `const Foo*` or `int*`,
  gets `cpp1-declarator-in-template-argument` instead.
- `postfix-star-in-template-argument`: a template argument ends in a
  postfix `*`. The message gives the pointer-type and the dereference
  spellings.

Full entry: [`EXPERIMENTS.md`](../EXPERIMENTS.md#part-of-the-default-language).

Tests:
[`postfix_star_template_arguments.cpp2`](../../tests/ports/syntax/postfix_star_template_arguments.cpp2),
[`postfix_star_template_arguments_error.cpp2`](../../tests/ports/syntax/postfix_star_template_arguments_error.cpp2),
[`cpp2_validation_test.cpp`](../../tests/unit/cpp2_validation_test.cpp).
