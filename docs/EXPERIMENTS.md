# Language Experiments

An experiment is an opt-in change to the Cpp2 language, such as a candidate
answer to a question in `OPEN_QUESTIONS.md`. Every experiment is off by
default, so the default language never changes.

## Using one

- Command line: `-experiment=name`, repeated for each experiment;
  `-experiment=all` enables every one, and `-experiment=list` prints them
  with their descriptions. An unknown name is an error. The flag is matched
  in full, unlike upstream's flags, so it does not shorten their unique
  prefixes.
- Language server: the `initialize` request's
  `initializationOptions: {"experiments": ["name", ...]}` applies to every
  document of the session; `"all"` enables every one. An unknown name fails
  `initialize` with `Invalid params`.
- An experiment that builds on another, such as `@given` on `@trait`, enables
  it: `-experiment=given` also enables `trait`. Experiments are independent
  otherwise, and new metafunctions are experiments like any other, so without
  the flag `@name` stays `unsupported-metaclass`.

There is no in-source switch. Cpp2 has no pragmas, and a file's meaning should
not depend on a comment; the build or editor configuration that chooses the
translator flags chooses the experiments too.

## Adding one

1. Add an entry to `all_experiments` in
   `include/cppfrontier/experiments.hpp`: its name, a one-line description,
   the `LANGUAGE.md` topic or `OPEN_QUESTIONS.md` entry it explores (or `none`),
   and the experiments it builds on, which must come before it. This is
   the only declaration; the command line, the language server and
   `-experiment=list` read the table.
2. In the stage that changes, ask the translation options:
   `options.experiments.enabled("name")`. The argument is checked at compile
   time, so a misspelled or removed name does not build. A stage that does
   not receive `translation_options` gets the `experiment_set` passed along
   (as `analyze_safety` and `cpp1_expression_options` do). A metafunction
   names its experiment in the `metafunctions` table of `src/reflection.cpp`.
3. Keep the default path unchanged and the change in as few stages as
   possible.
4. Test both behaviors: a `[experiments]` case in
   `tests/unit/experiments_test.cpp`, and a port in `tests/ports/experiments/`
   with a `// flags: -experiment=name` line, which `tools/run-port.sh` passes
   to the translator, and where the default differs, `<port>_default.cpp2`,
   the program without the flag, changed only where the default requires.
