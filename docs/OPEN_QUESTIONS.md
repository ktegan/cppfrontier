# Open Language Questions

Points where the Cpp2 documentation is unclear or silent, found by porting Cpp1
material by hand (`tests/ports/`). Until one is settled, cppfrontier's current
behavior stands. Settled questions are stated in [LANGUAGE.md](LANGUAGE.md)
and `UPSTREAM_AUDIT.md`.

## Semantics

**Left to the Cpp1 compiler** ([LANGUAGE.md "Declarations"](LANGUAGE.md#declarations)), which reports them clearly:
- a template parameter that shadows an enclosing one;
- a fold or `sizeof...` over a non-pack;
- an unexpanded pack.

## Documentation discrepancies (recorded in UPSTREAM_AUDIT.md)

- `x is std::integral` is documented as a concept query, but neither
  translator can tell a concept declared in Cpp1 from a type template.
- `pb as *D` is documented as `dynamic_cast<D*>(pb)`, but cpp2util rejects it;
  `p* as D` works.
- The contract handler type is documented as `* (* const char)`, but cpp2util's
  handler also takes a `std::source_location`.
- The `skat_game` comment in metafunctions.md says `@enum` values start at 1,
  but both translators start at 0.
