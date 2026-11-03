# cppfrontier Architecture

This document is the authoritative description of the implemented pipeline
and its Cpp2 coverage. The project's goals are in [README.md](../README.md);
full compatibility with upstream is one of them, and is measured by the corpus
checks under [Verification Layers](#verification-layers).

## Pipeline

```text
source bytes
  -> source_text: immutable text plus mixed-mode line categories
  -> tokenize: tokens for each bounded Cpp2 section
  -> extract_function_expressions: unnamed functions become generated declarations
  -> validate_cpp2: source-form diagnostics for unsupported Cpp2 spellings
  -> parse_cpp2: AST plus parser diagnostics
  -> build_ir: ir_program with stable symbol IDs
  -> apply_reflection: built-in metafunction expansion and metaclass diagnostics
  -> resolve_symbols: token-to-symbol references by scope lookup
  -> check_constraints: `_ is C` constraints against concepts declared here
  -> analyze_safety: initialization diagnostics and the deferred-construction plan
  -> emit_cpp1: Cpp1 text, safety rewrites, includes, and source map
```

`translator::translate_source` runs the pipeline and returns a
`translation_result`; the command line, the parallel driver and the language
server are adapters over it. If any diagnostic has error severity, emission is
skipped and `cpp1` is empty. Diagnostics are sorted by source position.
Parallel translation runs whole files as independent jobs, at most `-j N` at
a time, each with its own translation state, and returns results in request
order. The language rules the stages implement are in
[LANGUAGE.md](LANGUAGE.md).

## Modules

The project, its namespace, include paths and targets are named
`cppfrontier` in lowercase; macros use the `CPPFRONTIER_` prefix.

- `include/cppfrontier/source.hpp`, `src/source.cpp`: immutable source text,
  Cpp1/Cpp2/comment/preprocessor/raw-string line categories, sections, spans.
- `include/cppfrontier/token.hpp`, `src/lexy_lexer.cpp`: lexy scanner and the
  token post-pass for qualified names and multi-character operators.
- `include/cppfrontier/pipeline.hpp`, `src/cpp2_validation.cpp`:
  token-level validation for Cpp1-only type spellings, prefix address-of, and
  unsupported capture syntax.
- `include/cppfrontier/ast.hpp`, `src/parser.cpp`, `src/statement_parser.cpp`:
  bounded recursive-descent declaration and statement parsing.
- `include/cppfrontier/ir.hpp`, `src/ir_builder.cpp`: AST-to-IR construction,
  stable symbol allocation, and scope records for parameters (including
  statement, range and catch parameters), locals, and blocks.
- `include/cppfrontier/type.hpp`, `src/type_model.cpp`: `type_id`, the
  structured Cpp2 type-id. Declared, parameter and return types are parsed
  once by the parser, with their source spans; the expression renderer parses
  `as`/`is` targets, `inspect` result types, template type arguments and the
  type-ids of `sizeof`, `alignof` and `typeid` from their expression tokens.
  Kinds: qualified names with template arguments, fundamental types, `*T`, `const T`, function types with passing
  styles and a (`forward`) return, deduced `_`/`auto` with an optional
  `_ is C` constraint, packs, `decltype`/`type_of`, non-type template
  arguments, and a Cpp1 passthrough that keeps its text. `build_ir` marks
  qualified names whose scope depends on a template type parameter of an
  enclosing declaration as dependent. Lowering, safety, reflection and
  symbol resolution ask the model (`is_wildcard`, `constraint`,
  `pointer_depth`, `mentions_name`, `is_simple_type_specifier`) instead of
  inspecting type text.
- `src/cpp1_type_renderer.*`: the one renderer from `type_id` to Cpp1
  (declarators, qualifier order, function and function-pointer types,
  `typename`/`template` for dependent names).
- `src/constraint_checker.cpp`: `check_constraints`.
- `src/inspect_patterns.cpp`: `check_inspect_patterns`, the `patterns`
  experiment's exhaustiveness check for `inspect` over `@adt` and `@enum`
  subjects.
- `src/exclusivity.cpp`: `check_exclusivity`, the `exclusivity` experiment's
  check that an `inout` or `out` argument is not used by another argument.
- `src/ir_walk.hpp`: small declaration and statement traversal helpers shared by
  IR transforms. Traversal is post-order for declarations; it is not a general
  visitor framework.
- `src/reflection.cpp`: the metafunction registry (a table of name and
  applier) and the built-in metafunctions. `@enum`, `@flag_enum`, `@union`
  and `@adt` (experiment `adt`) generate their members as Cpp2 text like
  upstream, parsed into ordinary IR members, and unregistered metaclasses are
  diagnosed. `@newtype`, `@quantity` and `@unit` (experiment `newtype`) do the
  same; `@unit` makes its type an alias of `cpp2::quantity` from
  `runtime/cpp2units.h`.
- `src/generated_members.*`: parses metafunction-generated Cpp2 members into
  IR and maps diagnostics in them back to the source.
- `src/regex_metafunction.cpp`: the `@regex` pattern parser and the Cpp2
  matcher generator for upstream's `cpp2regex.h` runtime.
- `src/trait_metafunction.cpp`: `@trait`, `@impl` and `@given` (experiments
  `trait` and `given`), which replace a trait type with a concept and
  generate, as Cpp2, its requirements type, an archetype and a `dyn_`
  type-erasing value type; move a given's functions out as free functions;
  and add `static_assert` checks as trailing Cpp1 text
  (`ir_declaration::trailing_cpp1`), which lowering emits after every
  function definition.
- `src/autodiff_metafunction.cpp`: `@autodiff`, which generates derivatives
  as Cpp2 text and parses them into IR members before other reflection runs.
- `src/scope_model.hpp`, `src/scope_model.cpp`: the scope tree (namespace,
  type, function, block, statement-parameter and function-expression scopes)
  and Cpp2 name lookup at a statement, parameter, declaration or source
  position. Function bodies, blocks and statement parameters are ordered: a
  local is visible from its own declaration onward, including its
  initializer. Namespace and type scopes are unordered. A function
  expression's scope is nested where the expression appears. Members are
  visible through `this`, and `using` declarations and `using N::_;`
  directives are followed. The symbol resolver, the initialization and
  forwarding-return checks, the last-use analysis, the pointer checks, the
  UFCS member-call decision and the wrapping of deferred locals and `out`
  parameters each build one from the IR when they need it, so the IR holds no
  lookup results. `CPP2_UFCS_NONLOCAL` follows the emitting context, as
  upstream, rather than lookup.
- `src/symbol_resolver.cpp`: token-to-symbol references by scope lookup; a
  capture `x$` refers to what `x` names where its function expression appears.
- `src/safety.cpp`: initialization diagnostics. Default-on integer division and
  modulo checks are emitted by the expression renderer, not by this analyzer.
- `src/include_policy.cpp`: standard-header inference and shared `#include`
  parsing.
- `src/parser_context.*`: shared token cursors, balanced-delimiter scans, and
  parser diagnostics.
- `src/lowering.cpp`, `src/lowering_detail.cpp`,
  `src/cpp1_expression_renderer.cpp`: Cpp1 rendering, expression text lowering,
  include deduplication, and source mapping.
- `include/cppfrontier/options.hpp`, `include/cppfrontier/experiments.hpp`:
  `translation_options`, and the table of opt-in language experiments that
  stages query through `options.experiments.enabled("name")`; see
  `EXPERIMENTS.md`.
- `src/translator.cpp`: core pipeline and whole-file parallel driver.
- `src/language_server.cpp`: in-process document snapshots, symbols,
  diagnostics, and goto-definition.
- `include/cppfrontier/lsp.hpp`, `src/lsp.cpp`, `src/lsp_main.cpp`: the
  `cppfrontier-lsp` executable, a JSON-RPC 2.0 adapter over `language_server`
  on standard input and output with `Content-Length` framing. It handles
  `initialize` (whose `initializationOptions.experiments` enables
  experiments), `initialized`, `shutdown`, `exit`, full-sync
  `textDocument/didOpen`/`didChange`/`didClose` (each publishing
  diagnostics), `textDocument/documentSymbol` (flat `SymbolInformation`), and
  `textDocument/definition`, converting between byte columns and UTF-16 code
  units. It exits 0 after `shutdown` then `exit`, else 1. Editor features use
  the compiler's own tokens, symbols, diagnostics and source maps; there is
  no second grammar.
- `include/cppfrontier/json.hpp`, `src/json.cpp`: the JSON reader and writer
  for the LSP adapter and source maps.
- `src/main.cpp`: the command line, compatible with upstream's: its flags and
  unique prefixes (`-help`, `-version`, `-verbose`, `-quiet`,
  `-format-colon-errors`, `-emit-cppfront-info`, `-clean-cpp1`,
  `-line-paths`, `-import-std`, `-include-std`, `-pure-cpp2`, `-no-*-checks`,
  `-output`, `-cwd`, `-fno-exceptions`, `-fno-rtti`), status lines and
  `file(line,col): error:` diagnostics, default output paths, `.h` header
  guards, and exit codes, plus `-j N`, `-source-map file` and
  `-experiment=name`. `-debug`,
  `-_debug` and `-_gen_build` are rejected as not supported. It differs from
  upstream where upstream is silent or unsafe: every flag is validated before
  any file is translated, a flag missing its argument and a nonexistent
  `-cwd` directory are errors, `--` ends the flags, `-pure-cpp2` does not
  accept `.h2` files, `-output file` with several inputs is rejected, an
  output path that resolves to the input file or that an earlier input
  already writes is an error, an output file in a missing or read-only
  directory is an error, and `-line-paths` names standard input `stdin`.
  Each translation produces its output in one buffer; the CLI writes it to a
  sibling temporary file and renames that over the target, keeping an
  existing file's permissions, and writes in place a target that is not a
  regular file, has other hard links, or is in a directory where the
  temporary cannot be created.

## Representation Boundaries

`translation_options` controls optional lowering behavior:

- `check_division_by_zero` (default `true`, CLI `-no-div-zero-checks`/`-no-d`)
  enables integer division and modulo checks.
- `check_subscripts` (default `true`, CLI `-no-subscript-checks`/`-no-s`)
  enables `CPP2_ASSERT_IN_BOUNDS` on single-index subscripts.
- `check_null_dereference` (default `true`, CLI `-no-null-checks`/`-no-n`)
  enables `cpp2::impl::assert_not_null` on postfix dereferences.
- `exceptions` (default `true`, CLI `-fno-exceptions`/`-fno-e`) emits
  `#define CPP2_NO_EXCEPTIONS Yes` before `"cpp2util.h"` when disabled.
- `rtti` (default `true`, CLI `-fno-rtti`/`-fno-r`) emits
  `#define CPP2_NO_RTTI Yes` before `"cpp2util.h"` when disabled.
- `check_comparisons` (default `true`, CLI `-no-comparison-checks`) lowers
  `<`, `<=`, `>` and `>=`, alone or chained, through `cpp2::impl::cmp_*`.
- `standard_library` (CLI `-include-std`, `-import-std`) defines
  `CPP2_INCLUDE_STD` or `CPP2_IMPORT_STD` instead of inferring standard
  headers; `-include-std` wins.
- `pure_cpp2` (CLI `-pure-cpp2`) rejects Cpp1 lines and implies `-import-std`.
- `line_directives` (default `true`, CLI `-clean-cpp1`) emits `#line`.
- `include_standard_headers` (default `true`) infers standard headers from
  the library names used; a caller that supplies them disables it. The
  `"cpp2util.h"` include does not depend on it.
- `source_path` is the path in `#line` directives (absolute with
  `-line-paths`); `emit_version_info` and `header_guard` add a generator
  comment and an `#ifndef` guard.

Positions are source lines and byte columns; the LSP adapter converts columns
to UTF-16. The lexer emits only non-empty lexemes, the last token of a
section ends at the section's end, and the parser appends an end-of-file token
positioned after the last source line.

Lowering produces pieces of text that each begin on a recorded source line,
and both the `#line` directives and the source map are derived from those
lines. A statement whose source line differs from the line the output would
map to gets a `#line`, including a `next` clause, whose directive goes back.
`translation_result::source_map` has one entry for each generated line with a
source line: the line its `#line` directives imply. `-clean-cpp1` omits the
directives but keeps the map.
A line whose implied line passes the end of its declaration, such as a
generated `@regex` matcher, is unmapped. Within a declaration a mapped line
goes back only for a statement that shares a source line with an earlier one,
and for a loop's `next` clause, which follows the body; a type's out-of-line
members start at their own source lines. A `#line` without the file name is
emitted only where it changes the implied line.
`-source-map file` writes it as
`{"version":1,"source":path,"output":path,"lines":[...]}`, where `lines` has a
source line or `null` for each output line.

Restricted-runtime output must be compiled with matching `-fno-exceptions` and
`-fno-rtti` flags. These options configure the support header; they do not
remove exception or RTTI syntax from user source.

- The AST is syntactic. Expressions are retained as source text where a typed
  expression tree is not yet available.
- The IR has declaration signatures, function statements, stable symbol IDs,
  parent scope IDs, and reference records. It is not fully type checked.
- Semantic transforms consume IR by reference and return new IR or diagnostics.
  Function statement bodies use shared immutable ownership so IR remains
  copyable without duplicating statement trees.
- Emission is position-aware, not byte-for-byte position-preserving. It adds
  `#line` directives and may synthesize forward declarations and includes.
  Output follows upstream's phases: type forward declarations (plus function
  declarations that name no Cpp2 type, concept or function declared later,
  then concepts, then the forward declarations of types a concept constrains),
  then type definitions, Cpp1 lines and
  remaining function declarations (and `extern` declarations of typed
  namespace-scope objects) in source order, then function definitions,
  using-declarations of Cpp2 functions, and the objects' definitions. A
  namespace-scope object, `==` constant, function or type named by the Cpp1
  text of an earlier member of its namespace is declared (a constexpr function
  or a type defined) before that member, with the later constants and constexpr
  functions it names, when nothing it names is declared between them. Namespace-scope objects need a declared type unless
  they are `==` constants, so each can be declared ahead. Member
  functions of types outside functions are declared in their type and defined
  with the function definitions, qualified by their enclosing types' names and
  template heads. As upstream, a type cannot be declared in a function body
  (`local-type`).