5. Write `docs/experiments/name.md`, linked from its entry below: what it
   enables, why that is useful, prior art in other languages or proposals,
   an example with its output, a grammar rule for any lexing or parsing
   change (in the `//G` notation of upstream's `parse.h`), and its rules and
   diagnostics.

## Removing one

To drop it, delete its table entry; the build then names every
`enabled("name")` query, and those branches go. To adopt it as the default
language, delete the entry and the queries, keeping the experimental branch,
and state the rule in `LANGUAGE.md`. Either way, move or delete its
tests. With no experiments left the table is `std::array<experiment, 0>{}`.

## Current experiments

Each name links to a page that explains the experiment with an example; the
entry here is the reference for its exact rules and implementation.

- [`nonnegative-literal-comparisons`](experiments/nonnegative-literal-comparisons.md) (`LANGUAGE.md` Comparisons): a decimal integer
  literal without a `u` suffix may be compared with `<`, `<=`, `>` or `>=`
  against an unsigned operand, so `s > 0` is accepted for `s: std::size_t`.
  Such a literal is non-negative and converts to either signedness without
  changing its value. The safety check treats it as compatible with both,
  and the comparison is emitted as `cpp2::impl::cmp_less_literal` and its
  siblings instead of `cpp2::impl::cmp_*`; they reject `bool` operands as
  `cmp_*` does, but not mixed signedness (`runtime/experiments.patch`).
  Hexadecimal, octal and binary literals, which may be unsigned, keep the
  check. Touches `src/safety.cpp` (`comparison_checker`) and
  `src/cpp1_expression_renderer.cpp` (`either_sign_literal`).
- [`move-only-to-known-callees`](experiments/move-only-to-known-callees.md) ([`LANGUAGE.md` "Moves and last use"](LANGUAGE.md#moves-and-last-use), the alternative to moving
  into a non-const lvalue reference parameter): a definite last use that is a
  whole call argument, `f(x)`, is moved only when the callee is known to take
  it by value or rvalue reference, that is when `f` names functions of this
  program whose overloads viable for the argument count all have a `copy`,
  `move` or `forward` parameter there. The object of a UFCS call `x.f()` is
  the first argument, so a member function `f: (move this)` moves it.
  Otherwise, as for `std::exchange(x, 0)`, `v.push_back(x)` and `x.size()`,
  the mention is a use that is not moved, and `move x` asks for the move. A
  last use in another position (`x + y`, `y := x;`, `return x;`) still
  moves. Touches `src/safety.cpp` (`last_use_finder::unmoved`).
- [`defer`](experiments/defer.md) (none): `defer statement;` or `defer { ... }` runs its body when the
  enclosing block exits, by falling off its end, a jump or an exception, in
  reverse order of registration. It lowers to a `cpp2::finally` guard holding
  a `[&]` lambda. A body that returns, or breaks or continues out of itself,
  is `defer-control-flow`; a `defer` outside a function body is `defer-scope`;
  while enabled, `defer` cannot be declared (`defer-name`). A name the body
  mentions has no definite last use in its block, and the safety analysis
  (initialization, `use-after-move`, `linear`) analyzes the body where a
  path leaves the block: at its end and at `return`, `break` and `continue`,
  in reverse order. Only these normal exits are checked, not an exit by an
  exception, so a defer body may read a deferred local that a later
  statement initializes if an earlier statement throws. In a function with a
  defer, postconditions are checked after the defers have run, on every exit
  path: the guard that checks them is declared after the named return values
  and before any defer, and its destructor runs them. `return s;` of a name
  that a defer registered before it in an enclosing block mentions lowers to
  `return std::as_const(s);`, and named return values are copied rather than
  moved: with Cpp1's implicit move on return the defer would see `s` moved
  from, and its change to `s` would reach the result only under NRVO. So the
  defer sees `s` intact and the result is the value at the `return`; a
  move-only `s` that a defer mentions cannot be returned. Touches the line
  classifier (`src/source.cpp`), the parser (`parse_defer`), `ast.hpp`
  (`defer_statement`), `src/ir_builder.cpp`, `src/safety.cpp` (last uses) and
  `src/lowering.cpp`.
- [`unsafe`](experiments/unsafe.md) (none): `unsafe { ... }` is a block in which everything lexically
  inside, function expressions and string interpolations included, is
  translated as with `-no-null-checks`, `-no-div-zero-checks`,
  `-no-subscript-checks` and `-no-comparison-checks` (so a mixed-sign
  comparison is accepted too), and without checked arithmetic
  (`disable_runtime_checks`); it lowers to a plain block marked `// unsafe`,
  and the checks resume after it. Outside a function body it is
  `unsafe-scope`; `unsafe` not followed by `{` stays an identifier. Touches
  the line classifier (`src/source.cpp`), the parser (`parse_unsafe`,
  `skip_misplaced_statement`), `ast.hpp` (`unsafe_block`),
  `src/ir_builder.cpp`, `src/safety.cpp` (last uses, as a block, and
  `check_comparisons`) and `src/lowering.cpp`.
- [`destructuring`](experiments/destructuring.md) (none): `(a, b) := value;` binds the elements of a
  tuple-like value to deduced locals, lowering to `auto [a, b] = value;`; `_`
  binds a `[[maybe_unused]]` generated name. It is its own statement form,
  `destructuring_statement`, and each name is an ordinary local for name
  lookup and initialization. A name's definite last use, including
  `return a;`, is `CPP2_FORWARD(a)`: it moves an element held by value and
  copies one bound by reference, as through `std::tie`, which a move would
  empty. Rejected at file, namespace and
  type scope, with fewer than two names, duplicate names, a declared type or
  a list initializer. Touches the line classifier (`src/source.cpp`), the
  parser, `ast.hpp`, `src/ir_builder.cpp`, `src/scope_model.cpp`,
  `src/safety.cpp`, `src/lowering.cpp`, `src/pretty_print.cpp` and the
  statement walks of the metafunctions.
- [`trait`](experiments/trait.md) (none): metafunctions `@trait` and `@impl`. A `@trait` type of
  bodyless `this` or `inout this` functions, declared at namespace scope
  (`trait-scope`), becomes a C++20 concept, all generated as Cpp2: `Trait`
  calls `Trait_calls<T>::__cpp2_satisfied()`, which requires each function
  as a member or as a free function that UFCS reaches (`x.f()` tries
  `f(x)`); `Trait_archetype` has exactly the trait's functions; and
  `dyn_Trait` is a type-erased value holding any type that satisfies the
  trait, through an inner `@interface` and a model template that calls each
  function through `Trait_calls`. `Trait_calls` is defined after every other
  type definition and function declaration (`ir_declaration::is_defined_last`),
  so it sees types and functions declared anywhere. A trait function returns
  void (a non-void implementation's result is discarded), `-> T` or
  `-> forward T`, and has no default arguments, contracts, or `forward` or
  deduced parameters. A declaration named like a generated one in the
  trait's scope is `trait-generated-name`. `@impl<Trait>` on a type, or a
  marker `M: @impl<Trait, Type> type = {}`, adds `static_assert(Trait<Type>)`.
  A namespace-scope function template whose template parameters are all
  traits gets a `static_assert` that instantiates it with the archetypes, so
  its body is checked against the trait. These checks are trailing Cpp1 text
  (`ir_declaration::trailing_cpp1`), emitted after every function definition,
  since Cpp2 has no namespace-scope `static_assert`. Traits are resolved by
  name lookup where `@impl` and `@given` name them, and a trait used as the
  type of an object, parameter or return value is `trait-as-type`. Touches
  `src/trait_metafunction.cpp`, `src/reflection.cpp` and `src/lowering.cpp`.
- [`given`](experiments/given.md) (none; builds on `trait`): metafunction `@given`.
  `G: @given<Trait, Type> type = { f: (x: Type, ...) = ...; }` implements a
  trait for a type from outside it: its functions take the instance first
  instead of `this`, and become free functions following `G`, which is left
  an empty marker. A given defines every function of its trait
  (`given-missing-function`) and is declared in the namespace that declares
  the trait (`given-scope`), so the trait's concept and `dyn_Trait` find its
  functions. UFCS reaches them where ordinary lookup finds a free function:
  inside that namespace, through a `using`, or through argument-dependent
  lookup for a class type; elsewhere, `n.show()` on an `int` does not find
  `lib::show`. Touches `src/trait_metafunction.cpp` and `src/reflection.cpp`.
- [`adt`](experiments/adt.md) (none): metafunction `@adt`, a sum type whose cases are bodyless
  functions `Circle: (r: double);`. Each case becomes a nested `@struct`
  with those fields and `==` (and `<=>` in an ordered type such as
  `@value`), built as `Circle(1.0)`; the type holds a `std::variant` of the
  cases, converts implicitly from each, and has `is_Case()` and `as_Case()`
  accessors. Unless it declares `(out this, that)` or is `@copyable` or a
  value, its copy and move are Cpp1's, so a case may hold a move-only field
  such as `std::unique_ptr` of the type itself; a `@value` default-constructs
  to the first case. The runtime overloads `is` and `as` for such types
  (`runtime/experiments.patch`), so `inspect` and `x as Case` work
  unchanged. Diagnostics: `adt-case-form`, `adt-duplicate-case`, `adt-empty`.
  Touches `src/reflection.cpp` (`apply_adt`) and `runtime/cpp2util.h`.
- [`patterns`](experiments/patterns.md) (none): an `is` alternative of `inspect` may destructure,
  `is T(a, _)` (a structured binding of `s as T`) or `is (a, b)` (of the
  subject), list alternatives `is A | B` (types or values), bind `is T as c`,
  and add a guard `is T(r) if r > 1.0`; the parser puts each part in a slot
  of the alternative (`inspect_alternative_of`). A destructuring lists names
  and `_` and is not combined with `|`, and `_` takes neither. The bound
  names hide outer ones in the guard and the value. Alternatives lower to
  independent `if`s so a failed guard falls through, in a template an
  alternative binding `s as T` is left out where that conversion is invalid
  or yields `nonesuch` (its test fails there), a tuple pattern is left out
  where the subject is not tuple-like, an array or an aggregate
  (`cpp2::impl::destructurable` in `runtime/cpp2util.h`), and a chain
  without an alternative matching anything, other than such a tuple
  pattern, ends in `std::terminate()`. Only an
  unguarded `_` or tuple pattern, with or without `as c`, matches anything;
  `validate_cpp2` still requires one in every inspect, and
  `check_inspect_patterns` removes that error from an inspect whose subject
  is declared with an `@adt` or `@enum` type (the cases reflection records
  in `sum_cases`; a `@flag_enum` has none) and whose unguarded alternatives
  cover every case, or reports `inspect-not-exhaustive` with the missing
  ones. Without the experiment the new forms are `invalid-inspect`. Touches
  `src/expression_parser.cpp`, `src/cpp2_validation.cpp`,
  `src/cpp1_expression_renderer.cpp` (`render_inspect`), `src/safety.cpp`
  (bindings are not mentions), `src/reflection.cpp` (`sum_cases`),
  `src/inspect_patterns.cpp`, `src/translator.cpp` and `runtime/cpp2util.h`.
- [`try-operator`](experiments/try-operator.md) (none): a postfix `e?` unwraps a `std::expected` or
  `std::optional` and returns `cpp2::try_failure(e)` (`std::unexpected` of
  the error, or `std::nullopt`) from the enclosing function when there is no
  value. Each operand is hoisted into `auto&& __cpp2_try_N =
  cpp2::try_hold(e); if (!N) return ...;` before its statement, on the same
  line, and the use reads `*N`; `try_hold` keeps an lvalue `e` a reference and
  moves an rvalue into a value, so an xvalue into a temporary, as in
  `std::get<0>(make())?`, does not dangle. Hoisting evaluates every `?`
  operand of a statement before the rest of it, so in
  `std::cout << step() << get()?;` `get()` is called before `step()`, and
  `step()` is not called when `get()` fails.
  `runtime/cpp2try.h` is included when lowering hoists an operand. `?`
  outside a function body statement that can return early, as in a
  condition, a defer body (which runs in a scope guard), a comprehension's
  guard or value (which run in a lambda), a `static` local, a contract, a
  parameter default, a requires clause or a named return value, is
  `try-operator-context`; in a function without a declared `expected` or
  `optional` return type it is `try-operator-return-type`, and right of `&&`
  or `||` `try-operator-short-circuit`. While enabled, a Cpp1 `c ? a : b` is
  a parse error rather than `cpp1-conditional-operator`.
  Touches `src/expression_parser.cpp` (through `token_cursor::expression`),
  `src/cpp2_validation.cpp`, `src/cpp1_expression_renderer.cpp`
  (`render_try`), `src/lowering.cpp` (hoisting), `src/include_policy.cpp`,
  `src/try_operator.cpp` and `src/translator.cpp`.
- [`newtype`](experiments/newtype.md) (none): `@newtype<U>` makes a distinct value type holding one `U`
  in the private member `value`, with an explicit constructor from `U` and no
  assignment from it, copy, `<=>`, an explicit conversion to `U` and a
  hidden-friend `as<U>` (the only Cpp1 text, as Cpp2 spells neither); a
  member named `value` is `newtype-member-name`. `@quantity<U>` adds
  same-type `+ - += -=`, unary `-`, scaling by `U`, `k * q` as an
  `operator*` at namespace scope after the outermost enclosing type, found by
  argument-dependent lookup (so a nested quantity must not be private or
  protected, or be in a template type, `quantity-scope`) and the ratio of two
  quantities. `@unit<U, L, M,
  T, ...>` makes the type an alias of `cpp2::quantity<U, cpp2::dims<...>>`
  from `runtime/cpp2units.h`, where `*` and `/` add and subtract exponents; a
  unit is its dimensions, so two non-template `@unit` types with the same
  underlying type and exponents, which would be one type, are
  `unit-duplicate`. `@newtype` and `@quantity` combine only with
  `@hashable`, `@delegate` and `@print`, and `@unit` only with `@print`
  (`newtype-composition`). The safety pass reports `x as T` between two such
  types, for a name declared with one or a deduced local initialized with
  `x as T` or `T(...)`, as `newtype-conversion`. The runtime gains
  `as<C, literal>()` for a type that declares `cpp2_newtype_rep` (only these
  types do), so `7 as Count` works. Touches `src/reflection.cpp`
  (`apply_newtype`, `apply_unit`, no converting assignment for an
  `explicit` constructor in `generate_value_set`), `src/safety.cpp`
  (`distinct_types_of`, `deduced_distinct_type`), `src/lowering.cpp`
  (`explicit` for the generated constructor, the include),
  `include/cppfrontier/ir.hpp` (`ir_declaration::is_explicit`) and
  `runtime/cpp2util.h`.
- [`checked-arithmetic`](experiments/checked-arithmetic.md) (none): `+`, `-`, `*`, unary `-`, `+=`, `-=`, `*=`,
  `++` and `--` whose operands are all known integers (integer literals,
  names declared with an integer type or deduced from a known initializer,
  data members declared with an integer type named as `x`, `this.x` or
  `that.x`, `as`/typed values of an integer type, and integer operators
  over those)
  lower to the `cpp2::checked_*` helpers of `runtime/cpp2checked.h`, which
  report an `arithmetic_safety` violation when the result does not fit the
  type of the plain operator (or, for the compound forms, the stored-to type).
  Calls, members of other objects, range-`for` variables and deduced
  parameters keep the plain operators,
  as does an `unsafe` block. Unsigned arithmetic is checked too, so `i - 1`
  for `i: u32 = 0` is a violation; arithmetic meant to wrap, such as a hash,
  calls `wrapping_add`, `wrapping_sub` or `wrapping_mul` (`h.wrapping_mul(p)`),
  which compute the plain operator's result modulo 2^N and are support names
  like `unchecked_div`. Touches `src/cpp1_expression_renderer.cpp`
  (`integer_knowledge`, `wrapping_names`), `src/include_policy.cpp` and
  `src/lowering.cpp`.
- [`delegate`](experiments/delegate.md) (none): `@delegate<member>` generates, for each public function
  of the data member's type that takes an `in` or `inout` `this` and is not
  an operator or special member, a forwarding function with the same name,
  template parameters, parameters and return form that calls
  `this.member.f<Ts>(args...)`, as ordinary IR members. Names in its
  parameter types, return type and default values that name a type,
  namespace or alias declared in the member type's scopes are qualified from
  the global namespace (`::lib::Counter::Kind`), and named return values
  return the member type's `f_ret`. A function the type has is kept,
  `@delegate<member, f, g>` forwards only those (one it cannot forward is
  `delegate-not-forwardable`), and a name two `@delegate`s would forward is
  `delegate-ambiguous`. The member's type must be a non-template type of this
  translation unit (`delegate-unknown-type`, `delegate-template-type`).
  Touches `src/reflection.cpp` (`apply_delegate`).
- [`use-after-move`](experiments/use-after-move.md) (none): a flow analysis beside definite initialization in
  `analyze_safety` follows explicit moves (`move x` arguments) of locals,
  destructuring bindings, and non-`in` parameters (of functions, statements,
  range `for` loops and handlers) of a type known not to be a scalar (written,
  or deduced from an initializer of known type; see `src/known_types.hpp`). A later mention before
  the name is assigned again (`x = ...`, `out x`) is `use-after-move` when
  every path moved it and `use-after-maybe-move` when some path did, reported
  once per move; a name moved and mentioned again in one statement is
  `use-after-move`, since the order of evaluation is unspecified. A loop is
  analyzed again from the join of the states reaching it and its next
  iteration: a use found there of a move made inside the loop is
  `move-in-loop`, and after the loop the name may have been moved. A path
  that leaves (`return`, `break`, `continue`, `throw`, and the early return
  of a hoisted `?`) does not reach the code after its statement; returning
  reads the named return values. Inspect alternatives are separate paths,
  and a `try` handler starts from the join of the states the try block may
  throw from. Touches `src/safety.cpp` and `src/known_types.cpp`.
- [`linear`](experiments/linear.md) (none): `@linear` rejects a copy `operator=` (`linear-copy`),
  generates a missing memberwise move constructor and move assignment and
  deletes the copy functions. The flow analysis of `use-after-move` then
  follows each value of a `@linear` type of this translation unit (without
  needing that experiment): a local, binding or non-`in` parameter whose
  type is known to be that type, as for `use-after-move`, including a range
  `for` parameter over a `std::vector<T>`. A `move`, `copy` or `forward`
  parameter must be consumed exactly once. A value is consumed only by being a whole
  argument for a `copy`, `move` or `forward` parameter of a function of this
  translation unit (including `this` of a UFCS call `x.f()`, and the first
  parameter of a free `f` when the type of `x` has no member `f`), a whole
  initializer (including a capture `x$`), `return x;`, or `move x`; a mention
  in a contract never consumes, and reading a part never does, so a function
  ending a value's life passes it on, such as to a `move this` member. A path
  leaving the value's scope without consuming it, or assigning over it
  (`x = ...`, `out x`) before it is consumed, is `linear-not-consumed`; an
  `inout` or `out` parameter instead must not be left moved from. A second
  consumption is `linear-consumed-twice`, and a path ending in `throw` is not
  checked. Touches `src/reflection.cpp` (`apply_linear`) and `src/safety.cpp`.
- [`exclusivity`](experiments/exclusivity.md) (none): `check_exclusivity`, after symbol resolution,
  rejects a call that passes an object `inout` or `out` to a function
  declared in this translation unit while another argument of the call
  mentions that object or a part of it (`exclusivity-violation`, at the later
  argument). Objects are locals, parameters, members as `m` or `this.m`, and
  `this`; distinct parts `x.a` and `x.b` of an object are disjoint. A mention
  is any outside a compile-time context (`sizeof`, `decltype`, template
  arguments), including captures and string interpolations. The receiver of
  a member or UFCS call, `x.f()`, `(x).f()` or the implicit `this` of
  `f()`, is the first argument, bound as for `linear`. Parts are member
  paths of the expression tree. Unknown callees are skipped, and overloads
  must agree on the passing.
  Touches `src/exclusivity.cpp` and `src/translator.cpp`.
- [`interface-defaults`](experiments/interface-defaults.md) (none): a function of an `@interface` type may have a
  body, which makes it `virtual` with that body as its default
  implementation; a bodyless function stays pure virtual. When no function is
  pure, the generated default constructor is `protected`, so the type still
  cannot be created directly. A function without `this` is
  `interface-function-this` with or without a body, and a constructor with a
  body is still `interface-function-body`, since it cannot be virtual.
  Touches `src/reflection.cpp` (`apply_interface`).
- [`sealed`](experiments/sealed.md) (none; builds on `patterns`): metafunction `@sealed` on a
  `@polymorphic_base` or `@interface` type at namespace scope closes its
  hierarchy to this translation unit. Its derived types are the types in its
  scope whose base names it; after reflection, `seal_hierarchies` makes every
  constructor of the base private and declares each derived type a friend
  (the generated member `__cpp2_sealed_friends`), and records the derived
  types as the base's `sum_cases`, so an `inspect` of `s: Shape`, or `s*` for
  `s: *Shape`, that covers each needs no `is _`, and `inspect-not-exhaustive`
  lists the missing ones. Diagnostics:
  `sealed-not-polymorphic`, `sealed-template`, `sealed-scope` (nested in a
  type), `sealed-derived-scope` (a derived type in another scope) and the
  warning `sealed-no-derived`. Touches `src/reflection.cpp` (`apply_sealed`,
  `seal_hierarchies`, `sealed_derived_types`) and `src/inspect_patterns.cpp`.
- [`lazy`](experiments/lazy.md) (none): `name: lazy T = init;` at namespace or type scope declares an
  object whose initializer runs on its first use, exactly once
  (`std::call_once`), which avoids the static initialization order problem.
  `lazy` takes the slot before the type, as `final` does before `type`, and
  only when a type follows it. The object lowers to a constant-initialized
  `cpp2::lazy<T>` holder from `runtime/cpp2lazy.h` whose initializer is a
  captureless lambda returning `T{init}`; a namespace-scope object keeps the
  `extern` declaration and late definition, and a type-scope one is a
  `static` member defined `inline` after the types, which generated and
  memberwise functions leave out, and which is no enumerator or alternative.
  A use renders as `name.value()` where lookup of the (qualified) name finds the lazy object,
  so a local or member that hides it is left alone; every mention is
  rewritten the same way, default arguments and signatures included.
  Diagnostics: `lazy-scope` (a local), `lazy-initializer` (no `=`
  initializer, or `==`), `lazy-type` (a deduced type). Without the
  experiment `lazy` is an ordinary type name. Touches the parser
  (`consume_lazy`, `src/statement_parser.cpp`), `ast.hpp`
  (`declaration_signature::is_lazy`), `src/cpp1_expression_renderer.cpp`
  (`names_lazy_object`) and `src/lowering.cpp`.
- [`comprehension`](experiments/comprehension.md) (none): `for range do (x) [if guard] yield value` is an
  expression, and `for ...` in place of `yield value` nests a generator. It
  parses as `expression_kind::comprehension` and lowers to
  `cpp2::comprehend(range, [guard,] value)`, or `cpp2::comprehend_bind` for a
  nested generator, with the guard and value as lambdas over
  `auto const& x` that capture by reference (nothing outside a function) and
  copy the names bound by enclosing generators. `runtime/cpp2comprehension.h`
  makes a range a lazy `filter | transform` (`| join`) view and a type with
  `.and_then` and `.transform` (`std::optional`, `std::expected`) a monadic
  chain; a guard on an `expected` is a `static_assert`. An lvalue range the
  view cannot hold, such as a move-only view passed `in`, is referred to by
  `std::ranges::ref_view`. A view that outlives
  the locals its lambdas mention dangles, as any Cpp1 view would. The bound
  name hides outer names: it is not a mention for the safety analyses and is
  rendered as is. Nothing inside a comprehension is moved from. A `for` in an
  expression that is not a comprehension is `comprehension-yield`;
  `x := for ...` is an initializer rather than a loop label, and a loop
  header skips brackets, so `for (for xs do (x) yield x) do (y)` works.
  Without the experiment, `for` in an expression stays a syntax error.
  Touches `src/expression_parser.cpp` (through `token_cursor::expression`),
  `src/parser_context.cpp`, `src/statement_parser.cpp`,
  `src/cpp1_expression_renderer.cpp` (`render_comprehension`),
  `src/safety.cpp` (`for_each_mention`), `src/include_policy.cpp` and
  `src/lowering.cpp`.
- [`type-operators`](experiments/type-operators.md) (none): in a type position (an object, parameter or return
  type, a `type`-marked template argument, a type alias's target) `A | B` is a union type
  and lowers to `std::variant<A, B>`; `|` binds loosest, `*` and `const`
  apply to one operand, and a parenthesized operand groups. In a template
  argument `|` and `&` are Cpp1 operators, as in `std::bitset<A | B>`,
  unless the argument is marked `type`: `std::vector<type int | std::string>`;
  an unmarked argument with an operand that can only be a type is
  `type-operator-argument`. A written nested
  union is flattened, alternatives spelled alike in Cpp1 are kept once, and
  a union left with one alternative is that type; a union named by an alias
  stays one alternative. In a constraint, a template parameter `T: A & B` or
  `_ is A | B`, `&` and `|` are a conjunction and disjunction of concepts;
  Cpp1 has no `A & B auto`, so the parameter becomes `typename T` (or
  `auto`) and the check `A<T> && B<T>` goes into the declaration's `requires`
  clause, joined with a written one by `&&`; an object `x: _ is A | B = v`,
  local or at namespace scope, and a loop or statement parameter become
  `auto x` and a `static_assert`. Each operand is checked by
  `constraint-not-concept`. Diagnostics: `type-operator-single` (an operand
  missing; outside a constraint, `int&` stays `cpp1-reference-type`) and `type-operator-constraint` (`&` in a type position). Without
  the experiment the operators stay Cpp1 text. Not covered: operators in
  expressions (`x is (A | B)`, `as` targets), where `|` and `&` keep their
  meaning. Touches `src/type_model.cpp` (`union_type`, `intersection_type`,
  `type_parse_options`), the parser (`type_end`, `syntax_type`,
  `template_parameters`, type aliases), `ast.hpp`
  (`template_parameter::constraint`), `src/cpp1_type_renderer.cpp`,
  `src/constraint_checker.cpp` and `src/lowering.cpp`.
- [`condition-bindings`](experiments/condition-bindings.md) (none; builds on `patterns`): an operand of the
  top-level `&&` chain of an `if`, `else if` or `while` condition may be a
  pattern test that binds, `v is T as n`, `v is T(a, _)` or `v is (a, b)`,
  with `patterns`' forms; the expression parser makes it a `pattern_test`
  node whose second child is an `alternative`. The names are `auto&&`
  references in scope in the later operands and the guarded body only
  (`if_form::bindings`, `loop_form::bindings`; `scope_model::add_bound`).
  A pattern test anywhere else is `condition-binding-position`, including
  under `||`, `!` or parentheses, and in `do`-`while`, `next`, `if
  constexpr` and `for`; a binding that is not a name, `as _` and a name
  bound twice are `invalid-condition-binding`. The subject is evaluated
  once and never moved, and while a binding is used the names the subject
  mentions are not moved either; an `as` binding's definite last use
  forwards it. A chain lowers to nested `if`s, with a flag when there is an
  `else`; a `while` to `for (;; next) { ...; if (!(test)) break; ... }`.
  Without the experiment the forms keep their errors (`mixed-is-as`,
  `juxtaposed-operands`). Touches `src/expression_parser.cpp`
  (`pattern_test`), `src/parser_context.cpp` (`check_pattern_tests`),
  `src/statement_parser.cpp`, `src/translator.cpp`, `src/ir_builder.cpp`,
  `src/scope_model.cpp`, `src/mentions.hpp`, `src/safety.cpp`
  (`add_referents`, `find_local_last_uses`), `src/cpp1_expression_renderer.cpp`
  (`render_cpp1_condition`) and `src/lowering.cpp` (`render_bound_if`,
  `render_bound_while`).
- [`let-else`](experiments/let-else.md) (none): a local declaration
  `name := value as T else { ... }`, or with `destructuring`
  `(a, b) := value as T else { ... }`, declares `name` when `value is T`
  and otherwise runs the block. No `;` follows the block. The statement
  parser (`parse_let_else`) parses the part before `else` as the
  declaration it spells and stores the block in the form's `else_body`
  (`is_let_else`), which `for_each_nested_list` visits, so the `defer` and
  `try-operator` rules apply in it. The initializer must be one `value as
  T` (`let-else-initializer`), and the block's last statement `return`,
  `throw`, `break`, `continue`, a call to `std::terminate`, `std::abort`,
  `std::exit`, `std::quick_exit` or `std::unreachable`, or an `if` with an
  `else` whose branches each end so (`let-else-falls-through`). The names
  are declared at a scope item after the statement, so they are not in
  scope in the initializer or the block. The flow analyses treat the block
  as a path that leaves; the names are references into the value, which is
  not moved while they are used, `name` is forwarded at its last use and
  destructured names are never moved. It lowers to `auto&& __cpp2_let_L_C =
  value; if (!cpp2::impl::is<T>(...)) { ... } auto&& name =
  cpp2::impl::as<T>(...);`. Without the experiment the form keeps its
  `incomplete-expression` error. Touches `src/statement_parser.cpp`,
  `ast.hpp`, `src/ir_builder.cpp`, `src/scope_model.cpp`, `src/safety.cpp`
  (`let_else_initializer`), `src/cpp1_expression_renderer.cpp`
  (`render_cpp1_let_else`) and `src/lowering.cpp` (`render_let_else`).
- [`next-list`](experiments/next-list.md) ([`LANGUAGE.md` "Statements"](LANGUAGE.md#statements)): a loop's
  `next` clause may be a list `next (e1, e2, ...)` of two or more items,
  which run in order at the end of each iteration of a `while`, `do` or
  range `for` loop, also after `continue`. `(x)` and `(x,)` stay one
  expression, and a trailing comma is ignored. Each item is checked as a
  `next` clause of its own, so it may be one assignment (`next (i += 1, j
  -= 1)`) but not a chain or a list, and `_ = x` discards `x`. The items
  are Cpp1 statements, never a Cpp1 comma operator, which an overloaded
  `operator,` could take over: a `while` runs them in a lambda
  `[&]{ e1; e2; }()` in the Cpp1 `for` increment, a `do` in the lambda
  before its condition, and a range `for` as statements after the body.
  Without the experiment a list is `next-clause-list`. Touches
  `src/statement_parser.cpp` (`check_next`), `src/expression_parser.cpp`
  (`whole_list`), `src/cpp1_expression_renderer.cpp`
  (`render_cpp1_expression_items`) and `src/lowering.cpp`
  (`render_next_items`).

## Part of the default language

These experiments are the default language, or a default rule covers them;
each page records the design.

- [`contract-uses-never-move`](experiments/contract-uses-never-move.md):
  a mention in an `assert` statement is a use that is never moved.
- [`forward-return-bare-names`](experiments/forward-return-bare-names.md):
  a default rule covers it: a `-> forward` return may not refer
  into a local or an `in`, `copy` or `move` parameter through member accesses, subscripts,
  calls or postfix operators, up to a postfix `*`.
- [`reject-postfix-star-template-arguments`](experiments/reject-postfix-star-template-arguments.md):
  a template argument ending in a postfix `*`, such as `std::vector<Foo*>`, is an error.
- [`cpp2-requires-parameters`](experiments/cpp2-requires-parameters.md):
  requires-expression parameters use Cpp2 syntax, `requires (inout a: T) { ... }`.
