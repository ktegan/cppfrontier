# `newtype`

Enable with `-experiment=newtype`.
Explores: a new feature.

## What it enables

Three metafunctions that make a distinct type over an underlying type `U`,
so that two values with the same representation but different meanings
cannot be mixed up:

- `@newtype<U>` holds one `U`. It is constructed explicitly from a `U`
  (`7 as Count` or `Count(7)`), converts back only explicitly (`c as i32`),
  can be copied and is compared with `<=>`. It has no arithmetic and no
  assignment from `U`: `c = 5;` does not compile, `c = Count(5);` does.
- `@quantity<U>` adds the arithmetic of one quantity: `+`, `-`, `+=`,
  `-=` and unary `-` between values of the same type, `*` and `/` by a
  `U` (also `k * q`), and the ratio of two values, which is a `U`. A
  quantity may be nested in a type, as `Shop::Price`.
- `@unit<U, L, M, T, ...>` makes the type an alias of
  `cpp2::quantity<U, cpp2::dims<...>>` from `runtime/cpp2units.h`. The
  arguments after `U` are the exponents of the SI base dimensions, so `*`
  and `/` produce the right unit: metres divided by seconds is a speed.

Converting with `as` from one such type to another is a translator error.

## Why it is useful

Functions that take several values of one underlying type, such as
`cancel(user: i64, order: i64)`, accept their arguments in the wrong order
without complaint. Wrapping each in its own type turns that mistake into a
compile error, and the generated code adds no runtime cost over the
underlying type. Units go further: adding metres to seconds, or passing a
length where an area is expected, no longer compiles.

Writing such a type by hand takes an explicit constructor, an explicit
conversion, comparisons and, for a quantity, ten operators. The
metafunctions generate them. The cost is that every conversion is spelled
out, and an operation the metafunction does not generate, such as
`Length * Length -> Area` for `@quantity`, is a free function you write.

## Prior art

- **Haskell's `newtype`** declares a type with the representation of
  another and no implicit conversion between them, which is the model for
  `@newtype`.
- **Go's defined types**, `type UserId int64`, are distinct from their
  underlying type and need an explicit conversion. Go also allows a
  direct conversion between two types with the same underlying type,
  which this experiment rejects.
- **F#'s units of measure** (`float<m/s>`) check dimensions at compile
  time and erase them at run time, like `@unit`. F# infers derived units,
  where `@unit` names each one by its exponents.
- **Boost.Units and the mp-units library** do dimensional analysis with
  C++ templates. `cpp2::quantity` is a much smaller version of the same
  idea: one representation type, seven integer exponents, and no scaling
  between units such as metres and feet.

## Example

```cpp
UserId: @newtype<i64> type = { }
OrderId: @newtype<i64> type = { }
Cents: @quantity<i64> type = { }

Metres: @unit<double, 1> type = { }
Seconds: @unit<double, 0, 0, 1> type = { }
MetresPerSecond: @unit<double, 1, 0, -1> type = { }

cancel: (user: UserId, order: OrderId) = {
    std::cout << "user " << (user as i64) << " cancels order " << (order as i64) << "\n";
}

main: () = {
    cancel(42 as UserId, 1007 as OrderId);

    price := 1999 as Cents;
    total := price * 3 + (500 as Cents);
    std::cout << (total as i64) << " " << (total / price) << "\n";

    speed: MetresPerSecond = (100.0 as Metres) / (9.58 as Seconds);
    std::cout << (speed as double) << "\n";
}
```

The relevant lowered Cpp1:

```cpp
struct UserId { private: std::int64_t value;
public: using cpp2_newtype_rep = std::int64_t;
explicit UserId(cpp2::impl::in<std::int64_t> v);
UserId(UserId const& that);
[[nodiscard]] auto operator<=>(UserId const& that) const& -> auto = default;
constexpr explicit operator std::int64_t() const { return value; }
template<typename C> requires std::same_as<C, std::int64_t> friend constexpr auto as(UserId const& x) -> std::int64_t { return x.value; }
UserId(UserId&& that) noexcept;
auto operator=(UserId const& that) -> UserId&;
auto operator=(UserId&& that) noexcept -> UserId&;
};
// ...
[[nodiscard]] auto operator*(cpp2::impl::in<Cents::cpp2_newtype_rep> k, cpp2::impl::in<Cents> q) -> Cents;
// ...
using Metres = cpp2::quantity<double, cpp2::dims<1, 0, 0, 0, 0, 0, 0>>;
using MetresPerSecond = cpp2::quantity<double, cpp2::dims<1, 0, -1, 0, 0, 0, 0>>;
```

