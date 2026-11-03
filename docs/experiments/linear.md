# `linear`

Enable with `-experiment=linear`.
Explores: a new feature.

## What it enables

The metafunction `@linear` makes a type whose values must be consumed
exactly once on every path. The type is move-only: its copy constructor and
copy assignment are deleted, and a memberwise move constructor and move
assignment are generated when missing.

The translator then follows each local and each parameter of a `@linear`
type declared in this translation unit. A value is consumed only by:

- being a whole argument for a `move`, `copy` or `forward` parameter of a
  function of this translation unit, `release(pool, c)` or
  `pool.release(c)`, including `this` of a UFCS call, `c.close()` for
  `close: (move this)`;
- being a whole initializer, `d: Conn = c;`, or a capture `c$`;
- `return x;`;
- an explicit `move x`.

Passing the value to an `in` or `inout` parameter, reading a member, or
mentioning it in a contract never consumes it. So the chain of owners ends
in a `move this` member function or a function the translator does not
know, reached with `move x`. A path that leaves the value's scope without
consuming it is an error at its declaration, assigning over a value not
yet consumed is an error at the assignment, and a second consumption is an
error where it happens.

## Why it is useful

Some values stand for an obligation rather than data: a connection that
must go back to its pool, a transaction that must be committed or rolled
back, a thread that must be joined. A destructor can release such a value
silently, but cannot choose between commit and rollback, and nothing tells
the programmer that a path forgot to choose. With `@linear`, forgetting is
a translation error, and so is handing the same value on twice.

What it rejects: a `@linear` type with a copy `operator=` (`that` instead
of `move that`), any function that lets a followed value fall out of
scope, including a `move` parameter that is only read, and a function that
leaves an `inout` parameter moved from. A path that ends in `throw` is not checked, so the value is
destroyed normally during unwinding.

## Prior art

- **Rust's ownership** is affine, not linear: a moved-from value cannot be
  used again, but a value may be dropped without being used. This
  experiment adds the other half, that it must be used.
- **Linear Haskell** (the `LinearTypes` extension, GHC 9.0) puts linearity
  on the function arrow, `a %1 -> b`: the function uses its argument
  exactly once. Here linearity belongs to the type, as in the next two.
- **Austral** divides types into a free and a linear universe; a linear
  value must be used exactly once, and passing it to a function consumes
  it. An `in` parameter here plays the role of Austral's borrow.
- **Vale's "higher RAII"** is a type that cannot simply go out of scope
  and must be destroyed explicitly, which is what `linear-not-consumed`
  enforces.

## Example

A connection pool. `query` borrows a connection through an `in`
parameter; `release` consumes one and gives it back to the pool, and the
`move this` member `close` ends one's life.

```cpp
#include <iostream>
#include <string>
#include <vector>

Conn: @linear type = {
    public id: int;
    operator=: (out this, id_: int) = { id = id_; }
    close: (move this) = { std::cout << "close " << id << "\n"; }
}

open: (id: int) -> Conn = {
    c := Conn(id);
    return c;
}

release: (inout pool: std::vector<Conn>, move c: Conn) = {
    std::cout << "release " << c.id << "\n";
    pool.push_back(move c);
}

query: (c: Conn, sql: std::string) -> std::string = "conn " + std::to_string(c.id) + ": " + sql;

main: (args) -> int = {
    pool: std::vector<Conn> = ();
    a := open(1);
    b := open(2);
    std::cout << query(a, "select 1") << "\n";
    if args.ssize() > 1 {
        release(pool, move a);
        b.close();
    } else {
        b.close();
        release(pool, move a);
    }
    c := open(3);
    release(pool, c);
    std::cout << pool.ssize() << " pooled\n";
    return 0;
}
```

The type lowers to a move-only struct with an rvalue-qualified `close`.
The checks produce no code:

```cpp
struct Conn {
int id;
Conn(cpp2::impl::in<int> id_);
auto close()&& -> void;
// ...
Conn(Conn const&) = delete;
auto operator=(Conn const&) -> void = delete;
// ...
};
// ...
        CPP2_UFCS(close)(cpp2::move(b));
```

Output:

