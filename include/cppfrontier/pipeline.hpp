#pragma once

#include <compare>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <cppfrontier/ast.hpp>
#include <cppfrontier/ir.hpp>
#include <cppfrontier/options.hpp>
#include <cppfrontier/source.hpp>
#include <cppfrontier/token.hpp>

namespace cppfrontier {

class scope_model;

// Pipeline stages, in order. Each is a pure function of its inputs. No stage
// after `apply_reflection` changes the program's declarations or statements,
// so the later stages share one `scope_model` of the reflected program.

// The tokens of lines [begin_line, end_line_exclusive), adding the errors for
// text that is not a token to `diagnostics` when given.
[[nodiscard]] auto tokenize(const source_text& source, std::uint32_t begin_line,
                            std::uint32_t end_line_exclusive,
                            std::vector<diagnostic>* diagnostics = nullptr) -> token_list;

// Source text the parser reads as `text`: an extracted function expression
// reads as its name, and a capture `x$` as the capture's name.
struct source_replacement final {
    source_span span;
    std::string text;
};

struct function_expression_result final {
    token_list tokens;
    std::vector<function_expression> expressions;
    std::vector<source_replacement> replacements;
    // A second capture `$` in one postfix expression, which is removed.
    std::vector<diagnostic> diagnostics;
};

// Replaces each unnamed function expression `:(params) -> R = body` with an
// identifier naming a function declaration appended to the tokens. Names are
// numbered from `first_index`.
[[nodiscard]] auto extract_function_expressions(token_list tokens, std::size_t first_index)
    -> function_expression_result;

// With the `try-operator` experiment, `?` is an operator rather than a Cpp1 conditional's.
[[nodiscard]] auto validate_cpp2(const token_list& tokens, const experiment_set& experiments = {})
    -> std::vector<diagnostic>;

// The `try-operator` experiment: `?` outside a function body statement that can
// return early, in a function whose return type is not `std::expected` or
// `std::optional`, or on the right of `&&` or `||`.
[[nodiscard]] auto check_try_operator(const ir_program& program, const token_list& tokens)
    -> std::vector<diagnostic>;

// The parser recurses once per nested bracket or template argument list, so
// it rejects sources nested deeper than this before parsing.
inline constexpr auto max_nesting_depth = 256;

// A diagnostic at the first token nested deeper than `max_nesting_depth`.
[[nodiscard]] auto validate_nesting_depth(const token_list& tokens) -> std::optional<diagnostic>;

// A `}` in Cpp1 code that closes no `{`.
[[nodiscard]] auto validate_cpp1_braces(const source_text& source) -> std::vector<diagnostic>;

// Unmatched `#else` and `#endif` lines.
[[nodiscard]] auto validate_preprocessor(const source_text& source) -> std::vector<diagnostic>;

struct parse_options final {
    trace_sink trace;
    std::vector<source_replacement> replacements;
    experiment_set experiments;
};

struct parse_result final {
    ast tree;
    std::vector<diagnostic> diagnostics;
    // The `as` of each binding `is T as name` parsed (experiment
    // `condition-bindings`), which validation reports as `mixed-is-as`.
    std::vector<source_span> binding_operators;
};

[[nodiscard]] auto parse_cpp2(const source_text& source, const token_list& tokens,
                              const parse_options& options = {}) -> parse_result;

// Upstream's `@print` rendering of the declaration in `text`
// (`pretty_print_visualize`), or nullopt when it is not understood.
[[nodiscard]] auto pretty_print_declaration(std::string_view text) -> std::optional<std::string>;

struct build_result final {
    ir_program program;
    std::vector<diagnostic> diagnostics;
};

[[nodiscard]] auto build_ir(const ast& tree, ir_program program = {}) -> build_result;

struct reflection_result final {
    ir_program program;
    std::vector<diagnostic> diagnostics;
};

[[nodiscard]] auto apply_reflection(ir_program program, const experiment_set& experiments = {})
    -> reflection_result;

// The symbol each identifier in `tokens` names, and each `p*` dereferences.
[[nodiscard]] auto resolve_symbols(const ir_program& program, const scope_model& scopes,
                                   const token_list& tokens) -> std::vector<symbol_reference>;

// A text that the safety analysis annotates, named by its part of a statement
// or declaration, so that it survives copies of the IR.
struct text_key final {
    enum class part : std::uint8_t {
        text,         // an expression statement's or declaration statement's text
        initializer,  // a declaration statement's initializer
        condition,
        next,
        message,      // a contract statement's message
        flag,         // contract statement flag `index`
        group,        // a contract statement's group
        parameter,    // the initializer of statement parameter `index`
        capture,      // capture `index` of a function expression
        body,         // a function's single-expression body
    };
    // A statement id; the function expression's or function's symbol for a
    // capture or a body.
    std::uint64_t node = 0;
    part which = part::text;
    std::uint32_t index = 0;