## Cpp2 Coverage

The following constructs have focused parser and lowering coverage. Coverage
of the rest of the language is measured by the upstream corpus checks; a
translation test alone does not establish compatibility.

- Mixed Cpp1/Cpp2 files, Cpp1 passthrough, comments, preprocessor/import lines,
  and multiline raw strings. A C++20 module unit keeps `module;`, its module
  declaration, and the imports after it first; the generated includes go in its
  global module fragment, which is added when missing.
- Function, object, type, and basic namespace declarations, including nested
  type/namespace bodies and member declarations.
- Template declarations use one shared model, `declaration_signature` in the
  AST and IR, for template parameters, defaults, variadics, unnamed
  parameters, and `requires` clauses. Function, type, alias, object, and
  concept declarations share the template-prefix lowering.
  Merged `>>` closers retain every nested angle level, including in a template
  parameter kind or default argument.
- Dependent type names use the Cpp2 disambiguator `type T::member` wherever a
  type-id may appear (a template argument, a declared type, `sizeof`, `as`,
  an unnamed object, and a construction `(type T::x)()`), and it lowers to
  `typename T::member`. Ordinary uses of
  `type` as an identifier, such as `std::common_type<T>::type`, are preserved.
  In a type position (a declared, parameter or return type, but not a
  template argument, which may name a value, nor a base), a qualified name
  whose first component is an enclosing template type parameter, or whose
  scope has template arguments that mention one (`std::vector<T>::size_type`),
  or a member of a `decltype(e)` scope, is dependent and lowers with `typename`, and with `template` before a later
  component that has template arguments: `T::rebind<int>` is
  `typename T::template rebind<int>`.
