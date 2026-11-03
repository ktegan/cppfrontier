# `forward-return-bare-names`

A default rule covers this experiment ([`LANGUAGE.md` "Returns"](../LANGUAGE.md#returns)).

Explored: `OPEN_QUESTIONS.md` Q15, returning a local through `-> forward`.

## What it enables

A `-> forward` function returns a reference, so it must not return a
reference into a local or an `in`, `copy` or `move` parameter, which die
when the function returns. The translator follows the returned expression
from its leading name through member accesses, subscripts, calls, postfix
operators and parentheses, up to a postfix `*`, which dereferences and so
leaves the object. For a local `s` or `v`, `return s;`, `return s.x;`,
`return v[0];` and `return s.get();` refer into it. `return it*.second;`,
`return (p*).x;` and `return p*;` do not, so the result can be a reference
through a local iterator or a pointer parameter.

Upstream cppfront checks the leading name only: it rejects every postfix
chain that starts with a local or such a parameter, such as `it*.second`,
`p*` or `s.name`. The bare-name alternative, which names this page,
checks only a returned name, `return x;`, and accepts every chain.

## Why it is useful

Upstream's rule rejects common code that is correct. A local iterator, or
a pointer parameter, refers to an object that outlives the function, and
dereferencing it is how you reach that object. Upstream's workaround is
parentheses, `return (it*).second;`. They change nothing in the generated
Cpp1, and only hide the leading name from the check.

Checking only a returned name accepts that code, but it also accepts
chains that do dangle: `return v.front();` for a local `v`, and neither GCC
nor Clang warns about the Cpp1 it lowers to. Stopping at a postfix `*`
separates the two in the common cases: a dereference leaves the object,
while a member access, subscript or call stays in it. The translator has no
type information, so the rule is still an approximation: `return s[0];` for
a local `std::span` `s` is rejected though the element outlives the
function, and `return p*;` for a local pointer `p` to a local is accepted.

## Prior art

- **Rust's borrow checker** decides from lifetimes, not syntax: returning
  a reference obtained through an iterator over a `&mut` parameter is
  accepted, and returning a reference into a local `Vec` is rejected. Each
  translator rule approximates what such an analysis would decide.
- **The C++ lifetime profile** (Herb Sutter,
  [P1179](https://wg21.link/p1179)) tracks which objects a pointer or
  reference may point to through function calls, and would tell
  `it*.second` from `v.front()`.
- **GCC's `-Wreturn-local-addr` and Clang's `-Wreturn-stack-address`**
  warn when a function returns a reference or pointer to its own local.
  Like the bare-name alternative, they catch direct cases and miss cases
  that go through a call.

## Example

```cpp
#include <map>

// The count stored for `word`, inserting a zero first if needed.
slot: (inout counts: std::map<std::string, int>, word: std::string) -> forward int = {
    it := counts.try_emplace(word, 0).first;
    return it*.second;
}

main: () = {
    counts: std::map<std::string, int> = ();
    words: std::vector<std::string> = ("a", "b", "a");
    for words do (w) {
        slot(counts, w)++;
    }
    for counts do (entry) {
        std::cout << entry.first << "=" << entry.second << " ";
    }
    std::cout << "\n";
}
```

The translator accepts it:

```cpp
[[nodiscard]] auto slot(std::map<std::string, int>& counts, cpp2::impl::in<std::string> word) -> int&  {
    auto it {CPP2_UFCS(try_emplace)(counts, word, 0).first};
    return (*cpp2::impl::assert_not_null(cpp2::move(it))).second;
}
```
```
a=2 b=1
```

Upstream cppfront rejects it, because the returned expression starts with
the local `it`:

```
fr.cpp2(6,12): error: a 'forward' return type cannot return a local variable
```

Upstream accepts `return (it*).second;`, which gives the same Cpp1.

A return that refers into a local is rejected:

```cpp
first: () -> forward int = {
    v: std::vector<int> = (1, 2, 3);
    return v.front();
}
```
```
fv.cpp2:3:5: error: a 'forward' return cannot return a reference into a local variable or an 'in', 'copy', or 'move' parameter [forward-return-reference]
```

## Rules and diagnostics

- A returned expression refers into the object its leading name names,
  followed through member accesses, subscripts, calls, postfix operators
  and parentheses, up to a postfix `*`.
- In a `-> forward T` or `-> forward_ref _` function, a `return` statement
  or an expression body that refers into a non-static local or an `in`,
  `copy` or `move` parameter is rejected.
- In a `-> forward _` function or an expression body without a return
  type, which deduce a value or a reference from the returned expression's `decltype`, a return into a
  local or a `copy` or `move` parameter is checked by the generated Cpp1,
  `static_assert(!std::is_reference_v<decltype(expr)>, ...)`. So `return s;`
  for a local `std::string s` returns a copy, and `return s.front();` is
  rejected when the Cpp1 is compiled. An `in` parameter is the caller's
  object, or a copy returned by value, and is accepted.
- `inout`, `out` and `forward` parameters can be returned.
- `forward-return-reference`: a return in a `-> forward T` or
  `-> forward_ref _` function refers into a local or an `in`, `copy` or
  `move` parameter.

Full entry: [`EXPERIMENTS.md`](../EXPERIMENTS.md#part-of-the-default-language).

Tests:
[`forward_return_bare_names.cpp2`](../../tests/ports/experiments/forward_return_bare_names.cpp2),
[`forward_return_bare_names_default.cpp2`](../../tests/ports/experiments/forward_return_bare_names_default.cpp2),
[`pipeline_test.cpp`](../../tests/unit/pipeline_test.cpp).