    friend auto operator<=>(const text_key&, const text_key&) = default;
};

[[nodiscard]] inline auto statement_key(const statement& value,
                                        text_key::part which = text_key::part::text,
                                        std::size_t index = 0) -> text_key {
    return {value.id, which, static_cast<std::uint32_t>(index)};
}

using last_use_map = std::map<text_key, std::vector<last_use>>;

// Deferred locals constructed by each statement: the first assignment on the
// path. Produced by the safety analysis and consumed by lowering, with the
// definite last uses that lowering moves or forwards.
struct initialization_plan final {
    std::map<statement_id, std::vector<construction>> constructions;
    last_use_map last_uses;
    // For each function with a `move that` parameter, the last uses in each text
    // of its body that has a last use of `that`, with that text's other last
    // uses. Kept per function: a generated move function shares its body with
    // the `in that` function it comes from.
    std::map<symbol_id, last_use_map> that_last_uses;
    // The returned texts, of a `-> forward _` function or an expression body,
    // that refer into a local or a `copy` or `move` parameter: their type,
    // which the return type deduces, must not be a reference.
    std::set<text_key> value_returns;

    [[nodiscard]] auto constructions_in(statement_id value) const
        -> const std::vector<construction>*;
    [[nodiscard]] auto last_uses_in(const text_key& text) const -> const std::vector<last_use>*;
    [[nodiscard]] auto that_last_uses_in(symbol_id function) const -> const last_use_map*;
};

struct safety_result final {
    std::vector<diagnostic> diagnostics;
    initialization_plan plan;
};

// `check_comparisons` rejects mixed-sign `<`, `<=`, `>` and `>=` whose operand
// types are known; `experiments` enables the `use-after-move` analysis.
[[nodiscard]] auto analyze_safety(const ir_program& program, const scope_model& scopes,
                                  bool check_comparisons, const experiment_set& experiments)
    -> safety_result;
// As above, with the scope model of `program`.
[[nodiscard]] auto analyze_safety(const ir_program& program, bool check_comparisons = true,
                                  const experiment_set& experiments = {}) -> safety_result;

// Type constraints `_ is C<...>` whose concept `C` is declared in this
// translation unit and cannot accept them, or that name a non-concept.
// Whether a type satisfies a concept is left to the Cpp1 compiler.
[[nodiscard]] auto check_constraints(const ir_program& program, const scope_model& scopes)
    -> std::vector<diagnostic>;

// A forwarding return that refers into an object the function owns: the
// `forward-return-reference` error, and the message of the Cpp1
// `static_assert` on a deduced return (`value_returns`).
inline constexpr auto forward_return_message = std::string_view{
    "a 'forward' return cannot return a reference into a local variable or an 'in', "
    "'copy', or 'move' parameter"};

// The `invalid-inspect` error `validate_cpp2` reports at an inspect that has
// no alternative matching anything.
inline constexpr auto inspect_wildcard_message = std::string_view{
    "an inspect expression must have an `is _` match-anything wildcard alternative"};

// With the `patterns` experiment: removes the `inspect_wildcard_message`
// error of an inspect that covers every case of its subject's `@adt` or
// `@enum` type (or, with `sealed`, the derived types of a `@sealed` type),
// and replaces it with `inspect-not-exhaustive` for one that does not.
auto check_inspect_patterns(const ir_program& program, const scope_model& scopes,
                            const token_list& tokens, const experiment_set& experiments,
                            std::vector<diagnostic>& diagnostics) -> void;

// The types whose base names the `@sealed` type `base` and that are declared
// in its scope; those declared elsewhere are added to `elsewhere` when given.
[[nodiscard]] auto sealed_derived_types(const ir_program& program, const ir_declaration& base,
                                        std::vector<const ir_declaration*>* elsewhere = nullptr)
    -> std::vector<const ir_declaration*>;

// Null assignment, arithmetic and bitwise operations on names known to be
// pointers. Reads the program's resolved `references`.
[[nodiscard]] auto check_pointer_safety(const ir_program& program, const token_list& tokens)
    -> std::vector<diagnostic>;

// Experiment `exclusivity`: an object passed `inout` or `out` to a function
// declared in this translation unit while another argument of the same call
// mentions it or a part of it.
[[nodiscard]] auto check_exclusivity(const ir_program& program, const scope_model& scopes,
                                     const token_list& tokens) -> std::vector<diagnostic>;

struct emit_result final {
    std::string cpp1;
    std::vector<std::pair<source_position, source_position>> source_map;
};

[[nodiscard]] auto emit_cpp1(const ir_program& program, const scope_model& scopes,
                             const source_text& source, const token_list& tokens,
                             const translation_options& options,
                             const initialization_plan& plan) -> emit_result;
// As above, with the scope model of `program`.
[[nodiscard]] auto emit_cpp1(const ir_program& program, const source_text& source,
                             const token_list& tokens, const translation_options& options,
                             const initialization_plan& plan = {}) -> emit_result;

}