- A function's return is a `return_form` (`return_form_of` in `ir.hpp`):
  implicit `void`, implicit deduction from an expression body
  (`decltype(auto)`), `-> _` (`auto`), `-> forward _` (`decltype(auto)`),
  `-> forward_ref _` (`auto&&`), a declared type, or `-> forward T`.
  Lowering chooses the return type, `void` handling and expression-statement
  bodies from it.
- Concept definitions use `Name: <T> concept = expression;` at file or
  namespace scope and lower to `template<typename T> concept Name = expression;`.
  A type constraint `_ is C<args...>` whose `C` is found in this translation
  unit must name a concept whose first parameter takes a type and that
  accepts one more template argument than is written (`constraint-not-concept`,
  `constraint-not-type-concept`, `constraint-argument-count`). Whether a type
  satisfies a concept is left to the Cpp1 compiler, as upstream.
  A concept requires at least one template parameter and an expression body.
  Type scope and function scope are rejected with
  `concept-declaration-scope`; a brace body is rejected with
  `concept-requires-expression`.
- Names are parsed contextually. Ordinary identifiers and the documented
  `__identifier__` escape can be used in declaration, parameter, template,
  member, local, label, and reference positions. Lowering maps the upstream
  Cpp1 escape set to `cpp2_<name>` so contextual names such as `class` and
  `default` emit valid Cpp1. Structural words such as `if`, `return`, `while`,
  `break`, and `continue` remain statements, and `this`/`that` remain receiver
  parameters rather than ordinary typed parameters.
