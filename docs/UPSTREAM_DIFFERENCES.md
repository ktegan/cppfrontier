# Differences from upstream cppfront

cppfrontier is an independent Cpp2-to-Cpp1 translator. This page lists every
known way it behaves differently from upstream cppfront (compared against
upstream commit `66eb614`).

The default language follows the Cpp2 documentation (upstream's `docs/cpp2`).
Where the documentation is silent, cppfrontier follows upstream unless its own
rule is materially simpler or more consistent, follows the documentation where
upstream does not, or keeps the generated Cpp1 standard C++
([design principles](../README.md#design-principles); the rules are in
[LANGUAGE.md](LANGUAGE.md)). Generated Cpp1 has upstream's
semantics, except where listed below, but it is not byte-for-byte identical to
upstream's output. [UPSTREAM_AUDIT.md](UPSTREAM_AUDIT.md) records the upstream
source, the evidence and the tests for each item.

Upstream issue numbers (`#1436`) refer to the cppfront issue tracker.

## Accepted where upstream rejects or miscompiles

### Templates and dependent names

- The `type` marker works wherever a type-id may appear, not only in a template
  argument: `sizeof(type T::x)`, `x as type T::x`, `:type T::x = v`,
  `(type T::x)()` and declared or parameter types all lower to
  `typename T::x`. It also works before a `decltype` scope,
  `std::tuple<type decltype(Ts())::value_type...>`, which upstream does not
  parse as a type.
- An argument of `sizeof`, `alignof` or `typeid` that begins with `*` or
  `const` is a Cpp2 type-id, `sizeof(*int)` or `alignof(const T)`; upstream
  reads the `*` as a prefix dereference and rejects it.
- In type positions, `typename` (and `template` before a later templated
  component) is added to a qualified name that begins with an enclosing
  template type parameter, with an alias of a dependent type (`A: type == T;`
  then `A::x`), or with a scope whose template arguments name one
  (`std::vector<T>::size_type`), and to a member of a `decltype(e)` scope.
  Upstream omits them. Template arguments still need the `type` marker.
- The value of a type alias marks dependent names as other type positions do:
  `a: <H> type == H::rebind<int>;` is
  `using a = typename H::template rebind<int>;`, where upstream emits
  `H::rebind<int>`.
- The type of an unnamed object (`:C::value_type = ()`) and the type in `as`,
  `is` and an `inspect` result are type positions, so dependent names get
  `typename`/`template`; upstream emits `C::value_type{}`, which does not
  compile. A members-only call `h..get<4>()` lowers to `h.template get<4>()`,
  where upstream emits `h.get<4>()`.
- A parameter type that mentions only a non-type template parameter, such as
  `_: std::integral_constant<int, N>` or `_: std::index_sequence<Is...>`, is
  dependent and lowers to `T const&`, so it is deduced. Upstream emits
  `cpp2::impl::in<...>`, a non-deduced context, so the call does not compile
  (#595).
- Pack indexing `xs...[0]` and `Ts...[0]` passes through as C++26 pack
  indexing and is not bounds-checked. Upstream does not parse `Ts...[0]` as a
  return type and wraps `xs...[0]` in `CPP2_ASSERT_IN_BOUNDS_LITERAL`, which
  does not compile.
- A constrained parameter `x: _ is C<>` requires `C<CPP2_TYPEOF(x)>`, and a
  pack `xs...: _ is C` requires `(C<CPP2_TYPEOF(xs)> && ...)`. Upstream emits
  `C<CPP2_TYPEOF(x), >` and an unexpanded pack.
- A pointer to a deduced type keeps its pointer: `p: *_ is C` is
  `C auto* const& p`, where upstream emits `auto const& p`.
- A converting constructor taking a pack of the enclosing class template,
  `operator=: (out this, args...: Ts)` in `record: <Ts...> type`, lowers to
  `record(auto const& ...args)` constrained by
  `(std::is_convertible_v<CPP2_TYPEOF(args), Ts> && ...)`. Upstream emits
  `record(Ts const& ...args)` and `operator=(Ts const& ...)`, which Cpp1
  rejects for a pack whose length is not one.
- The named-return struct of a function template carries the function's
  template head (`template<typename X> struct g_ret`, returned as `g_ret<X>`),
  where upstream asserts. Operators get valid names such as
  `operator_subscript_ret` where upstream would emit `operator[]_ret`.
- `<T = int>` means `<T: type = int>`; upstream accepts only the latter. A
  trailing comma in template arguments, `std::vector<int,>`, is dropped as in
  other lists; upstream rejects it.
- A type parameter's default is read as a type, so `*int` there is a pointer
  type; upstream reads it as a prefix dereference.
- Inside a template argument list, an `=` after `: type` belongs to the typed
  value, so `std::integral_constant<int, :int = 0>` parses (#790).
- A parameter with a deduced default, `y := 2`, gets a defaulted template
  parameter (`template<typename __cpp2_default_y = std::remove_cvref_t<decltype(2)>>`)
  so the argument can be omitted. Upstream emits `auto const& y = 2`, from
  which Cpp1 cannot deduce when the argument is omitted.

### Declaration order

- Concepts are declared before the type forward declarations, so
  `boxed: <T> type requires hashable<T>` compiles. Upstream forward-declares
  the type with its constraint before the concept (#1388).
- A namespace-scope function whose signature names a type, concept or function
  declared later is declared after it, so a function constrained by a later
  concept compiles. Upstream declares every function among the type
  definitions in source order. A `using ::xf` inside an earlier namespace is
  emitted after the declaration of `xf` (#1213).
- A namespace-scope constant or object named by an earlier signature or member
  initializer is declared ahead of that use. Upstream emits it in source
  order, so the use does not compile.
- A `==` (constexpr) function that names no Cpp2 type and no later function is
  defined at its source position, so a file-scope Cpp1 `static_assert` after
  it can call it (#1074).
- A type-scope object alias is a `static constexpr` member defined in the
  class, so later members can use it in constant expressions, such as an array
  bound (#1221). An alias whose type names its enclosing type also works in
  nested and templated types, where upstream reports an alpha limitation, and
  a deduced alias `w :== 7` is `static constexpr auto`, where upstream emits a
  `static const auto` declaration that does not compile.
- A file that begins with `module;` or a module declaration produces output
  that begins with `module;`, then the generated includes, the global module
  fragment, the module declaration and its imports (#1190, #1358).

### Statements and expressions

- An unbraced range `for` body, `for r do (e) statement`, is exactly one
  complete statement, including nested `if` and loop statements; upstream
  parses it as a flat expression (#1426).
- `return;` is accepted in a function declared `-> void`, as functions.md
  allows; upstream rejects it.
- `throw;` rethrows, as in Cpp1; upstream rejects it.
- `inspect` is accepted as a declaration initializer, an `=` function body
  (`f: (o) -> std::string = inspect o -> std::string { ... };`) and an
  alternative value, where upstream reports that only inspect expressions are
  supported.
- An `inspect` alternative whose value is a list, `is 1 = (a, b);` or
  `is _ = ();`, initializes the result type, `T{a, b}` or `T{}`; upstream emits
  the comma operator.
- An `inspect` alternative whose value holds a function expression or another
  `inspect` keeps only its convertibility check, since a requires-expression
  cannot hold a lambda that captures; upstream puts the value in
  `requires{...}`, which the Cpp1 compiler rejects.
- When an `inspect` result type is not a simple name (a pointer, reference,
  array, function or multi-word type), its fallback value is
  `std::type_identity_t<T>{}`; upstream emits `return char const*{};` for
  `-> *const char` (#724).
- A character literal with an octal escape of more than one digit, such as
  `'\101'`, is accepted; upstream reports it unterminated.
- In a string interpolation, a backslash that begins a character literal's
  escape is kept, so `('\\')$` works. The format specification follows the
  first top-level single `:`, where upstream takes the last single `:`
  anywhere, so `((:() = 42)():>4)$` and `((:(x) = x + 1)(2))$` work; upstream
  rejects the latter.
- An interpolation ends at the `)` that matches its `(`, so `(f()$)$`
  captures `f()`; upstream ends one at the first `)$` and rejects it.
- In a raw interpolated string `$R"(...)"`, `(expr:spec)$` adds `"{:spec}"`
  as in ordinary strings; upstream emits Cpp1 that does not parse.
- A capture of an unexpanded parameter pack, `xs$...` or `(xs + 1)$...`, in a
  function expression or a postcondition is a pack init-capture
  `..._0 = xs`; upstream emits `_0 = xs`, which does not compile.
- A qualified UFCS call is a direct call: `x.N::f(args)` is `N::f(x, args)`,
  or the member call `x.N::f(args)` when `N::f` is a function with a
  `this` parameter of a type declared here, and
  `123.u8()` is `cpp2::u8(123)`. Upstream emits `CPP2_UFCS(N::f)(x, args)`,
  whose member test GCC 15 rejects, so `vec.std::ranges::sort()` does not
  compile with GCC 15.
- A call `y.f()` in the initializer of an object named `f` is a member call
  whatever the object's declared type (#863, #864); upstream does this only for
  deduced types. When `f` in `y.f<T>()` finds an object, the call is
  `y.template f<T>()`, where upstream's `CPP2_UFCS_TEMPLATE(f<T>)` does not
  compile (#889).
- A constructor or destructor with an expression body emits the expression as
  a statement, not a `return` (#822).
- A typed `in` statement parameter whose initializer is a parenthesized list
  constructs a `T const` object, so `(_: std::unique_lock = (m))` deduces its
  template arguments (#1003).
- `uint` is a Cpp2 fundamental type alias like `ushort` and `ulong`. Upstream
  emits it unchanged, which compiles only where the C library declares a
  global `uint`.
- An `operator<=>` with an expression body and no `->` deduces its return
  type; upstream rejects it.
- `x is C` with a concept `C` declared in the same file is
  `C<CPP2_TYPEOF(x)>` (with any written arguments after it), also in an
  `inspect` alternative; upstream emits `cpp2::impl::is<C>(x)`, which does not
  compile for a concept.
- A subject of `is` or `inspect` that is visibly a type, `T is X` for a
  template type parameter, fundamental type or type declared here, is a
  compile-time query on the type, and a value after it, `T is 0`, is
  `type-subject-value`; upstream emits `cpp2::impl::is<X>(T)` and
  `cpp2::impl::is(T, 0)`, which do not compile.
- After `is`, `true`, `false`, `nullptr` and an operand beginning with `!`
  are values, so `b is true`, `p is nullptr` and `is !0 = v;` work; upstream
  reads the keywords as type-ids and rejects `!` in an inspect alternative.
- The postfix `--` generated from a prefix `operator--` applies `--`; upstream's
  generated free postfix `--` applies `++`.

## Rejected where upstream accepts

These programs are rejected during translation. In most cases upstream emits
Cpp1 that the Cpp1 compiler rejects, and the item says so.

- One object passed as `out` twice in one expression, `f(out x, out x)`, is
  `out-argument-twice` (alternatives of one `inspect` may each pass it).
  Upstream accepts it; for a deferred local the second initialization fails
  at run time.
- A local without an initializer whose type is visibly a reference,
  `r: cpp1_ref<int>;` or an alias of one, is `uninitialized-reference`: a
  reference is bound where it is declared. Upstream emits
  `deferred_init<int&>`, which does not compile.
- An empty string interpolation, `"()$"` or `"( )$"`, is
  `empty-interpolation`; upstream emits `cpp2::to_string()`, which does not
  compile.
- An interpolation in a prefixed string literal, `u8"(x)$"` or
  `$u8R"((x)$)"`, is an error; upstream emits `u8"" + cpp2::to_string(x)`,
  which does not compile, and rejects the raw form as an ill-formed
  initializer.
- In a constructor or assignment, a `Base = value;` statement after the
  leading statements that set the bases and data members in declaration
  order (types.md "memberwise operator=") is `base-initialization-order`.
  Upstream emits it as the statement `Base = value;`, which does not compile.
- In a constructor's preconditions and leading `member = value;`
  statements, a read of a data member not yet initialized there, as in
  `x = y; y = 2;` or `this.y`, is `use-before-initialization`, as for a
  local. Members, those with a default initializer included, are
  initialized in declaration order, after the preconditions. Upstream emits
  the read of the uninitialized member.
- A capture of `that` whole, `that$`, in a copy constructor
  (`operator=: (out this, that)`) is `copy-captures-that`: copying `that`
  calls the constructor again. Upstream's Cpp1 recurses until the stack
  overflows. Its members can be captured, `that.n$`.
- In an expression, a joined `>=` or `>>=` is an operator and never closes a
  template argument list, so `std::is_same_v<int, int>>=0` is rejected; write
  `std::is_same_v<int, int> >= 0`. Upstream splits it when the `<` touches the
  preceding name, but whitespace is not significant in Cpp2 (functions.md),
  and without that rule `f(a < b, c >= d)` would read `a<b, c>` as a template
  argument list. After a type, as in `v: std::vector<int>= ();`, the `=` is
  never a comparison, so there `>=` closes the list as upstream.
- A template parameter declared twice, and a pack before the last parameter of
  a template that is not a function, are diagnosed; upstream emits both.
- A virtual function that deduces its return type (`f: (virtual this) = 1;`,
  `-> _`) is rejected, since Cpp1 does not allow it; upstream emits
  `virtual auto`, which GCC accepts and Clang rejects.
- A virtual function (`virtual`, `override` or `final this`, or an
  `@interface` function) with named returns is `virtual-named-returns`: each
  class would declare its own result type, which Cpp1 rejects as an invalid
  covariant return type. Upstream emits that Cpp1.
- Contracts on a function without a body (a pure virtual or `@interface`
  function, a defaulted `operator==`) are `contract-without-body`, since
  they are checked in the body; upstream drops them silently.
- A constraint `_ is C<...>` naming a declaration in the same file is checked
  for what needs no C++ semantics: `C` is a concept, its first parameter takes
  a type, and it accepts the written arguments plus the constrained type.
  A constraint that is not a name (`_ is int`, `_ is *T`, `_ is decltype(e)`,
  `T: type | C`) is not a concept (`constraint-not-concept`).
  Upstream leaves all of this to the Cpp1 compiler. Constraint satisfaction is
  left to the Cpp1 compiler in both.
- A `main` that returns `void` (no return type, or `-> void`) is emitted as
  `int` and takes no `return` (`void-main-return`); upstream accepts `return;`
  there.
- A function type's parameter that is `*(int)`, `*(x)`, `this`, has a default
  or is a pack is diagnosed, where upstream emits invalid Cpp1.
- Every prefix `*` is rejected, including `2 * *p`, since common.md lists only
  `!`, `+` and `-` as prefix operators; `reinterpret_cast` is rejected with the
  other Cpp1 casts, and `unchecked_cast` covers it. Upstream accepts both.
- An assignment is never a value: `x: int = a = 3`, `g(a = 3)`, `return a = 3`,
  `(c += b) += b` and a fold operand `(x = f(), ...)` are `chained-assignment`,
  and `if a = 3` is `assignment-condition`. Upstream rejects only a chain
  `c = b = a`.
- A comparison operator must return `bool`, and `operator<=>` one of the three
  `std::` ordering categories spelled exactly or a deduced type
  (`comparison-return-type`). Upstream checks by substring, so it accepts any
  type whose spelling contains `strong_ordering`.
- The runtime support names (`unchecked_*`, `finally`, `cpp1_ref`,
  `cpp1_rvalue_ref`) are reserved: declaring one is `reserved-support-name`.
  Upstream lets a user declaration be replaced by the runtime's.
- A `<`, `<=`, `>` or `>=` whose operands are both unqualified names declared
  with a fundamental integer type, or `u`-suffixed literals, of different
  signedness is reported as `mixed-sign-comparison` during translation.
  Upstream also rejects it, but only through the `static_assert` in
  `cpp2::impl::cmp_*` when the Cpp1 is compiled.
- A returned expression that refers into an object the function owns, a
  local or a `copy` or `move` parameter (its leading name, through member
  accesses, subscripts, calls, postfix operators and parentheses, up to a
  postfix `*`), is rejected in a `-> forward T` or `-> forward_ref _`
  function, which always returns a reference; so is an `in` parameter there,
  which may be passed by value. A `-> forward _` function or an expression
  body deduces by value or by reference from the returned expression's
  `decltype` (functions.md), so there the generated Cpp1 holds
  `static_assert(!std::is_reference_v<decltype(expr)>)` for a local or a
  `copy` or `move` parameter: `return i;` for a local `int i` returns a value,
  and so does `(copy x: int) = x`, since a returned parameter name is not
  moved there (Cpp1 moves it), while `(move s: std::string) = s`, a `std::string&&`, is
  rejected when the Cpp1 is compiled. An `in` parameter is the caller's
  object, or a copy whose `decltype` returns a value, so `(x) = x` and
  `(s: std::string) = s` are accepted. Upstream rejects every such return of
  a named function during translation, `in` parameters and the by-value
  `return i;` of `pure2-forward-return-diagnostics-error.cpp2` included, but
  not of a function expression, so it accepts `:(x) = x` and rejects
  `f: (x) = x`. It checks the leading name of any postfix chain, so it
  accepts `return (s).x;` and rejects `return it*.second;`.
- An object named `_` with a deduced type (`_ := f();`, `_: _ = f();`,
  `_: const _ = f();`) is `anonymous-object` in every scope, with upstream's
  message and at the name. Upstream gives that message for a local; it
  accepts a statement parameter `(_ := f())`, and at namespace and type scope
  reports only that the type is deduced. `_: T = f();` declares an unnamed
  object.
- A pointer initialized or assigned from `nullptr`, an integer or `()` in
  grouping parentheses, `p = (nullptr);`, is rejected as without them;
  upstream accepts it.
- A data member named `_` is copied, moved and hashed by generated members
  under its unique name; upstream's generated members name it `_`, which
  does not compile.
- An `@enum` enumerator `_;` is `enum-anonymous-enumerator`; upstream
  accepts it as an enumerator named `_`.
- A `@union` alternative, a typed temporary `:T = (x)` and a `-> forward`
  return may have any Cpp2 type, such as `*const char` or a function
  pointer. Upstream's `@union` cannot parse its `sizeof(*const char)`, its
  `char const*{x}` does not compile, and it rejects `-> forward` before a
  function type.
- A type alias's value takes the checks of a declared type: `A: type ==
  int&;` and `y: decltype(x)& = x;` are `cpp1-reference-type`. Upstream
  rejects both as parse errors.
- `decltype(x)` of a deferred local or an `out` parameter is its declared
  type; upstream's is `T&` from `x.value()`, so `y: decltype(x) = 5;` does
  not compile.
- A data member may hold a nested type defined after it (`a: inner = ();`
  before `inner: type`); upstream emits nested types in source order.
- An object whose initializer names itself, `g: int = g + 1;`, is
  `own-initializer` in every scope; upstream rejects it only for a local.
- A declaration name that is reserved in Cpp1 and outside the `cpp2_<name>`
  escape set, such as `namespace`, is rejected; upstream emits it as is. Names
  beginning with `__cpp2_` are reserved for the translator (`reserved-name`).
- `_` in a template argument, `h: std::vector<_> = ();`, is `nested-wildcard`;
  upstream emits `std::vector<_>`.
- `static` on an alias is diagnosed; upstream drops it silently (#1093).
- A `:=` initializer that assigns to a built-in type name, `my_int := i32 = 300`,
  is diagnosed with the `name: type = value` spelling; upstream emits invalid
  Cpp1 (#436).
- `[]`, `+`, `-`, `++` and `--` on a `*T` data member, named in a member
  function with or without `this.`, are pointer arithmetic, as on a `*T` local
  or parameter. Upstream checks only locals and parameters.
- `*` or `&` after an operand and before `!` or the `:` of a typed value or
  function expression is binary, so `a * !b`, `a * :int = 4` and
  `a * :() = 5;()` multiply; upstream reads a postfix `*` there and rejects
  what follows.
- A postfix `*` or `&` with whitespace before it and a prefix-looking
  operator after it, `a * -1`, `a * *p` or `x & -x`, is
  `misleading-postfix-spacing`. Upstream reads the same postfix operator
  (`(a*) - 1`) without a diagnostic.
- A `*` or `&` before `...` is postfix, so `(xs * ...)` and
  `(1 * ... * xs)` are `misleading-postfix-spacing` and a fold over `*` is
  written `(... * xs)`. Upstream also reads the `*` as postfix but drops the
  parentheses, emitting `(*cpp2::impl::assert_not_null(xs))...`, which does
  not compile.
- Arithmetic on an address, `x& + 1`, `x&[i]` or `(x&) - 1`, is
  `pointer-arithmetic`; upstream checks only pointer objects.
- A fundamental type name as an operand, as `double` in `v is int | double`,
  is `type-after-is` or `type-as-operand`; upstream emits Cpp1 that does not
  compile.
- An `inspect` result type that is deduced (`-> _`, `-> *const _`) or `void`
  is diagnosed, since each value converts to it; upstream emits Cpp1 that does
  not compile.
- An `inspect` alternative after an unguarded `is _` can never be chosen and
  is `unreachable-alternative`; upstream emits Cpp1 that does not compile.
- `decltype(x)` of a deferred local or `out` parameter in the signature or
  `requires` clause of a function expression, or of an `out` parameter in its
  own function's signature, is the declared type of `x`; upstream rejects the
  mention as a use before initialization.
- `not` at the end of a line is the operator `!`, as `not` followed by any
  other whitespace is. Upstream lexes it as a name there.
- A typed value with no initializer after `(` or `,`, as in
  `alias2: == (:int);`, is diagnosed; upstream crashes (#355).
- A conversion function `operator T: ...` is `conversion-function`, pointing to
  a named function or `operator=: (implicit out this, ...)`. Upstream passes a
  namespace-scope one through as Cpp1.
- `operator new`, `operator new[]`, `operator delete`, `operator co_await` and
  a literal operator `operator "" _x` are `unsupported-operator`; they belong
  in Cpp1 code. Upstream gives generic errors, or passes a namespace-scope
  literal operator through as Cpp1.
- `throw()`, `throw(a, b)` and `throw((a, b))` are `invalid-throw`. Upstream
  emits `throw()`, the Cpp1 comma expression `throw(a, b)`, and
  `throw({a, b})`.
- A Cpp1 declarator in a template argument (`T const`, `T&&`, `int*`,
  `const char*`) is `cpp1-declarator-in-template-argument`, with the Cpp2
  spelling, and one ending in a postfix `*`, `T*`, is
  `postfix-star-in-template-argument`, suggesting `*T` or `(T*)`. Upstream
  lowers `std::vector<int*>` to `*cpp2::impl::assert_not_null(int)`.
- A list `(a, b)` used as an operand, argument, condition or index, including
  a Cpp1 comma fold `(e, ...)`, is `comma-operator`; upstream emits the Cpp1
  comma operator. See also nested lists below.
- A loop's `next` clause is one expression, so `next (a += 1, b++)` is
  `next-clause-list`; upstream emits the Cpp1 comma operator. Experiment
  `next-list` runs each item in order.

## Different semantics

### Last use and move

cppfrontier follows upstream's backward scan for definite last uses, with
these differences.

- A definite last use must be the only use of the name in its statement
  (functions.md: "a single use of that name"), so `f(x, x)`,
  `x.substr(x.size())` and `v[v[0]]` move nothing. Upstream moves the
  textually last mention unless a binary operator separates them.
- A mention in a compile-time context (`sizeof`, `sizeof...`, `alignof`,
  `decltype`, `noexcept`, a requires-expression or a template argument list) is
  not a use and is never moved; upstream moves inside it, as in
  `h<cpp2::move(x)>()` and `sizeof(cpp2::move(x))`.
- A parameter mentioned in a postcondition is moved only by a `return`
  statement, which runs the postconditions first; upstream moves it in the
  body, so the postcondition sees a moved-from value.
- A mention in an `assert` statement is a use but is never moved, so whether an
  earlier statement moves does not depend on contract checking; upstream moves
  inside the assertion.
- A `static` local outlives the call, so no mention of it is a definite last
  use; upstream moves it at its last mention, and the next call sees it
  moved from.
- A local, or a `copy` or `move` parameter, whose declared type is not
  visibly an object type (a fundamental type, a pointer, or a type of this
  translation unit) may be a Cpp1 reference, as `std::vector<T>::reference`
  is, so it is forwarded at its definite last use, `CPP2_FORWARD(x)`. That
  moves an object, as `cpp2::move(x)` does, and leaves a reference alone;
  upstream moves it, which empties the object a reference refers to. Locals
  of types such as `std::string` are therefore spelled `CPP2_FORWARD(s)` in
  the Cpp1.
- The mentions of a name in the values of different alternatives of one
  inspect are each a definite last use, since only one is evaluated;
  upstream moves neither, so `is 1 = take(s); is _ = s;` does not compile
  when `take` takes a `move` parameter.
- An `inout` statement parameter's initializer is never moved; upstream emits
  the ill-formed `auto& t{cpp2::move(x)}`.
- The last mention before a `return`, `throw`, `break` or `continue` that
  leaves the name's scope is a definite last use, and so is a `catch`
  handler's last mention; upstream moves neither. A mention in a `catch`
  handler blocks moves in its `try` block, where upstream moves anyway.
- A returned name, `return x;` or an expression body that is just `x`, of an
  initialized local or a `copy` or `move` parameter is left to Cpp1's implicit
  move: `return x;` where upstream writes `return cpp2::move(x);`. Where the
  return type is `decltype(auto)`, `cpp2::move(x)` would return a reference
  to `x`.
- A mention inside a fold, or inside the operand of a pack expansion `x...`,
  may be evaluated more than once, so it is never moved, like a mention in a
  loop body (#1279). Another argument beside the expansion, `x` in
  `f(x, ys...)`, may still be moved.
- A capture `x$` is not moved when its statement mentions `x` elsewhere, and
  no other mention is moved beside a capture of `x`, since their evaluation
  order is unspecified. Upstream moves the capture in
  `both(x, :() = x$;)`.
- A `forward` parameter whose name starts with `guard` is never forwarded,
  as no `guard` name is moved; upstream forwards it at its last use.
- Constructor member initializers lowered from leading `m = value;`
  statements, and generated `@struct`-style constructors, are not rewritten
  for last use; upstream forwards `a_` there.
- In a function with a `move that` parameter, `that` is an ordinary `move`
  parameter: only its definite last use moves (`cpp2::move(that).m`).
  Upstream moves at every mention of `that`, so `that.id = 0;` assigns to an
  rvalue and a member read twice is moved twice. Generated memberwise defaults
  move, as a Cpp1 defaulted move does, unless the function also reads that
  part of `that`.

### Initialization

- A constructor checks its preconditions before its member initializers
  evaluate anything, and an assignment before it assigns the members, so
  `pre(!n.empty())` reads the argument that `name = n;` then moves. Upstream
  checks them in the body, after the member initializers.
- A branch that leaves by `return`, `throw`, `break` or `continue` does not
  join the merge after its `if`: `if c { x = 1; return; } x = 2;` is
  accepted, and `if c { return; } x = 2;` is rejected for an `out` parameter
  or named return value. Upstream ignores `return` here, so it accepts the
  latter with the value uninitialized. A `[[noreturn]]` call still joins.
- A path that ends in `throw` needs nothing initialized, so
  `f: (out x: int) = { throw(1); }` is accepted; upstream rejects it.
- An `out` argument initializes only where it is evaluated on every path. One
  after `&&` or `||` or in one inspect alternative does not, and a later use is reported as a use before initialization with a note;
  upstream counts it as initializing.
- Within one expression, an `out` argument initializes only the reads that
  C++17 sequences after it: the later operands of `<<`, `>>`, `&&` and `||`,
  the later elements of a list, an assignment's left operand, and the
  alternatives of an `inspect` after its subject, so
  `v[i] = init(out i);` is accepted, and `_ = init(out a) + a;` and
  `g(init(out a), a)`, whose operands are unsequenced, are rejected.
  Upstream follows the text, accepting a read after the `out` argument and
  rejecting one before it. The declaration order of locals initialized by
  one expression is checked in the evaluation order too, so `a = f(out b);`
  with `a` declared first is rejected and with `b` declared first is
  accepted; upstream does the reverse.
- A local that the last statement of a `try` block initializes may be
  initialized in a `catch` handler, `try { n = f(); } catch (_) { n = -1; }`;
  upstream emits a handler assignment that does not compile.
- A local named like an uninitialized local of the same function is accepted
  and does not initialize the outer one, since lookup tells the two apart;
  upstream rejects it.

### Other rules

- The value after `is` is one prefix expression, as operands of the binary
  operators are, so `x is 1 + 1` is `(x is 1) + 1` and
  `x is 1 && y is 2` is two tests. Upstream takes the rest of the
  expression, `x is (1 + 1)` and `x is (1 && (y is 2))`.
- A `move` parameter lowered to a forwarding reference (`x: _`, a typed pack
  or `x: T` with `T` the function's type parameter) is constrained to
  rvalues. Upstream accepts an lvalue there, and the parameter's last use
  moves from the caller's object.
- A constrained `in`, `copy` or `inout` parameter `x: _ is C` lowers to
  `C auto const&`, `C auto` or `C auto&`, so overloads on
  `_ is std::integral` and `_ is std::signed_integral` are ordered by
  subsumption. Upstream emits `auto const& x` with a `requires` clause, and
  GCC reports the call as ambiguous. `move` and `forward` parameters keep the
  `requires` clause.
- In a range `for` with a `next` clause, `break` does not run `next` (#1436),
  and a labeled `continue`, in the loop's body or a nested loop, runs `next`,
  which upstream skips.
- `(x)` in expression position is grouping everywhere, including a `for`
  range, so `for (v) do` iterates over `v`; a list needs two or more elements.
  Upstream lowers any parenthesized range to a braced list, so `for (v) do`
  runs once over `{ v }`.
- Cpp2 has no comma operator, so a parenthesized list inside a list is a
  nested initializer: `m: std::map<std::string, int> = (("CPU", 10), ("GPU", 15))`
  lowers to `{{"CPU", 10}, {"GPU", 15}}` (#1037). Upstream emits the comma
  operator.
- The converting assignment generated from an `out this` constructor assigns
  a member that the constructor initializes as `m = (x)` the braced
  `m = { x }`, as any assignment of a parenthesized value, so it converts as
  the constructor's `m{x}` does. Upstream writes `m = x`. Clang warns
  `-Wbraced-scalar-init` for a scalar member. A base set as `B = (x)` is
  assigned the same way, `B::operator=({x})`, so `B = (that)` binds `that`
  without constructing a `B`, which an abstract base cannot be; upstream
  writes `B::operator=(x)`.
- A type of qualifiers alone omits the `_` wildcard, so `p: * = x&;` is
  `p: *_ = x&;` and `(copy x: const)` is `auto const x`, as upstream's
  objects. In a parameter upstream drops the qualifiers: `(copy x: const)` is
  `auto x` and `(y: *)` is `auto const& y`.

## Metafunctions

- The default constructor `@value` generates is not `explicit`, so an array of
  values can be list-initialized; upstream's is `explicit`.
- In a type that applies a metafunction, a member that shares a name a
  metafunction generates (`to_string`, `hash`, `is_A`, `as_A`, the type's own
  name, ...) is `metafunction-reserved-name`; only overloads of generated
  functions, operators and constructors may share one. Upstream reserves only
  `@flag_enum`'s names and lets the others reach Cpp1.
- `@struct`, `@basic_value` and the `@value` family reject an `override this`
  or `final this` function, which is virtual (metafunctions.md: "not
  polymorphic"); upstream rejects only `virtual this`.
- A member name that starts with `_` is reserved only when a metafunction
  generates it (`_value`, `_storage`, `_discriminator`); upstream rejects every
  such member name in a type that applies a metafunction.
- An `@enum` value may be written in decimal, hexadecimal, binary or octal,
  with digit separators; upstream accepts only plain decimal.
- Each `@enum` value, written or implied, must fit the underlying type (or
  `i64` without one, `u64` for `@flag_enum`), so `@enum<u64>` holds the whole
  `u64` range. Upstream truncates a value outside the underlying type and
  leaves a negative value to the Cpp1 compiler.
- `@union` move construction and assignment move the active alternative;
  upstream copies it.
- Two `@union` alternatives with the same name are
  `union-duplicate-alternative`, at the second. Upstream generates both, and
  the Cpp1 compiler reports the overloads of their accessors.
- Two `@enum` or `@flag_enum` enumerators with the same name are
  `enum-duplicate-enumerator`, at the second. Upstream generates both, and
  the Cpp1 compiler reports the redeclaration in generated code.
- A `@struct` with a base gets a memberwise constructor that sets the base from
  its initializer or by default, so a base with an explicit default
  constructor works; upstream emits Cpp1 that does not compile for one.
- `@regex` generates its matcher as Cpp1 with inline member definitions rather
  than as Cpp2 source, escapes quotes and control characters in string and
  character literals, and invalidates a capture group on a later failed match
  with a flag rather than upstream's `cpp2::regex::make_on_return` guard.
- `@regex` diagnoses patterns that upstream accepts and miscompiles: a group
  without its closing bracket, an initializer that is not a single string
  literal, a backslash before an unknown word character (`\Q`, `\R`, `\cK`), a
  reversed range, a character code above 255, a non-digit inside `\x{...}`
  or `\o{...}`, and, as in Perl, `\K` in a lookahead or lookbehind and a
  lookbehind that can match more than 255 characters (`(?<=a+)`, or one
  holding a backreference). An empty alternative, as in `|a` or `(|a)`, matches
  the empty string as in Perl, where upstream rejects one at the start of a
  group. A backslash before any non-word character quotes it, escapes
  inside a bracketed class are read as Perl reads them, `^` negates only first
  in a class, a `-` next to a class such as `\d` is literal, and `\x` without
  braces takes at most two hexadecimal digits. As in Perl, a `{` that does
  not start a quantifier `{n}`, `{n,}`, `{,m}` or `{n,m}` after an element is
  a literal character (`a{2B`, `x|{2}`), except right after a letter escape
  such as `\d`, where it is an error, and a bound with a leading zero, a
  `*`, `+` or `?` after `|` or `(?i)`, and a quantifier after a quantifier
  (`a**`, `a{2}{3}`; a lazy or possessive suffix such as `a*?` is not one) are
  errors. Only a pattern's first parse
  error is reported.
- `@regex` objects' `search(str, start)`, `match(str, start)` and
  `find_all(f, str, start)`, also with a length, search `str` from `start` as
  Perl does from pos(): `\G` matches at `start`, `^`, `\A`, `\b` and
  lookbehind see the text before it, and group positions count from the start
  of `str`; `find_all` starts each search, and `\G`, where the previous match
  ended. Upstream treats `start` as the start of the string.
- `@regex` matches as Perl's documented rules (perlre) say. Where Perl's
  result depends on its optimizer, it deliberately differs: a group in a loop
  keeps the match of the last repetition that set it, as in PCRE and Python,
  where Perl sometimes leaves it unset (`(?:(a)|b)+` on `ab` leaves Perl's `$1`
  undefined, `(?:x(a)|b)+` on `xab` keeps it). Perl results that perlre does
  not state are not followed either: `(?|(a)|(b)(c))\2` matches `bcc` (Perl
  fails), `(?=(c+)?)[A-C]` matches `A` (Perl fails), and a group inside a
  negative lookahead is unset after it, since the lookahead succeeds only when
  its pattern fails (Perl keeps the group from the failed attempt; PCRE and
  Python leave it unset).
- `@autodiff` reverse mode handles `if` (the primal pass records the branch
  taken and the backprop pass replays it) and diagnoses `for` loops, member
  access such as `t.a`, `break`, `continue`, prefix operators and a `return`
  before the end of the function, and rejects `order=` greater than 1, whose
  derivatives upstream generates with mismatched Taylor orders. Upstream's
  other limitations and messages are kept.
- `@autodiff` generates correct code where upstream's is invalid or different:
  a literal has no backprop statement, a copy `r = x` propagates `x_b += r_b`,
  an assignment that overwrites a value saves the old value for the backprop
  pass, and in forward mode a `return` before the end of the function returns.
  A function with an unnamed return cannot declare `r`, the name of its
  differentiated return value.
- `@autodiff`'s `rws_suffix=` sets the reverse-mode suffix; upstream sets the
  forward one. Diagnostics inside a derivative point at the function it was
  generated from.
- `@print` writes upstream's form, but members generated by metafunctions such
  as a nested `@enum`'s are not printed, and a declaration the printer does not
  understand is printed as written.
- `@sample_traverser` does not print upstream's trace of the reflected
  statement and expression grammar; its effect on generated code is the same.

## Generated code and runtime

- In declarations, the Cpp2 fixed-width types lower to Cpp1 types: the
  fundamental type for `i8` to `i32` and `u8` to `u32` (`signed char` for
  `i8`), and `std::int64_t`/`std::uint64_t` for `i64`/`u64`. The same holds in
  `as` targets, typed temporaries and `requires` clauses. Upstream writes
  `cpp2::i32` and so on. In other expressions both write the runtime alias,
  as in `cpp2::u8(123)`.
- Statements that share a source line are emitted on separate lines, each
  with a `#line` directive to that line where the output would map elsewhere
  without it, so Cpp1 compiler errors and run-time check messages name the
  right line; upstream keeps the statements on one line. A loop's `next`
  clause, emitted after the body, gets a `#line` back to its own line, where
  upstream's maps to the loop's closing line. Each of a function's
  `pre` and `post` checks names the line of its condition, where upstream's
  name the line of the first statement, or a later one when there are named
  return values or the body is on the header's line. Statements a metafunction generates keep no
  line of their own.
- UFCS details: `x.f()` inside `decltype(...)` in a type stays a member call; a
  non-type template argument in a type uses the `_NONLOCAL` UFCS macros even
  inside a function body; calls to members generated by metafunctions, such as
  `@union` accessors, are emitted as member calls.
- Unused-warning attributes: an object named `_` is `auto_<line>_<column>`
  in every scope (upstream numbers function locals `auto_1`, `auto_2`, ...),
  and a local one is `[[maybe_unused]]`. A `that` parameter is always
  `[[maybe_unused]]`; upstream marks it only in a type with no data members. The `inspect` subject is
  `[[maybe_unused]]`. So the Cpp1 compiles without `-Wall -Wextra` warnings
  that upstream's has (`tools/strict-compile.sh`).
- The first assignment of a list to a deferred local or an uninitialized `out`
  parameter constructs through `construct_from`, from the prvalue the list
  produces, so a type with no copy or move constructor can be initialized from
  a list without narrowing.
- Generated code includes the headers in `runtime/`, which are upstream's
  `include/` headers with three patches to `cpp2util.h`: a bounds-checked
  subscript builds its message only on failure, so it works in a constant
  expression; `unchecked_cast` falls back to `reinterpret_cast` when
  `static_cast` does not apply; and `deferred_init` and `out` have
  `construct_from`. See [runtime/README.md](../runtime/README.md).
- The runtime also carries additions and headers used only by experiments,
  listed in [runtime/README.md](../runtime/README.md). They do not change
  programs that enable no experiment.

## Command line and diagnostics

- Status lines, `file(line,col): error: message` and `-format-colon-errors`
  follow upstream, but the file is printed as given on the command line, where
  upstream strips its directory.
- Messages do not end with upstream's `(at 'token')`.
- Every error is reported, where upstream reports some only once and stops at
  the first lex or parse error: the first syntax error of each statement, and
  no metafunction or flow-analysis diagnostic on a declaration that has one,
  since those see what recovery left. Upstream reports an unmatched `#else`
  one line late.
- Upstream's summary lines, such as `null initialization detected` and
  `==> program violates lifetime safety guarantee`, are not printed.
- An input whose output path an earlier input already writes
  (`d1/x.cpp2 d2/x.cpp2`) is an error; upstream overwrites the file.
- `-pure-cpp2` rejects a `.h2` input, which upstream splits into `.h` and
  `.hpp` files.
- A deferred local that is never initialized reports
  `local variable x is never initialized`, and one initialized on only some
  paths reports `must be initialized on every path`; upstream reports a use
  before initialization (#1288).
- The warning `member-copied-not-moved` names a read `p.m` of a `move`
  parameter `p` that copies because it is not `p`'s last use. Upstream has no
  such warning.

## Mixed Cpp1/Cpp2 files

- A `using` line begins Cpp2 only at file scope outside Cpp1 braces, in the
  documented forms `using a::b;` and `using a::_;` on one line. Other forms
  (`using a = b;`, `using namespace n;`, `using enum E;`, `using a::b, c::d;`)
  are Cpp1. Upstream treats every `using` line as Cpp1.
- Only the first branch of an `#if`/`#elif`/`#else` counts Cpp1 braces, as
  if it were taken. Upstream counts every branch, and counts once when every
  branch, `#else` included, opens the same number.
- A leading UTF-8 byte order mark is skipped; upstream reads the file as Cpp1.
- A backslash at the end of a directive, a Cpp1 line or a comment outside Cpp2
  splices the next line onto it, so that line keeps its category. Upstream
  splices only directives, so a declaration after a splice inside a Cpp1
  string becomes Cpp2.

## Experiments

cppfrontier has opt-in language experiments, all off by default, so the
default language above does not depend on them. Enable one with
`-experiment=name` (repeated for each), all with `-experiment=all`, and list
them with `-experiment=list`; unlike upstream's flags, this flag is matched in
full rather than by unique prefix. For example, `nonnegative-literal-comparisons`
lets an unsuffixed decimal literal compare with either signedness, so `u < 5`
with an unsigned `u` compiles; by default it is rejected, as upstream rejects
it. [EXPERIMENTS.md](EXPERIMENTS.md) describes each experiment.