Output:

```
user 42 cancels order 1007
6497 3
10.4384
```

Swapping the arguments, `cancel(o, u)`, is rejected by the Cpp1 compiler,
and so is `(100.0 as Metres) + (9.58 as Seconds)`:

```
bad.cpp:73:22: error: invalid initialization of reference of type ‘cpp2::impl::in<UserId>’ {aka ‘const UserId&’} from expression of type ‘OrderId’
bad3.cpp:8:46: error: no match for ‘operator+’ (operand types are ‘cpp2::quantity<double, cpp2::dims<1, 0, 0, 0, 0, 0, 0> >’ and ‘cpp2::quantity<double, cpp2::dims<0, 0, 1, 0, 0, 0, 0> >’)
```

Converting one distinct type to another is rejected by the translator:

```cpp
o := 1007 as OrderId;
u := o as UserId;
```
```
bad2.cpp2:5:5: error: no conversion between distinct types OrderId and UserId - convert through the underlying type, as in '(o as i64) as UserId' [newtype-conversion]
```

## Rules and diagnostics

### Accepted form

```
newtype-declaration:
    identifier ':' '@newtype' '<' type-id '>' 'type' '=' '{' member-seq? '}'
    identifier ':' '@quantity' '<' type-id '>' 'type' '=' '{' member-seq? '}'
    identifier ':' '@unit' '<' type-id exponent-list? '>' 'type' '=' '{' '}'

exponent-list:
    ',' integer-literal                       // optionally '-'-prefixed
    exponent-list ',' integer-literal         // at most 7 in all
```

- `@newtype` and `@quantity` types may declare members of their own. The
  stored value is the private member `value`, so no member may be named
  `value`.
- `@newtype` and `@quantity` combine only with `@hashable`, `@delegate` and
  `@print`, and `@unit` only with `@print`. Any other metafunction would
  generate members of its own.
- The `k * q` operator of a `@quantity` type is a Cpp2 function at
  namespace scope, declared after the outermost type the quantity is nested
  in (`k: A::Q::cpp2_newtype_rep, q: A::Q`), where argument-dependent
  lookup finds it. A nested `@quantity` must be one namespace scope can name
  and deduce: no type on the way to it is private or protected, and none
  that encloses it is a template.
- A unit is its dimensions: two `@unit` types with the same underlying type
  and exponents would be one type, and are rejected.
- The underlying type may be any type-id, such as `@newtype<std::vector<i32>>`.
- `@unit` exponents are in the order length, mass, time, current,
  temperature, amount of substance, luminous intensity. Missing ones
  are zero. A `@unit` with no exponents is dimensionless and converts
  implicitly to `U`.
- `x as U` converts to the underlying type, through the hidden friend
  `as<U>`. `literal as T` constructs `T` when the literal converts to `U`
  without narrowing, using the runtime's `as<C, literal>()` for a type that
  declares `cpp2_newtype_rep`.
- Nothing converts implicitly between `T` and `U` or between two distinct
  types. Other operators are free functions written by the user.
- `newtype-without-underlying-type`: `@newtype`, `@quantity` or `@unit` has
  no type argument.
- `unit-with-members`: a `@unit` type declares members. Use `@quantity`
  for that.
- `unit-dimension`: an exponent is not an integer, or there are more than
  seven.
- `newtype-conversion`: `x as T` where `x` is declared with one distinct
  type, or is a deduced local initialized with `y as S` or `S(...)` of
  one, and `T` is another.
- `newtype-member-name`: a `@newtype` or `@quantity` type declares a
  member named `value`.
- `newtype-composition`: a metafunction other than those listed above is
  combined with `@newtype`, `@quantity` or `@unit`.
- `quantity-scope`: a `@quantity` type is a private or protected member, is
  nested in one, or is nested in a template type.
- `unit-duplicate`: two `@unit` types have the same underlying type and
  exponents.
- `empty-metaclass-argument`: the argument list has an empty entry, as in
  `@unit<double, , 1>`.

Full entry: [`EXPERIMENTS.md`](../EXPERIMENTS.md#current-experiments).

Tests:
[`newtype.cpp2`](../../tests/ports/experiments/newtype.cpp2),
[`newtype_nested.cpp2`](../../tests/ports/experiments/newtype_nested.cpp2),
[`newtype_error.cpp2`](../../tests/ports/experiments/newtype_error.cpp2),
[`experiment_newtype_test.cpp`](../../tests/unit/experiment_newtype_test.cpp).