- Namespace aliases (`N: namespace == X;`), type aliases
  (`N: type == X;`), function aliases, and object aliases. Function and object
  aliases lower to `constexpr`; namespace-scope object aliases lower to
  `inline constexpr`; namespace-scope type aliases preserve template
  parameters. Function-local type aliases lower to `using`; a function-local
  template alias is left to the Cpp1 compiler, which rejects it. Type-scope
  object aliases are `static constexpr` members; one whose type names its enclosing type is
  declared `static const` and defined after all type definitions.
- `using` declarations at file, namespace, type, and block scope. Qualified
  names use `using namespace::name;`; the `using namespace::_;` wildcard lowers
  to `using namespace namespace;`. The Cpp1 `using namespace` spelling is
  rejected with a diagnostic.
- Function parameters: `in`, `in_ref`, `copy`, `inout`, `out`, `move`, and
  `forward`. A non-dependent typed `in T` lowers to `cpp2::impl::in<T>`, a
  dependent `in T` lowers to `T const&`, and deduced `in` lowers to
  `auto const&`; this matches upstream's use of its support type without
  copying its semantic analysis. `in_ref` always lowers to `T const&`.
  A non-dependent typed `out T` lowers to `cpp2::impl::out<T>`; an omitted or
  `_` out type is `auto x_` with the prolog `auto x = cpp2::impl::out(x_);`.
  `inout` lowers to `T&`, `move` to `&&`, and `forward` to `auto&&`. A typed
  `forward` parameter also emits upstream's convertibility constraint.
  Parameter names share the contextual declaration rules; a passing-style word
  is consumed as a style and cannot also be a bare parameter name. A parameter
  named `_` lowers to a deterministic `[[maybe_unused]] unnamed_param_N` and is
  not treated as a declaration name. Trailing commas in parameter and template
  lists are accepted and ignored.
- Deduced parameter and return types are written `_`; an omitted parameter type
  is the same wildcard. Explicit Cpp2 `auto` is rejected with
  `cpp1-auto-type`: use `:=` for deduced objects and `-> _` for deduced
  returns. An `auto` nested inside another type spelling is preserved.
- Explicit and deduced function returns; block bodies and single-expression
  bodies. A single-expression body defaults to `decltype(auto)`. An explicit
  `-> _` lowers to `auto`, `-> forward _` lowers to `decltype(auto)`, and
  `-> forward_ref _` lowers to `auto&&`. A typed `-> forward T` lowers to `T&`,
  or `T const&` for an `in this` receiver. A value-returning single expression
  becomes `return expr;`; an omitted-return or `-> void` expression remains a
  statement.
- Entry points accept `main: ()` or `main: (args)`, where `args` is the sole
  deduced `in` parameter. The latter emits Cpp1 `argc`/`argv` parameters and
  initializes `args` with `cpp2::make_args`. Other entry-point parameter lists
  are rejected with `invalid-main-parameters`. An omitted return lowers to
  `int`; explicit value returns use `return`.
- Type members use Cpp2 access defaults: data objects are private and
  functions/nested types are public unless explicitly marked
  `public`/`protected`/`private`. A `this` receiver accepts only `in`, `inout`,
  `out`, or `move`; other passing styles are diagnosed. Receiver qualifiers
  follow the passing style: `in this` emits `const&`, `inout this` and
  `out this` emit `&`, and `move this` emits `&&`. Assignment operators keep
  the ordinary Cpp1 parameter form without an additional receiver
  ref-qualifier. Explicit `this.member` access lowers to `(*this).member`.
