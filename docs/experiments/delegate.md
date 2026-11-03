# `delegate`

Enable with `-experiment=delegate`.
Explores: a new feature.

## What it enables

`@delegate<member>` on a type generates a forwarding function for each
public function of the data member's type: same name, template
parameters, parameters and return form, with a body that calls
`this.member.f(args...)`. Functions with an `in` or `inout` `this` are
forwarded. Operators and special member functions are not.

A function the type declares itself is kept, so the type can override what
it delegates. `@delegate<member, f, g>` forwards only `f` and `g`. A type can
have several `@delegate`s for different members.

## Why it is useful

Composition is often preferred to inheritance. The cost is boilerplate: a
type that wraps another and wants to expose most of its interface needs one
hand-written forwarding function per function, and the list has to be
kept up to date as the wrapped type changes. `@delegate` generates that list
and keeps it in step. Unlike public inheritance, the wrapper does not
convert to the member's type, and it can replace any forwarded function
with its own.

The generated functions are ordinary IR members of the type, so later
stages treat them like written ones and they appear in the lowered Cpp1.

The limits: the member's type must be a non-template type declared in this
translation unit, since the translator has no Cpp1 type information. A
`std::vector` member cannot be delegated to.

## Prior art

- **Kotlin's class delegation**, `class Derived(b: Base) : Base by b`,
  generates forwarding methods for an interface, and methods the class
  overrides are kept. Kotlin delegates an interface. `@delegate` delegates
  every public function of a concrete type, or a named subset.
- **Go's struct embedding** promotes the methods of an embedded field to
  the outer struct, and the outer struct's own methods take precedence. Go
  reports an ambiguous name only where it is used. `@delegate` reports it
  at the type, as `delegate-ambiguous`.
- **Rust** has no built-in form. Crates such as `delegate` generate the
  forwarding methods with macros, naming each one.
- **Metafunctions** (Herb Sutter, P0707) generate members from a type's
  declaration. `@delegate` is one, applied to the type of a data member.

## Example

```cpp
#include <map>

Inventory: type = {
    items: std::map<std::string, int> = ();
    operator=: (out this) = { }
    add: (inout this, name: std::string, n: int) = { items[name] += n; }
    count: (this, name: std::string) -> int = {
        it := items.find(name);
        if it == items.end() { return 0; }
        return it*.second;
    }
    kinds: (this) -> std::size_t = items.size();
}

// An inventory that also prints every change.
AuditedInventory: @delegate<inventory> type = {
    inventory: Inventory = ();
    operator=: (out this) = { }
    add: (inout this, name: std::string, n: int) = {
        std::cout << "add " << n << " " << name << "\n";
        inventory.add(name, n);
    }
}

main: () = {
    stock: AuditedInventory = ();
    stock.add("bolt", 40);
    stock.add("nut", 25);
    stock.add("bolt", 10);
    std::cout << stock.count("bolt") << " bolts, " << stock.kinds() << " kinds\n";
}
```

`add` is the type's own. `count` and `kinds` are generated:

```cpp
[[nodiscard]] auto AuditedInventory::count(cpp2::impl::in<std::string> name) const& -> int  {
    return CPP2_UFCS(count)((*this).inventory, name);
}

[[nodiscard]] auto AuditedInventory::kinds() const& -> std::size_t  {
    return CPP2_UFCS(kinds)((*this).inventory);
}
```

Output:

```
add 40 bolt
add 25 nut
add 10 bolt
50 bolts, 2 kinds
```

Rejected:

```cpp
Engine: type = { start: (inout this) = { } stop: (inout this) = { } }
Radio: type = { start: (inout this) = { } tune: (inout this, f: double) = { } }
Car: @delegate<engine> @delegate<radio> type = {
    engine: Engine = ();
    radio: Radio = ();
}
Playlist: @delegate<songs> type = { songs: std::vector<std::string> = (); }
```
```
dbad.cpp2:3:1: error: 'start' is forwarded by more than one @delegate of 'Car' [delegate-ambiguous]
dbad.cpp2:7:1: error: the type 'std::vector<std::string>' of 'songs' is not declared in this translation unit [delegate-unknown-type]
```

`@delegate<radio, tune>` resolves the first error, and so does a `start`
that `Car` declares itself.

## Rules and diagnostics

### Accepted form

```
delegate-metafunction:
    '@delegate' '<' identifier '>'
    '@delegate' '<' identifier ( ',' identifier )+ '>'
```

The first identifier names a data member of the type, not a base (`this`).
Any others name functions of the member's type.

- Forwarded: each public function of the member's type whose `this` is `in`
  or `inout`, except operators and special member functions. The forwarding
  function has the same template parameters, parameters, `this` passing and
  return form, and calls `this.member.f<Ts>(args...)`. An expression body
  forwards as an expression body, so a deduced return type is deduced the
  same way.
- A name in a parameter type, the return type or a default value that
  names a type, namespace or alias declared in the member type's scopes is
  qualified from the global namespace, as in `::lib::Counter::Kind`. A
  function with a named return list returns the member type's `f_ret`:
  `minmax: (this, v: std::vector<int>) -> (lo: int, hi: int)` forwards as
  `auto Wrapper::minmax(...) const& -> ::Stats::minmax_ret`.
- Not forwarded: a name the type already declares. The check is by name,
  so one declaration hides every overload of that name.
- The member's type is found by name lookup from the type: a nested type,
  a namespaced type or one declared later in the file.
- `delegate-unknown-member`: no member is named, or the type has no data
  member of that name.
- `delegate-unknown-type`: the member's type is not declared in this
  translation unit.
- `delegate-template-type`: the member's type is a template.
- `delegate-unknown-function`: a selected name is not a function of the
  member's type.
- `delegate-not-forwardable`: a selected function cannot be forwarded,
  such as a static or private one.
- `empty-metaclass-argument`: the argument list has an empty entry, as in
  `@delegate<m, , f>`.
- `delegate-ambiguous`: two `@delegate`s would forward the same name.

Full entry: [`EXPERIMENTS.md`](../EXPERIMENTS.md#current-experiments).

Tests:
[`delegate.cpp2`](../../tests/ports/experiments/delegate.cpp2),
[`delegate_forms.cpp2`](../../tests/ports/experiments/delegate_forms.cpp2),
[`delegate_error.cpp2`](../../tests/ports/experiments/delegate_error.cpp2),
[`experiment_delegate_test.cpp`](../../tests/unit/experiment_delegate_test.cpp).
