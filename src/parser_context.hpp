#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cppfrontier/ast.hpp>
#include <cppfrontier/diagnostic.hpp>
#include <cppfrontier/options.hpp>
#include <cppfrontier/pipeline.hpp>
#include <cppfrontier/source.hpp>
#include <cppfrontier/token.hpp>

#include "brackets.hpp"

namespace cppfrontier::parser_detail {

enum class using_parse_result : std::uint8_t { not_using, valid, invalid };

using detail::angle_close;
using detail::bracket_close;
using detail::bracket_open;

// How a `<` is read: in an expression, as a template argument list after a
// name when the tokens read as one; in a type, always as a list; in a
// declaration, as in a type up to a top-level `=`, then as in an expression.
// A `<` after `:` always begins a template parameter list.
enum class angle_rule : std::uint8_t { expression, type, declaration };

// The top-level `=` or `:=` in [begin, end) followed by nothing but `;`s, if any.
[[nodiscard]] auto empty_initializer(const token_list& tokens, std::size_t begin,
                                     std::size_t end) -> std::optional<std::size_t>;

// The first token of the postfix expression ending just before the capture
// `$` at `position`, or nullopt.
[[nodiscard]] auto capture_begin(const token_list& tokens, std::size_t begin,
                                 std::size_t position) -> std::optional<std::size_t>;

// `text` with each capture `x$` inside a string interpolation `(...)$`
// replaced by the name `capture` gives it.
[[nodiscard]] auto replace_interpolated_captures(
    std::string text, const std::function<std::string(const std::string&)>& capture)
    -> std::string;

// The `>` closing the template argument list opened at `open`, when the tokens
// read as one.
[[nodiscard]] auto template_argument_close(const token_list& tokens, std::size_t open)
    -> std::optional<std::size_t>;

// Whether the `type` at `position` marks the dependent type-id after it,
// `type T::x`, rather than beginning a type declaration or alias.
[[nodiscard]] auto is_type_marker(const token_list& tokens, std::size_t position) -> bool;

// Whether the `{` at `open` begins the requirements of a requires-expression,
// `requires { ... }` or `requires (parameters) { ... }`.
[[nodiscard]] auto is_requires_body(const token_list& tokens, std::size_t open) -> bool;

// `template_argument_close` for every `<` in `tokens`, indexed by position.
[[nodiscard]] auto template_argument_closes(const token_list& tokens)
    -> std::vector<std::optional<std::size_t>>;

// The `>` in [open, end) closing the template list that the `<` at `open`
// begins, read as in a type when `type`, or nullopt when it begins none.
[[nodiscard]] auto template_list_close(const token_list& tokens, std::size_t open,
                                       std::size_t end, bool type)
    -> std::optional<std::size_t>;

// The first position in [begin, end) outside brackets and template lists at
// which `stop(position)` holds or a closer leaves the range, else `end`; see
// `detail::find_top_level`.
template<class Stop, class Unclosed = detail::ignore_unclosed>
auto find_top_level(const token_list& tokens, std::size_t begin, std::size_t end,
                    angle_rule rule, Stop stop, Unclosed unclosed = {}) -> std::size_t {
    auto type = rule != angle_rule::expression;
    return detail::find_top_level(tokens, begin, end,
        [&](std::size_t open) { return template_list_close(tokens, open, tokens.size(), type); },
        [&](std::size_t position) {
            if (rule == angle_rule::declaration && tokens[position].text == "=") type = false;
            return stop(position);
        },
        unclosed);
}

// The `)` of the name list `(a, b)` at `begin` when `:=` or `:` follows it:
// the start of a destructuring declaration. Each name is a declaration name
// or `_`.
[[nodiscard]] auto destructuring_names_end(const token_list& tokens, std::size_t begin)
    -> std::optional<std::size_t>;

// The comma-separated ranges of [begin, end) outside brackets and template
// lists.
[[nodiscard]] auto top_level_items(const token_list& tokens, std::size_t begin, std::size_t end,
                                   angle_rule rule = angle_rule::expression)
    -> std::vector<std::pair<std::size_t, std::size_t>>;

[[nodiscard]] auto is_type_start(const token_list& tokens, std::size_t position) -> bool;

// Where the type-id starting at `begin` ends. With `type_operators`
// (experiment `type-operators`) a parenthesized operand may follow `|` or `&`.
[[nodiscard]] auto type_end(const token_list& tokens, std::size_t begin, std::size_t end,
                            bool type_operators = false) -> std::size_t;

// Where the type-id in [begin, end) ends by the type grammar: prefixes `*`
// and `const`, a name with template lists and `::` members, a function type,
// `decltype(...)`, `_ is C` or a fundamental type, then an optional `...`;
// a `>>` at `end` may close a list. With `type_operators` an operand may
// follow `|` or `&`.
[[nodiscard]] auto type_grammar_end(const token_list& tokens, std::size_t begin, std::size_t end,
                                    bool type_operators = false) -> std::size_t;

class token_cursor final {
public:
    token_cursor(const source_text& source, const token_list& tokens,
                 std::vector<diagnostic>& diagnostics, trace_sink trace,
                 const std::vector<source_replacement>* replacements = nullptr,
                 const experiment_set* experiments = nullptr);
    [[nodiscard]] auto experiment_enabled(experiment_id id) const noexcept -> bool;
    [[nodiscard]] auto token_at(std::size_t position) const -> const token&;
    // The expression `text`, parsed with the enabled experiments; `begin` is
    // where it begins when it is taken from the source as written.
    [[nodiscard]] auto expression(std::string text,
                                  std::optional<source_position> begin = std::nullopt) const
        -> parsed_expression {
        return parsed_expression{std::move(text), experiment_enabled("try-operator"),
                                 experiment_enabled("comprehension"),
                                 experiment_enabled("condition-bindings"), begin};
    }
    [[nodiscard]] auto token_count() const noexcept -> std::size_t;
    [[nodiscard]] auto trace_enabled() const noexcept -> bool;
    auto trace_event(std::string_view message) const -> void;
    [[nodiscard]] auto source_at(source_position begin, source_position end) const -> std::string;
    // The source text of `written`, as a diagnostic quotes it: a capture `x$`
    // that a function expression reads as `_0` is quoted as written.
    [[nodiscard]] auto quoted(const token& written) const -> std::string;
    [[nodiscard]] auto syntax_text(std::size_t begin, std::size_t end) const -> std::string;
    // Whether source text between the tokens of [begin, end], other than
    // whitespace and comments, is not a token, which the lexer has reported.
    [[nodiscard]] auto has_unlexed_text(std::size_t begin, std::size_t end) const -> bool;
    // The type-id written in tokens [begin, end); reports a misused type operator.
    [[nodiscard]] auto syntax_type(std::size_t begin, std::size_t end) -> type_id;
    // Reports the first token of [begin, end) after the type-id it begins.
    auto check_type_tokens(std::size_t begin, std::size_t end) -> void;
    // The options for `parse_type` under the enabled experiments.
    [[nodiscard]] auto type_options() const noexcept -> type_parse_options;
    // Reports an operand missing from a type operator anywhere in `type`, and
    // `&` where a type rather than a constraint is expected.
    auto check_type_operators(const type_id& type, bool constraint) -> void;
    // Reports the parts of the written type `type` that Cpp2 types do not
    // have: a `_` where Cpp1 has no `auto` (unless `placeholder`), a Cpp1
    // reference `T&` or `T&&`, and a function type with named returns.
    auto check_type_parts(const type_id& type, bool placeholder, bool top) -> void;
    // Reports the expression problems of the value template arguments in
    // `type`, written in tokens [begin, end).
    auto check_type_arguments(const type_id& type, std::size_t begin, std::size_t end) -> void;
    // Reports a misused type operator in the type alias target in tokens [begin, end).
    auto check_alias_type(std::size_t begin, std::size_t end) -> void;
    [[nodiscard]] auto parameter_passing_style(std::string_view text) const
        -> std::optional<passing_style>;
    [[nodiscard]] auto parse_parameter_range(std::size_t begin, std::size_t end,
                                             parameter& output) -> bool;
    auto template_parameters(std::size_t& index,
                             std::vector<template_parameter>& output) -> bool;
    // Reports Cpp1 `auto` as the type of `name`, which Cpp2 writes as `replacement`.
    auto check_cpp1_auto(const type_id& type, std::string_view name,
                         std::string_view replacement) -> void;
    // An `in` parameter is always const, so its type cannot be `const`.
    auto check_in_parameter_type(const parameter& value) -> void;
    // Reports an object named `_` whose type is deduced, as `_ := f();`.
    auto check_anonymous_object(std::string_view name, const type_id& type,
                                source_span name_span) -> void;
    // Reports, at `name`, an object initializer that is a block, the `{` at
    // `open` and braces holding a `;`, as in `x: int = { return 1; }`; returns
    // the index of its `}`. A braced list such as `{1, 2}` holds no `;`.
    auto diagnose_block_initializer(std::size_t open, source_span name)
        -> std::optional<std::size_t>;
    // Reports a `,` in [begin, end) outside brackets and template argument
    // lists; returns whether there was one.
    auto diagnose_top_level_comma(std::size_t begin, std::size_t end) -> bool;
    // Reports the first problem in the expression `value` at its token in
    // [begin, end): an operand that directly follows another, as in `x y`, a
    // token that is part of no expression, or an assignment used as a value;
    // returns whether there was one. A `statement` may itself be an assignment,
    // and a `list` value may itself be a list `(a, b)`. With `bindings`,
    // `value` is an `if` or `while` condition, and receives the names that the
    // pattern tests among its top-level `&&` operands bind (experiment
    // `condition-bindings`); every other pattern test is reported.
    auto diagnose_juxtaposition(const parsed_expression& value, std::size_t begin,
                                std::size_t end, bool statement = false, bool list = false,
                                std::vector<statement::binding>* bindings = nullptr) -> bool;
    // The `as` of each `as name` binding parsed, whose `mixed-is-as` does not apply.
    [[nodiscard]] auto binding_operators() const noexcept -> const std::vector<source_span>& {
        return binding_operators_;
    }
    // The position in [begin, end) of the tree's token `index`, the expression
    // text of those tokens.
    [[nodiscard]] auto tree_token_position(const expression_text& tree, std::size_t index,
                                           std::size_t begin, std::size_t end) const
        -> std::optional<std::size_t>;
    // The first token in [begin, end) after the expression that begins at `begin`.
    [[nodiscard]] auto expression_end(std::size_t begin, std::size_t end) const -> std::size_t;
    [[nodiscard]] auto using_declaration(std::size_t& index, std::string& target) const
        -> using_parse_result;
    // Reports the invalid using declaration whose `using` is at `index`.
    auto diagnose_invalid_using(std::size_t index) -> void;
    // Parses the `<group, flags...>(condition, message)` after the contract
    // keyword at `index`, and moves `index` past it. `captures`, given for a
    // postcondition, receives each `x$` of the condition, which then reads its name.
    auto contract(std::size_t& index, contract_clause& output,
                  std::vector<function_contract::capture>* captures = nullptr) -> bool;
    [[nodiscard]] auto unsupported_expression(std::size_t begin, std::size_t end) const
        -> std::optional<source_span>;
    [[nodiscard]] auto diagnostic_count() const noexcept -> std::size_t {
        return diagnostics_->size();
    }
    auto add_diagnostic(std::string_view code, diagnostic_severity severity,
                        source_span span, std::string_view message) -> void;

private:
    const source_text* source_;
    const token_list* tokens_;
    std::vector<diagnostic>* diagnostics_;
    trace_sink trace_;
    const std::vector<source_replacement>* replacements_;
    const experiment_set* experiments_;
    std::vector<source_span> binding_operators_;

    auto check_pattern_tests(const parsed_expression& value, std::size_t begin, std::size_t end,
                             std::vector<statement::binding>* bindings) -> void;
    auto check_typed_values(const parsed_expression& value, std::size_t begin, std::size_t end)
        -> void;
};

struct statement_parser final {
    token_cursor& context;
    const token_list& tokens;

    auto parse_block(std::vector<std::unique_ptr<statement>>& statements, std::size_t& index,
                     const token& terminating_token, std::size_t depth = 0,
                     bool single_statement = false) -> void;
};

class trace final {
public:
    explicit trace(std::string_view name, trace_sink sink);
    [[nodiscard]] auto enabled() const noexcept -> bool;
    auto event(std::string_view message) const -> void;

private:
    bool enabled_;
};

}