- Local declarations using `:=`, explicit types, and common brace/parenthesis
  initializers (compile/run tested for simple values and container CTAD). The
  canonical `name := value` spelling and the legacy split `name : = value`
  spelling are both accepted.
- An uninitialized typed local lowers to `cpp2::impl::deferred_init<T>`. Its
  first direct assignment of a list constructs from the prvalue
  `std::type_identity_t<T>{elements}` with `.construct_from`, which needs no
  copy or move; of an arithmetic literal, with
  `.construct(std::type_identity_t<T>{value})`; and with `.construct(value)`
  otherwise; later reads use `.value()` and later assignments update `.value()`. Safety
  analysis tracks each declaration by symbol, requires initialization on every
  branch path and in declaration order, follows C++17 sequencing within an
  expression, and rejects initialization inside loops. Reads are found in every expression a statement evaluates, including
  conditions, statement-parameter initializers, interpolations and captures.
  A branch that returns does not join the merge after its `if`.
- Function-local objects may begin with `static`, including typed and deduced
  declarations. The storage class persists across calls and lowers directly to
  a Cpp1 function-local `static`. `static` is rejected on aliases and is not a
  declaration form outside a function body.
- `if`/`else`, `while`, `do`, range/iterator `for`, `break`, `continue`,
  valid labels, and `return`. Range parameters are structured records using
  the ordinary passing styles: deduced `in` lowers to `auto const&`, typed
  `in` uses `cpp2::impl::in<T>`, `inout` to `auto&`, and `copy` by value. A
  range expression `a ..< b` lowers to `cpp2::range(a,b)` and `a ..= b` to
  `cpp2::range(a,b,true)`, including negative bounds. A
  range loop's `next` expression runs after a `continue` but is skipped by
  `break`. When an unlabeled `break` can target the range loop, lowering adds a
  loop-local break flag so the `next` clause is not incorrectly executed.
  Loop conditions and `next` clauses use the same full expression renderer as
  other expressions, so lowering and safety rewrites stay consistent.
  A range `for ... do (e)` body may be one unbraced statement; nested branches,
  loops, blocks, and expressions use the same statement parser and lower with
  explicit braces. Other loop and branch bodies require braces.
  Statement keywords are not reclassified as local declarations; malformed
  loop control emits a targeted diagnostic.
- `try` blocks with one or more `catch` handlers. Catch parameters use the
  ordinary parameter parser: `catch (error: std::exception)` lowers through
  the same passing-style rules, while `catch (_)` lowers to `catch (...)`. A
  named catch parameter requires a type, `catch` must have a block, a `try`
  must have a valid handler, and `throw` requires parentheses around its
  operand.
- Common fixed-width aliases and selected textual lowering for default and
  allocator-qualified `new`, `as`, subscript bounds checks, null-checked postfix pointer
  dereference and address-of, and UFCS calls. Postfix address-of wraps its
  postfix operand, so `value&` emits `&(value)`; binary `&`, `&&`, and `&=`
  are not rewritten. Explicit `move x` lowers to `std::move(x)` while member
  and qualified names containing `move` are left unchanged. Integer `/`,
  `/=`, `%`, and `%=` wrap the denominator in
  `CPP2_ASSERT_NOT_ZERO`; a single decimal literal denominator uses
  `CPP2_ASSERT_NOT_ZERO_LITERAL`. The check is enabled by default and can be
  disabled for a translation with `-no-div-zero-checks` or `-no-d`, or at a
  specific call with `cpp2::unchecked_div` (normally written `unchecked_div`).
  The checked and unchecked divisor call forms compile against
  `runtime/cpp2util.h`.
- Ordinary string interpolation for `(expr)$` and `(expr:spec)$` inside string
  literals. A capture emits `cpp2::to_string(expr)`, a format specification
  emits `cpp2::to_string(expr,"{:spec}")`, and an empty specification emits
  `cpp2::to_string(expr,"{:}")`. Captures may use qualified names and nested
  parentheses; as upstream, a backslash inside an interpolation escapes the
  next character (`(std::string(\"a\"))$`), except at the start of a
  character literal's escape sequence. `$` without a preceding capture is
  literal text. `$R"(...)"` raw strings are interpolated the same way; other raw
  strings are not rewritten. General `$` capture
  syntax outside string interpolation is rejected.
- Whitespace is insignificant at token boundaries, including around qualified
  name separators, except where it changes tokenization (for example
  `- >` is not `->`). Comments, string literals, character literals, and raw
  strings are opaque text; their internal whitespace and punctuation are not
  rewritten.
- Template parameters support type parameters (`T`, `T: type`), non-type
  parameters (`N: i32`, `N: _`), defaults, variadics, unnamed parameters, and
  `requires` clauses.
- Built-in `@value` expansion, as upstream's `@ordered` plus `@basic_value`:
  a defaulted strong `operator<=>`, the memberwise copy/move set, and a public
  explicit default constructor when none is declared. Protected or virtual
  functions and non-public destructors are diagnosed.
- Built-in `@singleton` expansion for types with default-initialized data
  members. It generates a private explicit default constructor, a public
  static `instance()` storing a function-local static, and deleted copy
  members. User-written constructors and uninitialized data members are
  diagnosed.