```
conn 1: select 1
close 2
release 1
release 3
2 pooled
```

Rejected: a sink that only reads its parameter, a function that leaves an
`inout` connection moved from, a connection released on one path only, one
released twice, and one overwritten before it is consumed.

```cpp
log_and_drop: (move c: Conn) = {                // line 21
    std::cout << "drop " << c.id << "\n";
}

borrow: (inout slot: Conn) = {                  // line 25
    slot.close();
}

main: (args) -> int = {
    pool: std::vector<Conn> = ();
    d := open(4);                               // line 31
    if args.ssize() > 1 {
        release(pool, move d);
    }
    e := open(5);
    release(pool, move e);
    release(pool, move e);                      // line 37
    f := open(6);
    f = open(7);                                // line 39
    f.close();
    return 0;
}
```

```
lbad2.cpp2:21:21: error: Conn c is not consumed on every path [linear-not-consumed]
lbad2.cpp2:25:16: error: Conn slot is moved from and not reassigned on every path [linear-not-consumed]
lbad2.cpp2:31:5: error: Conn d is not consumed on every path [linear-not-consumed]
lbad2.cpp2:37:5: error: e was consumed on line 36 and is consumed again here [linear-consumed-twice]
lbad2.cpp2:39:5: error: Conn f is assigned before it is consumed [linear-not-consumed]
```

Rejected: a copy `operator=`.

```
lcopy.cpp2:4:5: error: a @linear type cannot be copied: operator= must take 'move that', not 'that' [linear-copy]
```

## Rules and diagnostics

- `@linear` deletes the copy constructor and copy assignment and generates
  a missing memberwise move constructor and move assignment.
- Followed values: a local, binding or non-`in` parameter whose type is
  known to be a `@linear` type of this translation unit. The type is known
  as in [`use-after-move`](use-after-move.md#rules-and-diagnostics): written,
  or deduced from an initializer such as `T(...)`, `f(...)` for a function
  of this translation unit returning it, or a linear `y`; a range `for`
  parameter over a `std::vector<T>` and the like has the type `T`.
- Consumption is a whole argument for a `copy`, `move` or `forward`
  parameter of a function of this translation unit (including `this` of a
  UFCS call), a whole initializer or capture, `return x;` or `move x`.
  The receiver of a UFCS call `x.f(c)` binds `this` of a member function
  `f` of the type of `x` when it has one, and otherwise the first parameter
  of the function `f` that lookup finds. Contracts and reads of a part
  never consume.
- A local or a `move`, `copy` or `forward` parameter is consumed exactly
  once on every path to the end of its scope, including `return`, `break`
  and `continue`, and the early return of a hoisted
  [`try-operator`](try-operator.md) `?`. A path ending in `throw` is not
  checked.
- The alternatives of an `inspect` are separate paths, so each may consume
  the value.
- Assigning over a value, `x = ...` or `out x`, before it is consumed is an
  error; assigning after it is consumed starts a new obligation.
- An `inout` or `out` parameter belongs to the caller. It must not be left
  moved from: a consumption must be followed by a new value on every path.
- The analysis is the flow analysis of
  [`use-after-move`](use-after-move.md), which runs for `@linear` values
  without that experiment. A mention after consumption that does not
  consume again is `use-after-move`.

Diagnostics:

- `linear-copy`: the type declares a copy `operator=` taking `that`.
- `linear-not-consumed`: some path leaves the value's scope without
  consuming it (reported at the declaration), the value is assigned over
  before it is consumed, or an `inout` or `out` parameter is left moved
  from.
- `linear-consumed-twice`: a consumption after one on every path, or
  after one on some path ("may have been consumed").
- `use-after-move`, `move-in-loop`: as in `use-after-move`, for a followed
  value.

Full entry: [`docs/EXPERIMENTS.md`](../EXPERIMENTS.md#current-experiments).

Tests: [`linear.cpp2`](../../tests/ports/experiments/linear.cpp2),
[`linear_error.cpp2`](../../tests/ports/experiments/linear_error.cpp2),
[`experiment_linear_test.cpp`](../../tests/unit/experiment_linear_test.cpp).
