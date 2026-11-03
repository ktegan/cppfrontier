# `move-only-to-known-callees`

Enable with `-experiment=move-only-to-known-callees`.
Explores: [`LANGUAGE.md` "Moves and last use"](../LANGUAGE.md#moves-and-last-use),
the alternative to moving into a non-const lvalue reference parameter.

## What it enables

Cpp2 moves from a local or a `copy`/`move` parameter at its definite last
use. By default the translator moves every such use, including a whole call
argument, without knowing what the callee does with it. When the callee
takes a non-const lvalue reference, as `std::exchange(x, 0)` or a Cpp2
`inout` parameter does, the moved argument cannot bind and the Cpp1 does not
compile. The workaround is a later mention, `_ = x;`.

With the experiment, a last use that is a whole call argument, `f(x)`, is
moved only when the callee is known to take it by value or rvalue
reference: `f` names functions of this program, and every overload viable
for the argument count has a `copy`, `move` or `forward` parameter in that
position. Under UFCS the object of `x.f()` is the first argument: it is
moved when `f` is a member function of `x`'s type that takes `move this`,
or a function whose first parameter is `copy`, `move` or `forward`. Any
other call argument is a use that is not moved. `move x` still asks for a
move. A last use that is
not a call argument, such as `x + y`, `y := x;` or `return x;`, still
moves.

## Why it is useful

The default rule breaks code that is correct: passing a local stream to a
function with an `inout` parameter, as the last thing done with the stream,
is an error in the generated Cpp1. The experiment moves only where moving is
known to be what the callee wants, so it never turns a valid call into one
that does not compile, and it never picks an rvalue overload of an unknown
callee that the programmer did not ask for.

The cost is fewer implicit moves. `v.push_back(x)` and calls into Cpp1
libraries copy their last-use argument unless the code says `move x`. The
copy is never wrong, but it may be slower, and with a move-only type it
does not compile until `move` is added.

## Prior art

- **Rust** decides from the callee's signature: passing a non-`Copy`
  value to a by-value parameter moves it, and `&mut x` borrows it. Rust
  always knows the signature. The experiment knows it only for Cpp2
  functions of the same program and does not move otherwise.
- **C++'s implicit move** (P1825, P2266) applies only to `return` and
  `throw` operands. The language never moves a call argument implicitly,
  because the callee might not expect it.
- **Swift's `consuming` and `borrowing` parameter modifiers** (SE-0377)
  state in the callee's signature whether a call takes ownership. That is
  the information the experiment reads from Cpp2's `copy`, `move` and
  `forward` passing styles.

## Example

```cpp
#include <fstream>

Message: type = {
    operator=: (out this) = { }
    operator=: (out this, that) = { std::cout << "copy "; }
    operator=: (out this, move that) = { std::cout << "move "; }
}

archive: (copy m: Message) = { }
send: (copy m: Message) = archive(m);
store: (copy m: Message, inout box: std::vector<Message>) = box.push_back(m);

write_report: (inout out: std::ostream, count: std::size_t) = {
    out << "messages: " << count << "\n";
}

main: () = {
    m: Message = ();
    box: std::vector<Message> = ();
    box.reserve(1);
    send(m);
    std::cout << "| ";
    store(m, box);
    std::cout << "\n";
    file: std::ofstream = ("report.txt");
    write_report(file, box.size());
}
```

Without the flag, every last use moves. The move of `file` into the
`inout` parameter does not compile:

```cpp
auto send(Message m) -> decltype(auto) { return archive(cpp2::move(m)); }
auto store(Message m, std::vector<Message>& box) -> decltype(auto) { return CPP2_UFCS(push_back)(box, cpp2::move(m)); }
// ...
    write_report(cpp2::move(file), CPP2_UFCS(size)(cpp2::move(box)));
```
```
mk.cpp:54:28: error: cannot bind non-const lvalue reference of type ‘std::ostream&’ {aka ‘std::basic_ostream<char>&’} to an rvalue of type ‘std::basic_ostream<char>’
```

With the flag, `archive` is known to take `m` by `copy`, so `send` still
moves. `push_back` is not known, so `store` copies. `file` and `box` are
passed as lvalues:

```cpp
auto send(Message m) -> decltype(auto) { return archive(cpp2::move(m)); }
auto store(Message m, std::vector<Message>& box) -> decltype(auto) { return CPP2_UFCS(push_back)(box, m); }
// ...
    write_report(file, CPP2_UFCS(size)(box));
```
```
copy move | move copy
```

`report.txt` contains `messages: 1`. Writing `box.push_back(move m)` moves
in `store` again.

## Rules and diagnostics

- A definite last use that is a whole argument of a call `f(..., x, ...)` is
  moved only when `f` finds functions of this program and every overload
  whose parameter count fits the call takes that position by `copy`,
  `move` or `forward`, with at least one such overload. Default arguments
  and a trailing pack count when the overloads are matched.
- The object `x` of a UFCS call `x.f()` is argument 0. It binds `this` of
  a member function `f` of the type of `x` when that type is known and has
  one, else the first parameter of the functions `f` that unqualified
  lookup finds, so a `move this` member function such as
  `release: (move this)` moves it.
- Otherwise the mention is a use that is not moved: a Cpp1 callee, a
  member function name, an `in`, `inout` or `out` parameter (or `this`),
  or overloads that disagree.
- `move x` moves explicitly. A last use outside a call argument moves as by
  default.
- The experiment adds no diagnostics.

Full entry: [`EXPERIMENTS.md`](../EXPERIMENTS.md#current-experiments).

Tests:
[`move_only_to_known_callees.cpp2`](../../tests/ports/experiments/move_only_to_known_callees.cpp2),
[`move_only_to_known_callees_default.cpp2`](../../tests/ports/experiments/move_only_to_known_callees_default.cpp2),
[`experiments_test.cpp`](../../tests/unit/experiments_test.cpp).