- Built-in `@copy_constructible` expansion for types. When neither
  `(out this, that)` nor `(out this, move that)` exists, it generates a
  memberwise copy constructor and move constructor. A user-written general
  copy constructor is retained without duplicate generation. A user-written
  move-only constructor is diagnosed as partial copy/move construction.
  User-written value-set constructors lower to their Cpp1 special-member
  signatures and retain their bodies.
- Value-set generalization follows the documented upstream rules. A general
  `(out this, that)` constructor also supplies the move constructor and the
  copy/move assignments when no more-specific member exists. A copy
  constructor supplies a move constructor, and a copy assignment supplies a
  move assignment. A converting constructor supplies an equivalent converting
  assignment unless one is already declared. In generated and explicit move
  bodies, `that` is an ordinary `move` parameter that moves only at its
  definite last use, and generated memberwise defaults move from `that`
  unless the body also reads that part of it. As upstream, a polymorphic type (one with a base or a
  `virtual this` function) gets no generated converting assignment.
- Ordinary types with no user constructor receive a public `T() = default`.
  A type without a `that` constructor receives a deleted copy constructor and,
  when it has no `that` assignment, a deleted copy assignment. This prevents
  Cpp1 from silently generating copy operations that Cpp2 did not opt into.
  User-written `(out this, ...)` conversion constructors suppress only the
  generated default constructor. User-written `(inout this, that)` and
  `(inout this, move that)` assignments lower to the corresponding Cpp1
  assignment signatures and return `*this` consistently from expression and
  block bodies. These defaults apply independently of whether a type also
  opts into a metaclass.
- Initializers on type data members and the supported object declaration
  forms, including default (`()`/`{}`) and parenthesized initializers.
- Empty initializers (`= ;`) are rejected for every local and member type.
  Function-local objects may omit an initializer when assigned before use.
  Namespace-scope objects require an initializer and, unless `==` constants, a
  declared, non-wildcard type. Type-scope objects require a
  declared, non-wildcard type.
- Discard statements (`_ = expr`) lower to `static_cast<void>(expr);`.
- Supported beyond the documentation:
  `if constexpr`, `if consteval`, `static_assert`,
  requires-expressions (with Cpp2 parameters, `requires (inout a: T) { a += a; }`,
  also in a requires clause and an `if constexpr` condition), the `type T::m`
  disambiguator, constexpr
  constructors (`operator=: (out this, x: T) == ...`), variable templates
  (`v: <T> constexpr bool = ...`), C++26 pack indexing (`xs...[I]`) and
  `import std;`. Class template specialization is not supported.
- Standard-header inference for a fixed set of library names.
- Cpp2 input always includes `"cpp2util.h"`, the runtime in `runtime/`
  (upstream's `include/` plus `runtime/upstream.patch` and
  `runtime/experiments.patch`; see
  `runtime/README.md`); standard-header inference is
  independent of that support-header dependency. Restricted-runtime defines
  are emitted immediately before it.

## Known Limitations

- Parser and statement recovery are bounded and diagnostic-oriented; malformed
  input is not guaranteed to recover with a complete AST.
- Name lookup follows Cpp2 scopes but is not a C++ lookup engine: a name finds
  its first declaration in the innermost scope that has one, so overload sets,
  argument-dependent lookup, base-class members and Cpp1 declarations are not
  modeled. Member access `x.m` resolves only through `this` or an object whose
  declared type names a Cpp2 type.
- There is no general type checker, overload resolution, constraint
  satisfaction checking, or complete scope/visibility analysis. Types are
  modeled structurally and dependence on enclosing template type parameters
  is tracked, including through a local or member alias of a dependent type,
  but types are not resolved to declarations, so a member of a dependent base
  is not marked; such names still need `type T::member`. Concept satisfaction is
  left to the Cpp1 compiler.
- Reflection is limited to `@value` and `@basic_value` (copy/move set, default
  constructor, and for `@value` a strong `operator<=>`), `@ordered`,
  `@weakly_ordered`, `@partially_ordered` and their `_value` forms, `@hashable`,
  `@print` (pretty-prints the declaration as upstream), the supported `@singleton`
  shape, `@copy_constructible`, `@struct` (a Cpp1 aggregate, or a memberwise
  constructor when a base has an initializer), `@interface`,
  `@polymorphic_base`, `@enum` and `@flag_enum` (a class with `inline
  constexpr` enumerators), `@sample_traverser` (upstream's test helper,
  without its trace), and `@union` (tagged storage with generated
  accessors), and `@regex` (each `regex` or `regex_*` member becomes a
  `cpp2::regex::regular_expression` with a matcher generated at translation
  time by `src/regex_metafunction.cpp`), and `@autodiff` (forward, Taylor,
  and reverse derivatives of a type's functions and of the functions and types
  they use, by `src/autodiff_metafunction.cpp`). The metafunctions of
  experiments (`docs/EXPERIMENTS.md`) are supported only while enabled. Other
  metaclasses are rejected with `unsupported-metaclass`, once for each name.
  Metaclass arguments, as in `@newtype<std::vector<i32>>`, end at the `>` or
  `>>` closing the list, and an empty one is `empty-metaclass-argument`.
