# Cpp2 runtime

The headers that generated Cpp1 includes: `cpp2util.h` and its companions
`cpp2regex.h`, `cpp2taylor.h`, `cpp2ad_stack.h` and `cpp2units.h`. All but
the last are copied from
upstream cppfront `include/` (checkout 66eb614) so that language experiments can
change the runtime too. `cpp2try.h` is cppfrontier's own: the `try-operator`
experiment's `cpp2::try_hold` and `cpp2::try_failure` (docs/EXPERIMENTS.md).

Differences from upstream in `cpp2util.h`. The first three are collected in
`upstream.patch` for submission upstream, the rest in `experiments.patch`:

- `CPP2_ASSERT_IN_BOUNDS_IMPL` builds its message only when the check fails, so
  a bounds-checked subscript can be evaluated in a constant expression.
- `unchecked_cast<T>(x)` falls back to `reinterpret_cast` when `static_cast`
  does not apply (between unrelated pointer types, or a pointer and an
  integer), so it is the one spelling for every type-unsafe cast
  (expressions.md "Casts").
- `deferred_init` and `out` have `construct_from(make)`, which constructs the
  object from the prvalue `make()` returns, so a list `x = (1, 2)` assigned
  to a deferred local or an uninitialized `out` parameter needs no copy or
  move of the object and still does not narrow.
- `as<C, literal>()` constructs a type `C` that declares `cpp2_newtype_rep`
  (a `@newtype`, `@quantity` or `@unit` type of experiment `newtype`) from a
  literal when `C{literal}` is valid, so `7 as Count` works. No other type
  declares the name, so the overload does not change existing programs.
- `cmp_less_literal` and its siblings, which experiment
  `nonnegative-literal-comparisons` emits for a comparison with a
  non-negative decimal literal (`s > 0`): they reject `bool` operands as
  `cmp_less` does, but not mixed signedness, since such a literal converts to
  either signedness without changing its value, and `-Wsign-compare` is off
  inside them.
- `is` and `as` for an `@adt` type (experiment `adt`), a type with a
  `cpp2_adt_value()` returning a `std::variant`, look through it to that
  variant; `as` only to one of the variant's alternatives, so any other
  conversion is the general `as`. They must be declared before `as_` for
  `x as T` to find them.
- `destructurable<T>`, which experiment `patterns` tests before a tuple
  pattern `is (a, b)` takes the subject apart: `T` is tuple-like, an array
  or an aggregate.

Differences in `cpp2regex.h`, collected in `regex.patch`, which follow Perl's
matching rules (notes/regex_status.md) and go with the matchers that
`src/regex_metafunction.cpp` generates:

- A lazy quantifier backtracks into its repetitions as a greedy one does, and a
  possessive one takes the first greedy match; both used to take each
  repetition's first match only, so `(?:a|ab)+?c` did not match `abc`.
- A repetition that matches nothing ends the loop, also at the end of the
  string, so `(a?)+?b` no longer loops forever and `(a?)?` matches `""` with
  group 1 set.
- A backreference to a group that has not matched fails.
- `^`, `$` and `\Z` test the position in the forward string, so they also work
  in a lookbehind, and `^` with the `m` flag does not match after a newline
  that ends the string.
- A search tries every position also for a pattern with `\G`, which the
  generated matcher checks as an assertion, so `matcher::is_start_match` is gone.
- `search`, `match` and `find_all` with a string and a `start` search the
  string from `start` as Perl does from pos(): `^`, `\A`, `\b` and lookbehind
  see the text before `start`, group positions count from the start of the
  string, and `\G` (`search_start_token_matcher`) matches at `start`, or for
  `find_all` where the previous match ended, as Perl's `//g`.
- A group is opened (`open_group`) and closed (`close_group`); its match
  changes only when it closes, so a backreference inside the group sees its
  previous repetition. A generated function that fails restores the groups
  as they were on entry, so the group reset functions are gone.

`cpp2units.h` is cppfrontier's own: the `cpp2::quantity` template that `@unit`
types alias (docs/EXPERIMENTS.md).

`cpp2checked.h` is this project's own: the `cpp2::checked_*` helpers of
experiment `checked-arithmetic`, included after `cpp2util.h` when the
generated Cpp1 calls one.

`cpp2lazy.h` is this project's own: the `cpp2::lazy<T>` holder of experiment
`lazy`, included when a file declares a lazy object.

`cpp2comprehension.h` is this project's own: the `cpp2::comprehend` overloads
of experiment `comprehension`, included when the generated Cpp1 calls one.

To update, copy upstream's `include/*.h` here and run `patch -p2 < upstream.patch`,
`patch -p2 < experiments.patch` and `patch -p2 < regex.patch` in this directory.