- Implicit and generated special members are limited to the forms documented
  above: ordinary-type defaults, `@value`, `@copy_constructible`, and supported
  value-set generalization. Constructors and assignments set bases and
  members in declaration order from leading `m = value;` statements, else
  from `that`, else from their declared initializers.
- Basic namespace bodies support the same declaration forms as other scopes. Full qualified/out-of-order lookup and complete access semantics
  remain only partially modeled.
- Bases (`this: X = ...`) are public Cpp1 bases. A leading `X = value;`
  statement initializes or assigns a base; otherwise constructors use the
  declared initializer. A data member declared before a base lives in a
  `T_name_as_base` helper base, with a using-declaration carrying its access.
  Templated types cannot declare data members before a base, and a base
  cannot name a member type of its own type.
- `virtual`, `override`, and `final this` lower to Cpp1 virtual functions
  without ref-qualifiers; a bodyless `virtual` or `override` function is pure.
- Unnamed function expressions are extracted before parsing into generated
  function declarations and rendered as lambdas where they are used; `x$`
  inside them, including inside string interpolations, becomes a capture.
  A function expression inside an interpolation `(...)$`, including in a
  `$R` raw string, is tokenized at its own source position and extracted the
  same way; the literal then reads as the generated name. An ordinary-string
  interpolation containing a backslash is not searched for function
  expressions, since its tokens would not match the source text.
- `inspect` is supported as an expression with `is` alternatives and a `-> T`
  result; statement-form `inspect` and `as` alternatives are diagnosed. The
  `patterns` experiment (`docs/EXPERIMENTS.md`) adds destructuring, guards,
  alternatives, bindings and exhaustiveness over `@adt`/`@enum` subjects.
- `pre`/`post` contracts, with optional groups and `$` captures in
  postconditions, lower to `cpp2_default`-style checks and
  `cpp2::finally_presuccess`.
- UFCS: a call `x.f(args)` lowers to `CPP2_UFCS(f)(x, args)` (with
  `_TEMPLATE`, `_MOVE`/`_FORWARD` and `_NONLOCAL` variants, as upstream),
  which calls a member `f` if there is one and otherwise `f(x, args)`. A
  qualified name names a non-member, `x.N::f(args)` is `N::f(x, args)`,
  unless `N::f` is a function with a `this` parameter of a
  type declared here, which makes it the member call
  `x.N::f(args)`. `x..f()`, data members, and a call named after the local
  being initialized, as in `f := y.f()` (upstream's #550 lookup rule, extended
  to typed locals for #863 and #864), stay member accesses. A member call inside `decltype` in a
  type, and calls in metafunction-generated members, are emitted unchanged.
- Whole-file cancellation is the only cancellation mode. Requests are checked
  between files; a translation already in progress is not interrupted.
- Typed `out` parameters use `cpp2::impl::out<T>`. Assignment to an out
  parameter lowers to `.construct(...)`; other uses read through `.value()`,
  and explicit `out x` call arguments lower to `cpp2::impl::out(&x)`.
  Out parameters and named return values must be initialized before every
  `return` and at the end of the body.
- The `cpp2::impl::in<T>` support type supplies upstream's parameter wrapper;
  this project does not independently analyze copy cost. Use `in_ref` when
  reference semantics must be explicit in source.
- Definite last uses follow upstream's rules (`analyze_safety` records them
  per expression text and lowering renders them): the last mention of a
  local, or of a `copy`, `move` or `forward` parameter or statement
  parameter, on each path becomes `cpp2::move(x)` or `CPP2_FORWARD(x)`, and a
  member's last use in a `move this` function becomes `cpp2::move(*this).m`.
  Mentions inside a loop are never last uses for names declared outside it,
  both branches of an `if` are searched, a capture `x$` is a mention where its
  function expression appears, and two mentions separated by a binary
  operator are both left unchanged. Assignment targets, `out`, `inout` and
  `forward` arguments, the operand of a postfix `&`, `++` or `--`, and
  `return x;` of an initialized local are not rewritten. Names beginning with
  `guard` are never moved from. Unlike upstream, a mention in a `catch`
  handler keeps the `try` block's mentions from being last uses. A last-use
  receiver gives `CPP2_UFCS(f)(cpp2::move(x))`, and the name of `x.f()` at the
  last use of a local `f` gives `CPP2_UFCS_MOVE(f)(x)`. Mentions are matched
  to the candidate by scope lookup, so a hiding declaration in an inner scope
  is a different name.
- Name acceptance is intentionally narrower than upstream's parser in one
  respect: a declaration name is accepted only when it has a valid Cpp1
  spelling or belongs to the upstream `cpp2_<name>` escape set. Names such as
  `return` or `namespace`, which upstream can parse but cannot emit as valid
  Cpp1, are rejected with a diagnostic instead of producing invalid output.
- `forward` parameters use `auto&&`; they are forwarded with `CPP2_FORWARD`
  by an explicit `forward x` argument and at their definite last use.
- The emitter is intentionally text-oriented. `is`, `as`, `new`, subscripts
  (bounds-checked unless `-no-subscript-checks`), pointer dereferences
  (null-checked unless `-no-null-checks`), chained comparisons, and UFCS calls
  are selected rewrites, not a complete Cpp2 expression semantics.
- Emitted output aims for upstream's semantics, not its byte-for-byte
  generated-code format.

## Compatibility Policy

The default language follows the Cpp2 documentation. Upstream cppfront is the
reference for behavior the documentation does not cover, and provides test
material and issue examples; it does not define this project's internal
architecture or raw generated-output format. Where the documentation leaves a
rule open, the rule is chosen by the
[design principles](../README.md#design-principles) and stated in
[LANGUAGE.md](LANGUAGE.md). Each compatibility fix starts
with a focused regression test. A departure from upstream is recorded in
`UPSTREAM_AUDIT.md`, or is an opt-in experiment (`EXPERIMENTS.md`).

External Cpp2 code may use older syntax. Before changing behavior to accept
it, check it against current upstream and the grammar in the Cpp2
documentation.

## Verification Layers

- Unit tests cover lexing, validation, parsing, IR construction, transforms,
  safety diagnostics, and focused lowering rules.
- Runtime tests compile and execute generated Cpp1, including upstream
  regression programs, and compare its output and exit status. They run the
  compiler and the program through `fork` and `execvp` with a timeout
  (`tests/unit/test_support.hpp`), not through a shell.
- The whole upstream corpus is a test gate: the scripts below translate,
  compile, and run it, and check its error tests.
- Scripts in `tools/` measure the whole upstream corpus; each defaults to
  `../cppfront` for upstream and `build/cppfrontier` for the binary (override
  with `UPSTREAM` and `BIN`), and writes under `build/corpus/`:
  - `corpus-pure2.sh` translates and compiles every `pure2-*` test.
  - `corpus-mixed.sh` also runs every `mixed-*` test and compares its output
    with upstream's gcc-14 results.
  - `diagnostics-parity.sh` checks that every `*-error` test is rejected with
    an error on each line upstream reports one.
  - `fuzz-truncation.py` translates truncated prefixes of every test and
    reports crashes.
  - `fuzz-mutation.py` translates randomly edited copies of every test
    (deterministic for a given `--seed`) and reports crashes and hangs.
  - `self-host.sh` translates upstream's `source/reflect.h2`,
    `include/cpp2regex.h2` and `include/cpp2taylor.h2`, builds `cppfront.cpp`
    from them with upstream's CI warning flags beside a build of upstream's
    checked-in headers, and requires no warning the reference build lacks,
    identical Cpp1 and messages from both binaries on every regression test,
    and that the self-hosted binary regenerates upstream's three headers.
    `--full` also compiles and runs the tests that use the translated
    `cpp2regex.h` or `cpp2taylor.h` and compares their output with
    upstream's gcc-14 results.
  - `strict-compile.sh` compiles the Cpp1 of every `pure2-*` and `mixed-*`
    test and every port with `-Wall -Wextra -Wpedantic`,
    `-fsanitize=address,undefined` and `-D_GLIBCXX_ASSERTIONS`, runs it, and
    compares its output. A warning is new unless upstream's Cpp1 for the same
    program has the same one; new warnings and failures fail the run unless
    its `KNOWN` list explains them (the user's own text, a compiler bug).
    `--fast` (the `strict-compile` test) checks a subset with `-Werror`;
    `--matrix` repeats the run for g++, clang++-20 and clang++-21, with
    libc++ when it is installed, and `--unit` runs the unit tests with every
    program they compile built with the same flags (`CPPFRONTIER_TEST_CXX`).
  - `diff-fuzz.py` generates random well-defined Cpp2 programs that print
    their results (each reproducible from `--seed N`), translates each with
    cppfrontier and upstream cppfront (`UPSTREAM_BIN`, built from the
    upstream checkout when missing), compiles both with g++ `-std=c++23`
    against `runtime/` and upstream's `include/`, runs them, and compares
    the stage reached and the output. A program that differs is reduced by
    deleting lines and blocks while the difference persists; upstream
    failures on valid programs are listed in its `KNOWN` table. The test
    runs 40 seeds; `--seeds 1:2001` is the long mode.
  - `wild-corpus.py` checks Cpp2 written outside the project: the
    repositories in `wild-corpus.json`, each at a pinned commit, and the code
    blocks of upstream's `docs/`. It fetches only the listed files into
    `build/corpus/wild/src/`, without submodules, hooks or build scripts,
    translates each file with cppfrontier and upstream, compiles both
    translations with `g++ -std=c++20 -fsyntax-only`, and classifies each
    file (both accept, both reject, only one rejects, only one's Cpp1 fails).
    A file that only cppfrontier rejects, or whose Cpp1 fails only for
    cppfrontier, fails the run unless its `KNOWN` list explains it (older
    syntax, a documented difference, an open question); `summary.txt` and
    `details.txt` hold the results. `--repo` narrows the run, and `--reuse`
    keeps earlier checkouts.

  Each exits with a failure status on any regression, and CMake registers
  them as tests with the `corpus` label, except `wild-corpus.py`, which
  downloads; its `--check-manifest` mode is the `wild-corpus-manifest` test.
- When libclang is available, a test-only semantic oracle compares normalized
  Clang AST declarations and statements from generated Cpp1 against upstream
  Cpp1. If `CPPFRONTIER_UPSTREAM_BIN` points to a live upstream compiler, the
  oracle regenerates the reference output; otherwise it uses the checked-in
  expected Cpp1. This checks structure without requiring byte-equal formatting
  and does not add a compiler runtime dependency.
