#include "parser_context.hpp"
#include "cpp1_text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <utility>

namespace cppfrontier::parser_detail {

namespace {

auto is_operand_token(const token& value) -> bool {
    return value.kind == token_kind::identifier ||
        value.kind == token_kind::keyword ||
        value.kind == token_kind::integer_literal ||
        value.kind == token_kind::string_literal ||
        value.kind == token_kind::character_literal;
}

auto is_single_operand_expression(const token_list& tokens, std::size_t begin, std::size_t end)
    -> bool {
    if (end > begin && tokens[end - 1].text == ";") --end;
    if (begin >= end) return false;
    auto position = begin;
    if (tokens[position].text == "(") {
        const auto close = bracket_close(tokens, position, end);
        if (!close || *close + 1 != end) return false;
        ++position;
        if (position >= *close) return false;
        if (!is_operand_token(tokens[position])) return false;
        ++position;
        return position == *close;
    }
    if (!is_operand_token(tokens[position])) return false;
    // A function expression statement declares an unused lambda, as upstream allows.
    if (is_function_expression_name(tokens[position].text)) return false;
    ++position;
    if (position == end) return true;
    if (tokens[position].kind != token_kind::punctuation) return false;
    if (tokens[position].text == "::") return true;
    if (tokens[position].text == "." || tokens[position].text == "->") {
        ++position;
        return position + 1 == end && is_operand_token(tokens[position]);
    }
    return false;
}

// The operand or `do` that directly follows the postfix expression that
// begins [begin, end), which no statement has.
auto malformed_statement_operand(const token_list& tokens, std::size_t begin,
                                 std::size_t end) -> std::optional<std::size_t> {
    auto position = begin;
    if (position < end && tokens[position].text == "static" &&
        position + 1 < end && is_declaration_name(tokens[position + 1])) {
        ++position;
    }
    if (position >= end || !is_operand_token(tokens[position])) return std::nullopt;
    if (tokens[position].text == "return") return std::nullopt;
    ++position;
    while (position < end) {
        if (tokens[position].text == "." || tokens[position].text == "->" ||
            tokens[position].text == "::") {
            ++position;
            if (position >= end || !is_operand_token(tokens[position])) return std::nullopt;
            ++position;
            continue;
        }
        if (tokens[position].text == "(" || tokens[position].text == "[") {
            const auto close = bracket_close(tokens, position, end);
            if (!close) return std::nullopt;
            position = *close + 1;
            continue;
        }
        break;
    }
    if (position >= end) return std::nullopt;
    const auto& next = tokens[position];
    if (is_operand_token(next) || next.text == "do") return position;
    return std::nullopt;
}

struct local_declaration_prefix final {
    std::size_t name = 0;
    bool is_static = false;
};

auto local_declaration_prefix_at(const token_list& tokens, std::size_t begin,
                                 std::size_t end) -> local_declaration_prefix {
    auto result = local_declaration_prefix{begin, false};
    if (begin < end && tokens[begin].text == "static" &&
        begin + 1 < end && is_declaration_name(tokens[begin + 1])) {
        result.name = begin + 1;
        result.is_static = true;
    }
    return result;
}

auto source_text_between(const token_list& tokens, std::size_t begin, std::size_t end,
                         const token_cursor& context) -> std::string {
    if (begin >= end || begin >= tokens.size()) return {};
    end = std::min(end, tokens.size());
    return context.source_at(tokens[begin].span.begin, tokens[end - 1].span.end);
}

// The source of tokens `begin` to `end` without a final `;`, so a comment
// before the `;` is not part of the text.
auto statement_text_between(const token_list& tokens, std::size_t begin, std::size_t end,
                            const token_cursor& context) -> std::string {
    end = std::min(end, tokens.size());
    while (end > begin && tokens[end - 1].text == ";") --end;
    return source_text_between(tokens, begin, end, context);
}

struct local_alias_prefix final {
    std::size_t type_begin = 0;
    std::size_t type_end = 0;
    std::size_t value_begin = 0;
    std::vector<template_parameter> template_parameters;
    bool is_type_alias = false;
    bool is_namespace_alias = false;
    bool is_concept = false;
};

auto parse_local_alias_prefix(token_cursor& context, const token_list& tokens,
                              std::size_t name, std::size_t end)
    -> std::optional<local_alias_prefix> {
    auto position = name + 2;
    auto result = local_alias_prefix{};
    if (position < end && tokens[position].text == "<") {
        if (!context.template_parameters(position, result.template_parameters)) return std::nullopt;
    }
    if (position < end && tokens[position].kind == token_kind::keyword &&
        tokens[position].text == "concept") {
        result.is_concept = true;
        return result;
    }
    if (position < end && tokens[position].kind == token_kind::keyword &&
        tokens[position].text == "type" && !is_type_marker(tokens, position)) {
        result.is_type_alias = true;
        ++position;
    }
    else if (position < end && tokens[position].kind == token_kind::keyword &&
             tokens[position].text == "namespace") {
        result.is_namespace_alias = true;
        ++position;
    }
    else if (position < end && tokens[position].text != "requires" &&
             tokens[position].text != "==") {
        result.type_begin = position;
        result.type_end = type_end(tokens, position, end,
                                   context.experiment_enabled("type-operators"));
        if (result.type_end <= result.type_begin || result.type_end > end) return std::nullopt;
        position = result.type_end;
    }
    if (position < end && tokens[position].kind == token_kind::keyword &&
        tokens[position].text == "requires") {
        position = find_top_level(tokens, position + 1, end, angle_rule::expression,
            [&](std::size_t at) { return tokens[at].text == "=="; });
    }
    if (position >= end || tokens[position].text != "==") return std::nullopt;
    result.value_begin = position + 1;
    return result.value_begin <= end ? std::optional{result} : std::nullopt;
}

enum class step : std::uint8_t { next, stop, unhandled };

auto is_builtin_type_name(std::string_view name) -> bool {
    static constexpr auto names = std::array<std::string_view, 23>{
        "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "uint", "bool", "char",
        "char8_t", "char16_t", "char32_t", "wchar_t", "short", "int", "long", "float",
        "double", "signed", "unsigned", "void"};
    return std::find(names.begin(), names.end(), name) != names.end();
}

// Whether a `break` or `continue` in `statements` names `label`.
auto uses_label(const statement_list& statements, const std::string& label) -> bool {
    return std::any_of(statements.begin(), statements.end(), [&](const auto& current) {
        if (const auto* jump = std::get_if<statement::jump_form>(&current->form);
            jump && jump->label == label) {
            return true;
        }
        auto found = false;
        for_each_nested_list(*current, [&](const auto& nested) {
            found = found || uses_label(nested, label);
        });
        return found;
    });
}

// The first `return`, or `break`/`continue` targeting a loop outside the
// deferred `statements`, which a defer body cannot run; `labels` names the
// loops entered inside the body.
auto escaping_jump(const statement_list& statements, std::vector<std::string> labels)
    -> const statement* {
    for (const auto& current : statements) {
        if (current->kind == statement_kind::return_statement) return current.get();
        if (const auto* jump = std::get_if<statement::jump_form>(&current->form)) {
            const auto inside = jump->label.empty()
                ? !labels.empty()
                : std::find(labels.begin(), labels.end(), jump->label) != labels.end();
            if (!inside) return current.get();
        }
        auto nested_labels = labels;
        if (const auto* loop = std::get_if<statement::loop_form>(&current->form)) {
            nested_labels.push_back(loop->label);
        }
        const statement* found = nullptr;
        for_each_nested_list(*current, [&](const statement_list& nested) {
            if (found == nullptr) found = escaping_jump(nested, nested_labels);
        });
        if (found != nullptr) return found;
    }
    return nullptr;
}

// Reports each `break` or `continue` in `statements` naming no enclosing
// loop; `labels` names the enclosing loops, innermost last, an unlabeled one
// as "".
auto check_jumps(token_cursor& context, const statement_list& statements,
                 std::vector<std::string>& labels) -> void {
    for (const auto& current : statements) {
        if (const auto* jump = std::get_if<statement::jump_form>(&current->form)) {
            const auto keyword = current->kind == statement_kind::break_statement
                ? std::string{"break"} : std::string{"continue"};
            if (!jump->label.empty() &&
                std::find(labels.begin(), labels.end(), jump->label) == labels.end()) {
                context.add_diagnostic("unknown-loop-label", diagnostic_severity::error,
                                       current->span,
                                       "a named " + keyword + " must use the name of an "
                                       "enclosing local loop label");
            }
        }
        const auto* loop = std::get_if<statement::loop_form>(&current->form);
        if (loop != nullptr) labels.push_back(loop->label);
        for_each_nested_list(*current, [&](const statement_list& nested) {
            check_jumps(context, nested, labels);
        });
        if (loop != nullptr) labels.pop_back();
    }
}

// Parses the statements of one block. Each statement form has its own member;
// `statement_begin` marks the start of the expression statement being scanned.
struct block_parser final {
    token_cursor& context;
    const token_list& tokens;
    std::vector<std::unique_ptr<statement>>& statements;
    std::size_t& index;
    const token& terminating_token;
    std::size_t depth;
    std::size_t statement_begin = index;
    bool saw_expression_start = false;
    std::string current_label;

    auto parse_block(std::vector<std::unique_ptr<statement>>& body, std::size_t& position,
                     const token& terminator, std::size_t nested_depth,
                     bool single_statement = false) -> void {
        statement_parser{context, tokens}.parse_block(
            body, position, terminator, nested_depth, single_statement);
    }

    auto is_terminator(const token& value) const -> bool {
        return value.text == terminating_token.text &&
            value.span.begin.line == terminating_token.span.begin.line &&
            value.span.begin.column == terminating_token.span.begin.column;
    }

    // Advances `index` to the first token at which `stop` holds outside
    // brackets, `requires` expressions and `inspect` expressions, or to the
    // last token.
    template<class Stop>
    auto skip_condition(Stop stop) -> void {
        const auto last = tokens.size() - 1;
        while (index < last && !stop(index)) {
            const auto& text = context.token_at(index).text;
            auto close = std::optional<std::size_t>{};
            if (text == "(" || text == "[") close = bracket_close(tokens, index, last);
            else if (text == "inspect") {
                // The alternatives follow the subject and `-> T` in braces.
                auto body = index + 1;
                while (body < last && context.token_at(body).text != "{" &&
                       context.token_at(body).text != ";" && context.token_at(body).text != "}") {
                    const auto& part = context.token_at(body).text;
                    const auto inner = part == "(" || part == "["
                        ? bracket_close(tokens, body, last) : std::nullopt;
                    body = inner ? *inner + 1 : body + 1;
                }
                if (context.token_at(body).text == "{") close = bracket_close(tokens, body, last);
            }
            else if (text == "requires") {
                auto body = index + 1;
                if (context.token_at(body).text == "(") {
                    if (const auto parameters = bracket_close(tokens, body, last)) {
                        body = *parameters + 1;
                    }
                }
                if (context.token_at(body).text == "{") close = bracket_close(tokens, body, last);
            }
            index = close ? *close + 1 : index + 1;
        }
    }

    auto parse_statement(const token& current) -> step;
    auto flush() -> void;
    auto check_condition(std::size_t begin, std::size_t end) -> void;
    auto diagnose_empty_next(std::size_t keyword) -> void {
        context.add_diagnostic("missing-next-expression", diagnostic_severity::error,
                               context.token_at(keyword).span, "expected an expression after 'next'");
    }
    // A `next` clause in tokens [begin, end) is one expression; with experiment
    // `next-list` it may be a list `(a, b)`, each item checked as one.
    auto check_next(const parsed_expression& next, std::size_t begin, std::size_t end) -> void {
        if (whole_list(next.tree()) == nullptr) {
            context.diagnose_juxtaposition(next, begin, end, true);
            return;
        }
        if (!context.experiment_enabled("next-list")) {
            context.add_diagnostic("next-clause-list", diagnostic_severity::error,
                                   context.token_at(begin).span,
                                   "a 'next' clause is one expression, not a list '(a, b)' - "
                                   "'-experiment=next-list' runs each item in order");
            return;
        }
        for (const auto& [first, last] : top_level_items(tokens, begin + 1, end - 1)) {
            const auto item = context.expression(
                context.source_at(context.token_at(first).span.begin,
                                  context.token_at(last - 1).span.end),
                context.token_at(first).span.begin);
            context.diagnose_juxtaposition(item, first, last, true);
        }
    }
    auto parse_using(const token& current) -> step;
    auto parse_loop_or_branch(const token& current, bool is_labeled_loop) -> step;
    auto parse_else(const token& current) -> step;
    auto parse_jump(const token& current) -> step;
    auto expect_semicolon(std::string_view what) -> void;
    auto missing_body_braces(std::string_view what, std::size_t condition_begin) -> step;
    auto parse_return(const token& current) -> step;
    auto parse_try(const token& current) -> step;
    auto parse_compound(const token& current,
                        statement_kind kind = statement_kind::block) -> step;
    auto parse_defer(const token& current) -> step;
    auto parse_unsafe(const token& current) -> step;
    auto parse_statement_parameters(const token& current) -> step;
    auto parse_destructuring(const token& current) -> step;
    auto parse_let_else(const token& current) -> step;
    auto parse_contract(const token& current) -> step;
    auto parse_expression_statement(const token& current) -> step;
};

auto block_parser::parse_statement(const token& current) -> step {
    if (is_terminator(current)) return step::stop;
    if (current.text == "}") {
        ++index;
        return step::stop;
    }
    if (current.kind == token_kind::keyword && current.text == "using") {
        if (const auto result = parse_using(current); result != step::unhandled) return result;
    }

    const auto is_loop_or_branch = current.kind == token_kind::keyword &&
        (current.text == "while" || current.text == "do" || current.text == "for" || current.text == "if");
    const auto is_labeled_loop = is_declaration_name(current) &&
        context.token_at(index + 1).text == ":" &&
        context.token_at(index + 2).kind == token_kind::keyword &&
        (context.token_at(index + 2).text == "while" ||
         context.token_at(index + 2).text == "for" ||
         context.token_at(index + 2).text == "do");
    if (is_loop_or_branch || is_labeled_loop) return parse_loop_or_branch(current, is_labeled_loop);
    if (current.kind == token_kind::keyword && current.text == "else") return parse_else(current);
    if (current.text == "break" || current.text == "continue") return parse_jump(current);
    if (current.text == ";") {
        if (statement_begin == index && !saw_expression_start) {
            context.add_diagnostic("empty-statement", diagnostic_severity::error, current.span,
                                   "empty statement is not allowed - remove extra semicolon");
        }
        ++index;
        flush();
        return step::next;
    }
    if (current.text == "return") return parse_return(current);
    if (current.kind == token_kind::keyword && current.text == "try") return parse_try(current);
    if (current.text == "{") return parse_compound(current);
    if (current.text == "defer" && current.kind == token_kind::identifier &&
        statement_begin == index && !saw_expression_start &&
        context.experiment_enabled("defer")) {
        return parse_defer(current);
    }
    if (current.text == "unsafe" && current.kind == token_kind::identifier &&
        statement_begin == index && !saw_expression_start &&
        context.token_at(index + 1).text == "{" && context.experiment_enabled("unsafe")) {
        return parse_unsafe(current);
    }
    if (current.text == "assert" && current.kind == token_kind::identifier &&
        statement_begin == index && !saw_expression_start &&
        (context.token_at(index + 1).text == "(" || context.token_at(index + 1).text == "<")) {
        return parse_contract(current);
    }
    if (statement_begin == index && !saw_expression_start &&
        context.experiment_enabled("let-else")) {
        if (const auto result = parse_let_else(current); result != step::unhandled) return result;
    }
    if (current.text == "(" && statement_begin == index && !saw_expression_start) {
        if (const auto result = parse_statement_parameters(current); result != step::unhandled) {
            return result;
        }
        if (context.experiment_enabled("destructuring")) {
            if (const auto result = parse_destructuring(current); result != step::unhandled) {
                return result;
            }
        }
    }
    return parse_expression_statement(current);
}

// Whether [begin, end) begins with a function's parameter list, after any
// template parameter list.
auto is_function_signature(const token_list& tokens, std::size_t begin, std::size_t end) -> bool {
    if (begin < end && tokens[begin].text == "<") {
        const auto close = angle_close(tokens, begin, end);
        if (!close) return false;
        begin = *close + 1;
    }
    return begin < end && tokens[begin].text == "(";
}

auto block_parser::flush() -> void {
    if (statement_begin == index && !saw_expression_start) return;
    if (const auto unsupported = context.unsupported_expression(statement_begin, index)) {
        context.add_diagnostic(
            "unsupported-function-expression", diagnostic_severity::error, *unsupported,
            "unnamed function expressions are not supported");
        statement_begin = index;
        saw_expression_start = false;
        return;
    }
    // A statement with a diagnostic is left out of the tree.
    const auto reported = context.diagnostic_count();
    if (const auto operand = malformed_statement_operand(tokens, statement_begin, index)) {
        // `T name = value;` declares as Cpp1 does.
        const auto& name = tokens[*operand];
        const auto& next = context.token_at(*operand + 1).text;
        const auto type = context.syntax_text(statement_begin, *operand);
        const auto cpp1_declaration = is_declaration_name(name) &&
            (next == "=" || next == "{" || next == "(" ||
             (next == ";" && is_builtin_type_name(type)));
        context.add_diagnostic(
            "invalid-statement-expression", diagnostic_severity::error,
            tokens[statement_begin].span, !cpp1_declaration
                ? std::string{"invalid statement expression"}
                : type == "auto"
                ? "to define a variable " + name.text + " with deduced type, write '" +
                      name.text + " := /* initializer */'"
                : "to define a variable " + name.text + " of type " + type + ", write '" +
                      name.text + ": " + type + " = /* initializer */'");
        statement_begin = index;
        saw_expression_start = false;
        return;
    }
    if (context.diagnose_top_level_comma(
            statement_begin, index > statement_begin && tokens[index - 1].text == ";"
                ? index - 1 : index)) {
        statement_begin = index;
        saw_expression_start = false;
        return;
    }
    auto kind = statement_kind::expression;
    const auto span = tokens[statement_begin].span;
    auto declaration = std::optional<statement::declaration_data>{};
    const auto statement_end = std::min(index, context.token_count());
    auto source = source_text_between(tokens, statement_begin, statement_end, context);
    const auto has_source = !source.empty();
    auto text = context.expression(std::move(source), span.begin);
    if (index - statement_begin >= 2 && tokens[statement_begin].text == "_" &&
        tokens[statement_begin + 1].text == "=") {
        kind = statement_kind::discard_statement;
        text = context.expression(
            statement_text_between(tokens, statement_begin + 2, statement_end, context),
            tokens[statement_begin + 2].span.begin);
    }
    // `throw;` rethrows the exception being handled.
    else if (tokens[statement_begin].text != "throw" &&
             is_single_operand_expression(tokens, statement_begin, index)) {
        context.add_diagnostic(
            "unused-expression-statement", diagnostic_severity::error, span,
            "unused literal or identifier");
        statement_begin = index;
        saw_expression_start = false;
        return;
    }
    const auto prefix = local_declaration_prefix_at(tokens, statement_begin, index);
    if (index - prefix.name >= 3 && is_declaration_name(tokens[prefix.name]) &&
        (tokens[prefix.name + 1].text == ":" ||
         tokens[prefix.name + 1].text == ":=")) {
        const auto inferred = tokens[prefix.name + 1].text == ":=";
        // A `lazy` object (experiment `lazy`) cannot be a local.
        if (index > prefix.name + 3 && tokens[prefix.name + 2].text == "lazy" &&
            tokens[prefix.name + 2].kind == token_kind::identifier &&
            is_type_start(tokens, prefix.name + 3) && context.experiment_enabled("lazy")) {
            context.add_diagnostic(
                "lazy-scope", diagnostic_severity::error, span,
                "a lazy object must be declared at namespace or type scope");
            statement_begin = index;
            saw_expression_start = false;
            return;
        }
        kind = statement_kind::declaration_statement;
        declaration.emplace();
        declaration->is_static = prefix.is_static;
        if (const auto equals = empty_initializer(tokens, statement_begin, index)) {
            context.add_diagnostic(
                "missing-initializer", diagnostic_severity::error, tokens[*equals].span,
                "expected an initializer after '" + tokens[*equals].text + "'");
        }
        if (inferred || (index >= prefix.name + 3 && tokens[prefix.name + 2].text == "=")) {
            // `name :== value` lexes as `:=` `=` and declares a deduced alias.
            const auto is_deduced_alias = inferred && prefix.name + 3 < index &&
                tokens[prefix.name + 2].text == "=";
            const auto initializer_begin = inferred
                ? prefix.name + (is_deduced_alias ? 3 : 2) : prefix.name + 3;
            // `x := i32 = 300` assigns to a type; `x: i32 = 300` was meant.
            if (inferred && initializer_begin + 1 < index &&
                tokens[initializer_begin + 1].text == "=" &&
                is_builtin_type_name(tokens[initializer_begin].text)) {
                context.add_diagnostic(
                    "deduced-declaration-type", diagnostic_severity::error,
                    tokens[prefix.name + 1].span,
                    "':=' deduces the type; write '" + tokens[prefix.name].text + ": " +
                        tokens[initializer_begin].text + " = ...' to declare one");
            }
            auto initializer_text =
                statement_text_between(tokens, initializer_begin, index, context);
            text = context.expression(tokens[prefix.name].text + " = " + initializer_text + ";");
            declaration.emplace();
            declaration->name = tokens[prefix.name].text;
            declaration->type = parse_type("auto");
            declaration->initializer = context.expression(std::move(initializer_text),
                                                          tokens[initializer_begin].span.begin);
            declaration->kind = declaration_kind::object;
            declaration->is_alias = is_deduced_alias;
            declaration->is_static = prefix.is_static;
        }
        else if (!inferred && is_function_signature(tokens, prefix.name + 2, index)) {
            context.add_diagnostic(
                "local-function", diagnostic_severity::error, tokens[prefix.name].span,
                "a function cannot be declared in a function body - write a local "
                "initialized with an unnamed function, as in '" + tokens[prefix.name].text +
                    " := :(x) = x + 1;'");
            statement_begin = index;
            saw_expression_start = false;
            return;
        }
        else if (tokens[prefix.name + 2].text == "@" ||
                 (tokens[prefix.name + 2].text == "type" &&
                  tokens[prefix.name + 3].text != "==" &&
                  !is_type_marker(tokens, prefix.name + 2))) {
            context.add_diagnostic(
                "local-type", diagnostic_severity::error, tokens[prefix.name].span,
                "a type must be in a namespace or type scope - function-local types are "
                "not supported");
            statement_begin = index;
            saw_expression_start = false;
            return;
        }
        else if (tokens[prefix.name + 2].kind == token_kind::keyword &&
                 tokens[prefix.name + 2].text == "namespace" &&
                 tokens[prefix.name + 3].text != "==") {
            context.add_diagnostic(
                "local-namespace", diagnostic_severity::error, tokens[prefix.name + 2].span,
                "a namespace cannot be declared in a function body");
            statement_begin = index;
            saw_expression_start = false;
            return;
        }
        else if (const auto alias = parse_local_alias_prefix(
                     context, tokens, prefix.name, index)) {
            if (alias->is_concept) {
                context.add_diagnostic(
                    "concept-declaration-scope", diagnostic_severity::error, span,
                    "a concept can only be declared at namespace scope");
                statement_begin = index;
                saw_expression_start = false;
                return;
            }
            if (prefix.is_static) {
                context.add_diagnostic(
                    "static-alias", diagnostic_severity::error, span,
                    "'static' is only allowed on function-local objects, not aliases");
                statement_begin = index;
                saw_expression_start = false;
                return;
            }
            const auto statement_end = index > 0 && tokens[index - 1].text == ";" ?
                index - 1 : index;
            auto value =
                statement_text_between(tokens, alias->value_begin, statement_end, context);
            text = context.expression(tokens[prefix.name].text + " = " + value + ";");
            declaration->name = tokens[prefix.name].text;
            declaration->kind = alias->is_type_alias ? declaration_kind::type
                : alias->is_namespace_alias ? declaration_kind::namespace_node
                : declaration_kind::object;
            declaration->is_alias = true;
            declaration->is_static = prefix.is_static;
            if (alias->is_type_alias || alias->is_namespace_alias) {
                declaration->alias = std::move(value);
                if (alias->is_type_alias) context.check_alias_type(alias->value_begin, statement_end);
            }
            else {
                declaration->initializer = context.expression(
                    std::move(value), tokens[alias->value_begin].span.begin);
            }
            declaration->template_parameters = alias->template_parameters;
            if (alias->type_begin != alias->type_end) {
                declaration->type = context.syntax_type(alias->type_begin, alias->type_end);
            }
        }
        else if (index >= prefix.name + 3 && is_type_start(tokens, prefix.name + 2)) {
            const auto statement_end = index > 0 && tokens[index - 1].text == ";"
                ? index - 1 : index;
            const auto declaration_type_end =
                type_end(tokens, prefix.name + 2, statement_end,
                         context.experiment_enabled("type-operators"));
            if (declaration_type_end <= prefix.name + 2 ||
                declaration_type_end > statement_end) {
                statement_begin = index;
                saw_expression_start = false;
                return;
            }
            declaration->type = context.syntax_type(prefix.name + 2, declaration_type_end);
            context.check_cpp1_auto(declaration->type, tokens[prefix.name].text, ":= value");
            const auto is_alias = declaration_type_end < statement_end &&
                tokens[declaration_type_end].text == "==";
            if (declaration_type_end < statement_end && !is_alias &&
                tokens[declaration_type_end].text != "=") {
                context.add_diagnostic(
                    "invalid-declaration-initializer", diagnostic_severity::error,
                    tokens[declaration_type_end].span,
                    "typed local declaration must use '=' before its initializer");
                statement_begin = index;
                saw_expression_start = false;
                return;
            }
            if (declaration_type_end + 1 < statement_end &&
                (tokens[declaration_type_end].text == "=" || is_alias)) {
                auto initializer = context.source_at(
                    tokens[declaration_type_end + 1].span.begin,
                    tokens[statement_end - 1].span.end);
                while (!initializer.empty() &&
                       (initializer.back() == ';' ||
                        std::isspace(static_cast<unsigned char>(initializer.back())))) {
                    initializer.pop_back();
                }
                text = context.expression(tokens[prefix.name].text + " = " + initializer + ";");
                declaration->name = tokens[prefix.name].text;
                declaration->initializer = context.expression(
                    std::move(initializer), tokens[declaration_type_end + 1].span.begin);
                declaration->kind = declaration_kind::object;
                declaration->is_alias = is_alias;
                declaration->is_static = prefix.is_static;
            }
            else if (declaration_type_end == statement_end) {
                if (declaration->type.kind == type_kind::deduced) {
                    context.add_diagnostic(
                        "deduced-object-initializer", diagnostic_severity::error, span,
                        "an object with a deduced type must have an = initializer");
                }
                text = context.expression(tokens[prefix.name].text + " ;");
                declaration->name = tokens[prefix.name].text;
                declaration->kind = declaration_kind::object;
                declaration->is_static = prefix.is_static;
            }
        }
    }
    if (declaration && declaration->kind == declaration_kind::object &&
        !declaration->is_alias) {
        context.check_anonymous_object(declaration->name, declaration->type,
                                       tokens[prefix.name].span);
    }
    if (has_source && context.diagnostic_count() == reported &&
        !(declaration && declaration->is_alias && !declaration->alias.empty())) {
        const auto discard = kind == statement_kind::discard_statement;
        context.diagnose_juxtaposition(text, statement_begin + (discard ? 2 : 0), index,
                                       !discard);
    }
    if (has_source && context.diagnostic_count() == reported) {
        auto current = std::make_unique<statement>(kind);
        current->span = span;
        if (kind == statement_kind::declaration_statement) {
            current->as<statement::declaration_form>() = {std::move(text), std::move(declaration)};
        }
        else {
            current->as<statement::expression_form>().text = std::move(text);
        }
        statements.push_back(std::move(current));
    }
    statement_begin = index;
    saw_expression_start = false;
}

// An `if`, `while` or `do` condition is present and, parenthesized or not, is
// not an assignment.
auto block_parser::check_condition(std::size_t begin, std::size_t end) -> void {
    if (begin == end) {
        context.add_diagnostic("missing-condition", diagnostic_severity::error,
                               context.token_at(end).span,
                               "expected a condition before '" + context.token_at(end).text + "'");
        return;
    }
    constexpr auto assignments = std::array<std::string_view, 11>{
        "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="};
    while (end - begin > 2 && tokens[begin].text == "(" &&
           detail::bracket_end(tokens, begin, end) == end - 1) {
        ++begin;
        --end;
    }
    const auto found = find_top_level(tokens, begin, end, angle_rule::expression,
        [&](std::size_t position) {
            return std::find(assignments.begin(), assignments.end(), tokens[position].text) !=
                assignments.end();
        });
    if (found < end && !detail::is_close_bracket(tokens[found].text)) {
        context.add_diagnostic("assignment-condition", diagnostic_severity::error,
                               tokens[found].span,
                               "a condition cannot be an assignment; to compare, use '=='");
    }
}

auto block_parser::parse_using(const token& current) -> step {
    auto using_name = std::string{};
    auto using_end = index;
    const auto using_result = context.using_declaration(using_end, using_name);
    if (using_result == using_parse_result::valid) {
        flush();
        auto current_statement = std::make_unique<statement>(statement_kind::using_declaration);
        current_statement->span.begin = current.span.begin;
        current_statement->span.end = context.token_at(using_end - 1).span.end;
        current_statement->as<statement::using_form>().name = std::move(using_name);
        statements.push_back(std::move(current_statement));
        index = using_end;
        statement_begin = index;
        saw_expression_start = false;
        return step::next;
    }
    if (using_result == using_parse_result::invalid) {
        context.diagnose_invalid_using(index);
        while (index < tokens.size() - 1 && context.token_at(index).text != ";" &&
               context.token_at(index).text != "}") {
            ++index;
        }
        if (context.token_at(index).text == ";") ++index;
        statement_begin = index;
        saw_expression_start = false;
        return step::next;
    }
    return step::unhandled;
}

auto block_parser::parse_loop_or_branch(const token& current, bool is_labeled_loop) -> step {
    if (is_labeled_loop) {
        flush();
        current_label = current.text;
        index += 2;
    }
    const auto open = is_labeled_loop ? context.token_at(index) : current;
    auto current_statement = std::make_unique<statement>(
        open.text == "while" ? statement_kind::while_statement :
        open.text == "do" ? statement_kind::do_statement :
        open.text == "for" ? statement_kind::for_statement : statement_kind::if_statement);
    current_statement->span = current.span;
    auto label = std::exchange(current_label, {});
    auto body = statement_list{};
    auto condition = parsed_expression{};
    auto next = parsed_expression{};
    auto range_parameter = std::optional<parameter>{};
    auto bindings = std::vector<statement::binding>{};
    // Moves the parsed parts into the statement's form and appends it.
    const auto push = [&] {
        if (!label.empty() && !uses_label(body, label)) {
            context.add_diagnostic(
                "unused-loop-label", diagnostic_severity::error, current.span,
                label + ": a named loop must have its name used (did you forget 'break " +
                    label + ";' or 'continue " + label + ";'?)");
        }
        if (current_statement->kind == statement_kind::if_statement) {
            auto& form = current_statement->as<statement::if_form>();
            form.condition = std::move(condition);
            form.body = std::move(body);
            form.bindings = std::move(bindings);
        }
        else {
            current_statement->as<statement::loop_form>() = {
                std::move(label), std::move(condition), std::move(next),
                std::move(range_parameter), std::move(body), std::move(bindings)};
        }
        statements.push_back(std::move(current_statement));
    };
    ++index;
    if (current_statement->kind == statement_kind::if_statement &&
        context.token_at(index).text == "constexpr") {
        current_statement->as<statement::if_form>().is_constexpr = true;
        ++index;
    }

    if (current_statement->kind == statement_kind::do_statement) {
        const auto open = context.token_at(index);
        if (open.text != "{") {
            context.add_diagnostic("expected-do-body", diagnostic_severity::error, open.span, "expected '{' after 'do'");
            return step::stop;
        }
        ++index;
        const auto close = context.token_at(index);
    const auto scan = bracket_close(tokens, index - 1, tokens.size() - 1);
        if (!scan) {
            context.add_diagnostic("unterminated-do-body", diagnostic_severity::error, open.span, "expected '}'");
            return step::stop;
        }
        const auto close_index = *scan;
        current_statement->span.end = tokens[close_index].span.end;
        auto saved_index = index;
        parse_block(body, saved_index, tokens[close_index], depth + 1);
        index = close_index + 1;
        if (context.token_at(index).text == "next") {
            ++index;
            auto next_begin = index;
            skip_condition([&](std::size_t at) {
                return context.token_at(at).text == "while" || context.token_at(at).text == "{";
            });
            if (context.token_at(index).text != "while") {
                context.add_diagnostic("expected-do-next-while", diagnostic_severity::error, context.token_at(index).span, "expected 'while' after do-statement next expression");
                return step::stop;
            }
            if (next_begin == index) diagnose_empty_next(next_begin - 1);
            auto next_text = std::string{};
            auto next_end = next_begin;
            while (next_end < tokens.size() - 1 && context.token_at(next_end).text != "while") {
                ++next_end;
            }
            next_text = context.source_at(
                context.token_at(next_begin).span.begin,
                context.token_at(next_end - 1).span.end);
            next_text += ';';
            next = context.expression(std::move(next_text), context.token_at(next_begin).span.begin);
            check_next(next, next_begin, next_end);
        }
        if (context.token_at(index).text != "while") {
            context.add_diagnostic("expected-do-while", diagnostic_severity::error,
                                   context.token_at(index).span,
                                   "a 'do' loop body must be followed by 'while'");
            return step::stop;
        }
        ++index;
        const auto condition_begin = index;
        skip_condition([&](std::size_t at) {
            const auto& text = context.token_at(at).text;
            return text == "{" || text == "}" || text == ";";
        });
        check_condition(condition_begin, index);
        if (condition_begin != index) {
            const auto condition_end = index - 1;
            condition = context.expression(context.source_at(
                context.token_at(condition_begin).span.begin, context.token_at(condition_end).span.end),
                context.token_at(condition_begin).span.begin);
            context.diagnose_juxtaposition(condition, condition_begin, index, true);
        }
        if (condition_begin == index) condition = context.expression("true");
        expect_semicolon("'do' loop condition");
    }
    if (current_statement->kind == statement_kind::do_statement) {
        push();
        statement_begin = index;
        saw_expression_start = false;
        return step::next;
    }
    else {
        const auto condition_begin = index;
        if (current_statement->kind == statement_kind::for_statement &&
            context.token_at(index).text == "next") {
            context.add_diagnostic(
                "unexpected-for-next-clause", diagnostic_severity::error,
                context.token_at(index).span,
                "'next' must follow the for range"
            );
            ++index;
        }
        // `next` starts a loop's next clause where it follows an operand; elsewhere
        // it is an ordinary name.
        const auto is_next_clause = [&](std::size_t at) {
            if (context.token_at(at).text != "next" || at == condition_begin ||
                current_statement->kind == statement_kind::if_statement) {
                return false;
            }
            const auto& previous = context.token_at(at - 1);
            return previous.kind != token_kind::punctuation || previous.text == ")" ||
                previous.text == "]" || previous.text == "}" || previous.text == "++" ||
                previous.text == "--" ||
                previous.text == "*" || previous.text == "&";
        };
        skip_condition([&](std::size_t at) {
            const auto& text = context.token_at(at).text;
            return text == "{" || text == ";" || text == "}" || is_next_clause(at) ||
                text == "do";
        });
        if (const auto& text = context.token_at(index).text; text == ";" || text == "}") {
            return missing_body_braces(open.text == "if" ? "an 'if' branch"
                                       : open.text == "while" ? "a 'while' loop"
                                                              : "a 'for' loop",
                                       condition_begin);
        }
        if (current_statement->kind != statement_kind::for_statement) {
            check_condition(condition_begin, index);
        }
        else if (condition_begin == index) {
            context.add_diagnostic("missing-range", diagnostic_severity::error,
                                   context.token_at(index).span, "expected a range after 'for'");
        }
        if (condition_begin != index) {
            const auto condition_end = index - 1;
            condition = context.expression(context.source_at(
                context.token_at(condition_begin).span.begin, context.token_at(condition_end).span.end),
                context.token_at(condition_begin).span.begin);
            // A range is a value, possibly a list; a root assignment condition
            // is reported above. An `if constexpr` condition binds nothing.
            const auto range = current_statement->kind == statement_kind::for_statement;
            const auto binds = !range &&
                !(current_statement->is<statement::if_form>() &&
                  current_statement->as<statement::if_form>().is_constexpr);
            context.diagnose_juxtaposition(condition, condition_begin, index, !range, range,
                                           binds ? &bindings : nullptr);
        }
        if (context.token_at(index).text == "next") {
            ++index;
            const auto next_begin = index;
            auto next_end = index;
            const auto next_stop = current_statement->kind == statement_kind::for_statement
                ? std::string{"do"}
                : std::string{"{"};
            while (index < tokens.size() - 1 && context.token_at(index).text != next_stop) {
                ++index;
            }
            if (next_begin == index) diagnose_empty_next(next_begin - 1);
            else {
                next_end = index - 1;
                next = context.expression(context.source_at(
                    context.token_at(next_begin).span.begin,
                    context.token_at(next_end).span.end), context.token_at(next_begin).span.begin);
                check_next(next, next_begin, index);
            }
        }
        if (current_statement->kind == statement_kind::for_statement &&
            context.token_at(index).kind == token_kind::keyword &&
            context.token_at(index).text == "do") {
            auto next_text = std::move(next);
            ++index;
            const auto parameter_begin = index;
            if (context.token_at(index).text != "(") {
                context.add_diagnostic(
                    "expected-for-parameter", diagnostic_severity::error,
                    context.token_at(index).span, "expected '(' after 'do'");
                return step::stop;
            }
            const auto parameter_close = bracket_close(tokens, index, tokens.size() - 1);
            if (!parameter_close) {
                context.add_diagnostic(
                    "unterminated-for-parameter", diagnostic_severity::error,
                    context.token_at(parameter_begin).span, "expected ')'");
                return step::stop;
            }
            index = *parameter_close;
            auto range_parameter_value = parameter{};
            if (!context.parse_parameter_range(parameter_begin + 1, index,
                                               range_parameter_value)) {
                context.add_diagnostic(
                    "invalid-for-parameter", diagnostic_severity::error,
                    context.token_at(parameter_begin).span,
                    "expected a loop parameter name with an optional type");
                return step::stop;
            }
            context.check_in_parameter_type(range_parameter_value);
            range_parameter = std::move(range_parameter_value);
            next = std::move(next_text);
            ++index;
            if (context.token_at(index).text != "{") {
                auto body_index = index;
                parse_block(body, body_index, terminating_token,
                            depth + 1, true);
                if (body_index == index) {
                    context.add_diagnostic(
                        "expected-body", diagnostic_severity::error,
                        context.token_at(index).span,
                        "expected a statement after the loop parameter");
                    return step::stop;
                }
                index = body_index;
                push();
                statement_begin = index;
                saw_expression_start = false;
                return step::next;
            }
        }
        if (index >= tokens.size() - 1) {
            context.add_diagnostic("expected-body", diagnostic_severity::error, current.span, "expected block body");
            return step::stop;
        }
    }

    const auto body_open = context.token_at(index);
    if (body_open.text != "{") {
        context.add_diagnostic("expected-block-body", diagnostic_severity::error, body_open.span, "expected '{' after loop or branch condition");
        return step::stop;
    }
    ++index;
    const auto scan = bracket_close(tokens, index - 1, tokens.size() - 1);
    if (!scan) {
        context.add_diagnostic("unterminated-block", diagnostic_severity::error, open.span, "expected '}'");
        return step::stop;
    }
    const auto close_index = *scan;
    const auto close = context.token_at(close_index);
    auto returned_index = index;
    parse_block(body, returned_index, close, depth + 1);
    index = close_index + 1;
    push();
    statement_begin = index;
    saw_expression_start = false;
    return step::next;
}

auto block_parser::parse_else(const token& current) -> step {
    // The innermost `if` of the preceding else-if chain still lacking an else.
    auto find_else_parent = [&]() -> std::optional<statement::if_form*> {
        if (statements.empty() || !statements.back()->is<statement::if_form>()) {
            return std::nullopt;
        }
        auto* parent = &statements.back()->as<statement::if_form>();
        while (parent->has_else) {
            auto* nested = parent->else_body.size() == 1
                ? std::get_if<statement::if_form>(&parent->else_body.back()->form) : nullptr;
            if (nested == nullptr || !nested->is_else_if) return std::nullopt;
            parent = nested;
        }
        return parent;
    };
    auto else_parent = find_else_parent();
    if (!else_parent) {
        context.add_diagnostic(
            "else-without-if", diagnostic_severity::error, current.span,
            "'else' must follow an 'if' block"
        );
        ++index;
        return step::next;
    }
    ++index;
    auto else_if = std::unique_ptr<statement>{};
    if (context.token_at(index).kind == token_kind::keyword &&
        context.token_at(index).text == "if") {
        else_if = std::make_unique<statement>(statement_kind::if_statement);
        else_if->span = current.span;
        else_if->as<statement::if_form>().is_else_if = true;
        ++index;
        if (context.token_at(index).text == "constexpr") {
            else_if->as<statement::if_form>().is_constexpr = true;
            ++index;
        }
        const auto condition_begin = index;
        skip_condition([&](std::size_t at) {
            const auto& text = context.token_at(at).text;
            return text == "{" || text == ";" || text == "}";
        });
        if (context.token_at(index).text != "{") {
            return missing_body_braces("an 'if' branch", condition_begin);
        }
        check_condition(condition_begin, index);
        if (condition_begin != index) {
            const auto condition_end = index - 1;
            else_if->as<statement::if_form>().condition = context.expression(context.source_at(
                context.token_at(condition_begin).span.begin,
                context.token_at(condition_end).span.end), context.token_at(condition_begin).span.begin);
            context.diagnose_juxtaposition(else_if->as<statement::if_form>().condition,
                                           condition_begin, index, true, false,
                                           else_if->as<statement::if_form>().is_constexpr
                                               ? nullptr
                                               : &else_if->as<statement::if_form>().bindings);
        }
    }
    if (context.token_at(index).text != "{") return missing_body_braces("an 'else' branch", index);
    ++index;
    const auto scan = bracket_close(tokens, index - 1, tokens.size() - 1);
    if (!scan) {
        context.add_diagnostic(
            "unterminated-else-body", diagnostic_severity::error, current.span,
            "expected '}'"
        );
        return step::stop;
    }
    const auto close_index = *scan;
    const auto close = context.token_at(close_index);
    auto returned_index = index;
    (*else_parent)->has_else = true;
    if (else_if) {
        parse_block(else_if->as<statement::if_form>().body, returned_index, close, depth + 1);
        (*else_parent)->else_body.push_back(std::move(else_if));
    }
    else {
        parse_block((*else_parent)->else_body, returned_index, close, depth + 1);
    }
    index = close_index + 1;
    statement_begin = index;
    saw_expression_start = false;
    return step::next;
}

// `break` or `continue`, an optional loop label, then `;`.
auto block_parser::parse_jump(const token& current) -> step {
    auto current_statement = std::make_unique<statement>(current.text == "break"
        ? statement_kind::break_statement
        : statement_kind::continue_statement);
    current_statement->span = current.span;
    ++index;
    if (is_declaration_name(context.token_at(index))) {
        current_statement->as<statement::jump_form>().label = context.token_at(index).text;
        ++index;
    }
    expect_semicolon("'" + current.text + "' statement");
    statements.push_back(std::move(current_statement));
    statement_begin = index;
    saw_expression_start = false;
    return step::next;
}

// Reports the body of `what` that is not enclosed in braces: the statement
// after the condition that begins at `condition_begin` and ends at the `;`
// or `}` at `index`, which the parse moves past.
auto block_parser::missing_body_braces(std::string_view what, std::size_t condition_begin)
    -> step {
    const auto body = condition_begin == index ? index
                                               : context.expression_end(condition_begin, index);
    context.add_diagnostic("unbraced-body", diagnostic_severity::error,
                           context.token_at(body).span,
                           std::string{what} + " body must be enclosed with { }");
    if (context.token_at(index).text == ";") ++index;
    statement_begin = index;
    saw_expression_start = false;
    return step::next;
}

// Moves past the `;` ending a statement, or reports it missing and skips to
// the next `;` in the block.
auto block_parser::expect_semicolon(std::string_view what) -> void {
    if (context.token_at(index).text == ";") {
        ++index;
        return;
    }
    const auto& found = context.token_at(index);
    context.add_diagnostic("missing-semicolon", diagnostic_severity::error,
                           found.text == "}" || found.kind == token_kind::end_of_file
                               ? context.token_at(index - 1).span : found.span,
                           "expected ';' at the end of the " + std::string{what});
    index = find_top_level(tokens, index, tokens.size() - 1, angle_rule::expression,
        [&](std::size_t position) {
            return is_terminator(tokens[position]) || tokens[position].text == ";";
        });
    if (context.token_at(index).text == ";") ++index;
}

auto block_parser::parse_return(const token& current) -> step {
    auto return_text = std::string{};
    auto current_statement = std::make_unique<statement>(statement_kind::return_statement);
    current_statement->span = current.span;
    ++index;
    // `return :T = value;` returns a typed temporary.
    const auto typed_temporary = context.token_at(index).text == ":" &&
        (context.token_at(index + 1).kind == token_kind::identifier ||
         context.token_at(index + 1).text == "::");
    if ((context.token_at(index).text == ":" && !typed_temporary) ||
        context.token_at(index).text == ":=" || context.token_at(index).text == "=") {
        context.add_diagnostic(
            "invalid-return-expression", diagnostic_severity::error,
            context.token_at(index).span, "expected an expression after 'return'");
    }
    // Brackets within the value, as in `return inspect x -> T { ... };`, are nested.
    index = find_top_level(tokens, index, tokens.size() - 1, angle_rule::expression,
        [&](std::size_t position) {
            return is_terminator(tokens[position]) || tokens[position].text == ";";
        });
    const auto return_end = index;
    expect_semicolon("'return' statement");
    if (return_end > statement_begin + 1) {
        return_text = context.source_at(tokens[statement_begin + 1].span.begin,
                                        tokens[return_end - 1].span.end);
    }
    current_statement->as<statement::expression_form>().text = context.expression(
        std::move(return_text), tokens[std::min(statement_begin + 1, tokens.size() - 1)].span.begin);
    context.diagnose_juxtaposition(current_statement->as<statement::expression_form>().text,
                                   statement_begin + 1, return_end, false, true);
    statements.push_back(std::move(current_statement));
    statement_begin = index;
    saw_expression_start = false;
    return step::next;
}

auto block_parser::parse_try(const token& current) -> step {
    auto current_statement = std::make_unique<statement>(statement_kind::try_statement);
    current_statement->span = current.span;
    auto& form = current_statement->as<statement::block_form>();
    ++index;

    const auto try_open = context.token_at(index);
    if (try_open.text != "{") {
        context.add_diagnostic(
            "expected-try-body", diagnostic_severity::error, try_open.span,
            "'try' must be followed by a block enclosed in braces");
        return step::stop;
    }
    const auto try_scan = bracket_close(tokens, index, tokens.size() - 1);
    if (!try_scan) {
        context.add_diagnostic(
            "unterminated-try-body", diagnostic_severity::error, try_open.span,
            "expected '}'");
        return step::stop;
    }
    auto try_index = index + 1;
    parse_block(form.body, try_index, context.token_at(*try_scan),
                depth + 1);
    index = *try_scan + 1;

    if (context.token_at(index).kind != token_kind::keyword ||
        context.token_at(index).text != "catch") {
        context.add_diagnostic(
            "missing-catch-handler", diagnostic_severity::error,
            context.token_at(index).span,
            "a 'try' block must be followed by at least one 'catch' handler");
        statements.push_back(std::move(current_statement));
        statement_begin = index;
        saw_expression_start = false;
        return step::next;
    }

    const auto catch_span = context.token_at(index).span;
    auto handler_failed = false;
    while (context.token_at(index).kind == token_kind::keyword &&
           context.token_at(index).text == "catch") {
        ++index;
        const auto parameter_open = context.token_at(index);
        if (parameter_open.text != "(") {
            context.add_diagnostic(
                "expected-catch-parameters", diagnostic_severity::error,
                parameter_open.span,
                "'catch' must be followed by a parenthesized parameter declaration");
            handler_failed = true;
            return step::stop;
        }
        const auto parameter_close = bracket_close(tokens, index, tokens.size() - 1);
        if (!parameter_close) {
            context.add_diagnostic(
                "unterminated-catch-parameters", diagnostic_severity::error,
                parameter_open.span, "expected ')'");
            handler_failed = true;
            return step::stop;
        }
        auto caught = parameter{};
        if (!context.parse_parameter_range(index + 1, *parameter_close, caught) ||
            caught.name == "this" || caught.name == "that") {
            context.add_diagnostic(
                "invalid-catch-parameter", diagnostic_severity::error,
                parameter_open.span,
                "expected a parameter declaration in 'catch(...)'");
            handler_failed = true;
            return step::stop;
        }
        if (caught.name != "_" && caught.type.is_wildcard()) {
            context.add_diagnostic(
                "catch-parameter-requires-type", diagnostic_severity::error,
                parameter_open.span,
                "a named catch parameter requires a type; use 'catch(_)' to catch all");
            handler_failed = true;
            return step::stop;
        }
        if (caught.name == "_" && !caught.type.is_wildcard()) {
            context.add_diagnostic(
                "invalid-catch-all", diagnostic_severity::error,
                caught.name_span,
                "catch-all syntax is 'catch(_)' without a type");
            handler_failed = true;
            return step::stop;
        }
        index = *parameter_close + 1;
        const auto body_open = context.token_at(index);
        if (body_open.text != "{") {
            context.add_diagnostic(
                "expected-catch-body", diagnostic_severity::error, body_open.span,
                "'catch(...)' must be followed by a block enclosed in braces");
            handler_failed = true;
            return step::stop;
        }
        const auto body_scan = bracket_close(tokens, index, tokens.size() - 1);
        if (!body_scan) {
            context.add_diagnostic(
                "unterminated-catch-body", diagnostic_severity::error, body_open.span,
                "expected '}'");
            handler_failed = true;
            return step::stop;
        }
        auto handler = statement::handler{};
        handler.caught = std::move(caught);
        auto handler_index = index + 1;
        parse_block(handler.body, handler_index, context.token_at(*body_scan),
                    depth + 1);
        form.handlers.push_back(std::move(handler));
        index = *body_scan + 1;
        current_statement->span.end = context.token_at(index - 1).span.end;
    }
    if (form.handlers.empty() && !handler_failed) {
        context.add_diagnostic(
            "missing-catch-handler", diagnostic_severity::error, catch_span,
            "a 'try' block must have at least one valid 'catch' handler");
    }
    statements.push_back(std::move(current_statement));
    statement_begin = index;
    saw_expression_start = false;
    return step::next;
}

auto block_parser::parse_compound(const token& current, statement_kind kind) -> step {
    const auto open = current;
    flush();
    ++index;
    const auto scan = bracket_close(tokens, index - 1, tokens.size() - 1);
    if (!scan) {
        context.add_diagnostic(
            "unterminated-block", diagnostic_severity::error, open.span, "expected '}'");
        return step::stop;
    }
    const auto close_index = *scan;
    const auto close = context.token_at(close_index);
    auto current_statement = std::make_unique<statement>(kind);
    current_statement->span.begin = open.span.begin;
    current_statement->span.end = close.span.end;
    auto saved_index = index;
    parse_block(current_statement->as<statement::block_form>().body, saved_index, close,
                depth + 1);
    index = close_index + 1;
    statements.push_back(std::move(current_statement));
    statement_begin = index;
    return step::next;
}

// `defer { ... }` or `defer statement;` (experiment `defer`): the body runs
// when the enclosing block exits.
auto block_parser::parse_defer(const token& current) -> step {
    const auto& next = context.token_at(index + 1);
    if (next.text == ":" || next.text == ":=") {
        context.add_diagnostic("defer-name", diagnostic_severity::error, current.span,
                               "'defer' begins a defer statement while the defer experiment "
                               "is enabled, so it cannot be declared");
        while (index < tokens.size() - 1 && context.token_at(index).text != ";" &&
               context.token_at(index).text != "}") {
            ++index;
        }
        if (context.token_at(index).text == ";") ++index;
        statement_begin = index;
        saw_expression_start = false;
        return step::next;
    }
    auto current_statement = std::make_unique<statement>(statement_kind::defer_statement);
    current_statement->span.begin = current.span.begin;
    auto& form = current_statement->as<statement::block_form>();
    ++index;
    if (next.text == "{") {
        const auto scan = bracket_close(tokens, index, tokens.size() - 1);
        if (!scan) {
            context.add_diagnostic("unterminated-block", diagnostic_severity::error, next.span,
                                   "expected '}'");
            return step::stop;
        }
        auto body_index = index + 1;
        parse_block(form.body, body_index, tokens[*scan], depth + 1);
        index = *scan + 1;
    }
    else {
        if (next.text != ";") parse_block(form.body, index, terminating_token, depth + 1, true);
        if (form.body.empty()) {
            context.add_diagnostic("expected-body", diagnostic_severity::error, next.span,
                                   "expected a statement after 'defer'");
            if (next.text != ";") return step::stop;
            statement_begin = ++index;
            return step::next;
        }
    }
    current_statement->span.end = context.token_at(index - 1).span.end;
    if (const auto* jump = escaping_jump(form.body, {})) {
        context.add_diagnostic("defer-control-flow", diagnostic_severity::error, jump->span,
                               "a defer body cannot return, break or continue out of the "
                               "deferred statement");
    }
    statements.push_back(std::move(current_statement));
    statement_begin = index;
    saw_expression_start = false;
    return step::next;
}

// `unsafe { ... }` (experiment `unsafe`): a block whose runtime checks are
// not emitted.
auto block_parser::parse_unsafe(const token& current) -> step {
    flush();
    ++index;
    statement_begin = index;
    const auto result = parse_compound(context.token_at(index), statement_kind::unsafe_block);
    if (result == step::next) statements.back()->span.begin = current.span.begin;
    return result;
}

// `(copy x := value, inout y := other) statement`. Each parameter is
// `[passing] name := value` or `[passing] name: type = value`; anything else is
// left to the expression statement parser.
auto block_parser::parse_statement_parameters(const token& current) -> step {
    const auto scan = bracket_close(tokens, index, tokens.size() - 1);
    if (!scan) return step::unhandled;
    const auto close = *scan;
    auto parameters = std::vector<initialized_parameter>{};
    auto position = index + 1;
    while (position < close) {
        const auto head = position;
        if (context.parameter_passing_style(tokens[position].text)) ++position;
        if (position + 1 >= close || !is_declaration_name(tokens[position])) return step::unhandled;
        const auto separator = tokens[position + 1].text;
        if (separator != ":" && separator != ":=") return step::unhandled;
        // The type, for `name: type =`, ends at the top-level `=`.
        auto equals = position + 1;
        if (separator == ":") {
            auto angles = 0;
            auto groups = 0;
            for (++equals; equals < close; ++equals) {
                const auto& value = tokens[equals].text;
                if (value == "(" || value == "[") ++groups;
                else if (value == ")" || value == "]") --groups;
                else if (value == "<") ++angles;
                else if (value == ">") --angles;
                else if (groups == 0 && angles == 0 && (value == "=" || value == ",")) break;
            }
            // `name: type` cannot begin an expression, so this is a statement
            // parameter missing its initializer.
            if (equals >= close || tokens[equals].text != "=") {
                auto value = parameter{};
                if (!context.parse_parameter_range(head, equals, value)) return step::unhandled;
                context.add_diagnostic("statement-parameter-initializer",
                                       diagnostic_severity::error, value.name_span,
                                       "a statement parameter requires an initializer");
                parameters.push_back({std::move(value), {}});
                position = equals < close ? equals + 1 : equals;
                continue;
            }
        }
        auto end = equals + 1;
        for (auto groups = 0; end < close; ++end) {
            const auto& value = tokens[end].text;
            if (value == "(" || value == "[" || value == "{") ++groups;
            else if (value == ")" || value == "]" || value == "}") --groups;
            else if (groups == 0 && value == ",") break;
        }
        if (end == equals + 1) return step::unhandled;
        auto value = parameter{};
        if (!context.parse_parameter_range(head, separator == ":" ? equals : position + 1, value)) {
            return step::unhandled;
        }
        context.check_anonymous_object(value.name, value.type, value.name_span);
        // A statement parameter is initialized by its initializer, which an
        // `out` parameter would have to construct.
        if (value.passing == passing_style::out) {
            context.add_diagnostic("statement-parameter-out", diagnostic_severity::error,
                                   context.token_at(head).span,
                                   "a statement parameter cannot be 'out'");
        }
        auto initializer = parsed_expression{context.source_at(
            tokens[equals + 1].span.begin, tokens[end - 1].span.end), false, false, false,
            tokens[equals + 1].span.begin};
        if (!context.diagnose_block_initializer(equals + 1, value.name_span)) {
            context.diagnose_juxtaposition(initializer, equals + 1, end, false, true);
        }
        parameters.push_back({std::move(value), std::move(initializer)});
        position = end < close ? end + 1 : end;
    }
    if (parameters.empty()) return step::unhandled;

    auto current_statement = std::make_unique<statement>(statement_kind::block);
    current_statement->span = current.span;
    auto& form = current_statement->as<statement::block_form>();
    form.parameters = std::move(parameters);
    auto body_index = close + 1;
    parse_block(form.body, body_index, terminating_token, depth + 1, true);
    if (form.body.empty()) {
        context.add_diagnostic("expected-body", diagnostic_severity::error,
                               context.token_at(close + 1).span,
                               "expected a statement after the statement parameters");
        return step::stop;
    }
    index = body_index;
    current_statement->span.end = context.token_at(index - 1).span.end;
    statements.push_back(std::move(current_statement));
    statement_begin = index;
    saw_expression_start = false;
    return step::next;
}

// `(a, b) := initializer;` with the `destructuring` experiment. Anything that
// is not a name list followed by `:=` or `:` is left to the expression parser.
auto block_parser::parse_destructuring(const token& current) -> step {
    const auto close = destructuring_names_end(tokens, index);
    if (!close) return step::unhandled;
    const auto separator = *close + 1;
    const auto end = tokens.size() - 1;
    const auto semicolon = find_top_level(tokens, separator + 1, end, angle_rule::expression,
        [&](std::size_t position) {
            return tokens[position].text == ";" || tokens[position].text == "}" ||
                is_terminator(tokens[position]);
        });
    // The statement ends at its `;`, else before the `}` or terminator found.
    const auto last = semicolon < end && tokens[semicolon].text == ";" ? semicolon : semicolon - 1;
    const auto finish = [&] {
        index = last + 1;
        statement_begin = index;
        saw_expression_start = false;
        return step::next;
    };
    if (tokens[separator].text == ":") {
        context.add_diagnostic("destructuring-type", diagnostic_severity::error,
                               tokens[separator].span,
                               "a destructuring declaration deduces its types - write ':='");
        return finish();
    }
    auto current_statement = std::make_unique<statement>(statement_kind::destructuring_statement);
    auto& form = current_statement->as<statement::destructuring_form>();
    for (auto position = index + 1; position < *close; position += 2) {
        const auto& name = tokens[position];
        const auto duplicate = name.text != "_" &&
            std::any_of(form.bindings.begin(), form.bindings.end(),
                        [&](const auto& binding) { return binding.name == name.text; });
        if (duplicate) {
            context.add_diagnostic("destructuring-duplicate-name", diagnostic_severity::error,
                                   name.span,
                                   "'" + name.text + "' is bound twice in a destructuring declaration");
        }
        form.bindings.push_back({name.text, name.span, invalid_symbol});
    }
    if (form.bindings.size() < 2) {
        context.add_diagnostic("destructuring-arity", diagnostic_severity::error, current.span,
                               "a destructuring declaration needs at least two names");
    }
    const auto initializer_begin = separator + 1;
    if (initializer_begin >= semicolon) {
        context.add_diagnostic("missing-initializer", diagnostic_severity::error,
                               tokens[separator].span, "expected an initializer after ':='");
        return finish();
    }
    // `(1, 2)` is a list, not one tuple-like value.
    const auto list_close = tokens[initializer_begin].text == "("
        ? bracket_close(tokens, initializer_begin, semicolon) : std::nullopt;
    if (list_close && *list_close + 1 == semicolon &&
        find_top_level(tokens, initializer_begin + 1, *list_close, angle_rule::expression,
            [&](std::size_t position) { return tokens[position].text == ","; }) < *list_close) {
        context.add_diagnostic("destructuring-initializer", diagnostic_severity::error,
                               tokens[initializer_begin].span,
                               "a destructuring initializer is one tuple-like value - write "
                               "std::tuple(...) for a list");
        return finish();
    }
    if (const auto unsupported = context.unsupported_expression(initializer_begin, semicolon)) {
        context.add_diagnostic(
            "unsupported-function-expression", diagnostic_severity::error, *unsupported,
            "unnamed function expressions are not supported");
        return finish();
    }
    form.initializer =
        context.expression(source_text_between(tokens, initializer_begin, semicolon, context),
                           tokens[initializer_begin].span.begin);
    if (context.diagnose_juxtaposition(form.initializer, initializer_begin, semicolon)) {
        return finish();
    }
    current_statement->span = {current.span.begin, tokens[last].span.end};
    statements.push_back(std::move(current_statement));
    return finish();
}

// Whether `body` cannot complete normally: its last statement is `return`,
// `break`, `continue`, `throw`, a call to a function that does not return,
// or an `if` with an `else` whose branches each diverge.
auto diverges(const statement_list& body) -> bool {
    static constexpr auto no_return = std::array<std::string_view, 5>{
        "std::terminate", "std::abort", "std::exit", "std::quick_exit", "std::unreachable"};
    if (body.empty()) return false;
    const auto& last = *body.back();
    switch (last.kind) {
    case statement_kind::return_statement:
    case statement_kind::break_statement:
    case statement_kind::continue_statement:
        return true;
    case statement_kind::if_statement: {
        const auto& form = last.as<statement::if_form>();
        return form.has_else && diverges(form.body) && diverges(form.else_body);
    }
    case statement_kind::expression: {
        const auto& tree = last.as<statement::expression_form>().text.tree();
        if (!tree.tokens.empty() && tree.tokens.front().text == "throw") return true;
        if (tree.root.children.empty()) return false;
        const auto& node = tree.root.children.front();
        if (node.kind != expression_kind::call || node.children.empty()) return false;
        const auto& callee = node.children.front();
        auto name = std::string{};
        for (auto at = callee.first; at < callee.last; ++at) name += tree.tokens[at].text;
        return std::find(no_return.begin(), no_return.end(), name) != no_return.end();
    }
    default:
        return false;
    }
}

// Experiment `let-else`: `name := value as T else { ... }`, and with
// `destructuring`, `(a, b) := value as T else { ... }`. The declaration
// before `else` is parsed as a statement of its own; no `;` follows the
// block, as after any statement that ends in a block.
auto block_parser::parse_let_else(const token& current) -> step {
    auto separator = index + 1;
    if (current.text == "(") {
        const auto close = destructuring_names_end(tokens, index);
        if (!close || !context.experiment_enabled("destructuring")) return step::unhandled;
        separator = *close + 1;
    }
    else if (!is_declaration_name(current)) {
        return step::unhandled;
    }
    const auto end = tokens.size() - 1;
    if (separator >= end || tokens[separator].text != ":=") return step::unhandled;
    const auto keyword = find_top_level(tokens, separator + 1, end, angle_rule::expression,
        [&](std::size_t position) {
            return tokens[position].text == ";" || tokens[position].text == "}" ||
                is_terminator(tokens[position]) ||
                (tokens[position].kind == token_kind::keyword && tokens[position].text == "else");
        });
    if (keyword >= end || is_terminator(tokens[keyword]) || tokens[keyword].text != "else" ||
        tokens[keyword + 1].text != "{") {
        return step::unhandled;
    }
    const auto close = bracket_close(tokens, keyword + 1, end);
    if (!close) return step::unhandled;
    const auto count = statements.size();
    auto position = index;
    block_parser{context, tokens, statements, position, tokens[keyword], depth}
        .parse_statement(current);
    auto else_body = statement_list{};
    auto body_index = keyword + 2;
    parse_block(else_body, body_index, tokens[*close], depth + 1);
    index = *close + 1;
    statement_begin = index;
    saw_expression_start = false;
    if (statements.size() != count + 1) return step::next;
    auto& declared = *statements.back();
    const auto* initializer = [&]() -> const parsed_expression* {
        if (auto* form = std::get_if<statement::destructuring_form>(&declared.form)) {
            return &form->initializer;
        }
        auto* form = std::get_if<statement::declaration_form>(&declared.form);
        return form && form->declaration && !form->declaration->initializer.empty()
            ? &form->declaration->initializer : nullptr;
    }();
    // The initializer is one `value as T`.
    const auto is_cast = [](const expression_text& tree) {
        return tree.root.children.size() == 1 &&
            tree.root.children.front().kind == expression_kind::is_as &&
            tree.tokens[tree.root.children.front().op].text == "as";
    };
    if (initializer == nullptr || !is_cast(initializer->tree())) {
        context.add_diagnostic("let-else-initializer", diagnostic_severity::error,
                               tokens[separator].span,
                               "a let-else declaration's initializer is 'value as T'");
        return step::next;
    }
    if (!diverges(else_body)) {
        context.add_diagnostic("let-else-falls-through", diagnostic_severity::error,
            else_body.empty() ? tokens[keyword].span : else_body.back()->span,
            "the 'else' block of a let-else declaration must end in 'return', 'throw', "
            "'break', 'continue' or a call to 'std::terminate', 'std::abort', 'std::exit', "
            "'std::quick_exit' or 'std::unreachable'");
    }
    declared.span.end = tokens[*close].span.end;
    std::visit([&](auto& form) {
        using form_type = std::remove_cvref_t<decltype(form)>;
        if constexpr (std::is_same_v<form_type, statement::declaration_form> ||
                      std::is_same_v<form_type, statement::destructuring_form>) {
            form.else_body = std::move(else_body);
            form.is_let_else = true;
        }
    }, declared.form);
    return step::next;
}

// `assert<group, flags...>(condition, message);`
auto block_parser::parse_contract(const token& current) -> step {
    auto current_statement = std::make_unique<statement>(statement_kind::contract_statement);
    current_statement->span = current.span;
    if (!context.contract(index, current_statement->as<statement::contract_form>())) {
        return step::stop;
    }
    expect_semicolon("contract");
    current_statement->span.end = context.token_at(index - 1).span.end;
    statements.push_back(std::move(current_statement));
    statement_begin = index;
    saw_expression_start = false;
    return step::next;
}

auto block_parser::parse_expression_statement(const token& current) -> step {
    if (context.trace_enabled()) {
        context.trace_event("statement token index=" + std::to_string(index) +
            " begin=" + std::to_string(statement_begin) + " text=" + current.text + '@' +
            std::to_string(current.span.begin.line) + ':' +
            std::to_string(current.span.begin.column));
    }
    if (statement_begin != index) return step::next;
    statement_begin = index;
    saw_expression_start = true;
    if (current.kind == token_kind::punctuation &&
        (current.text == "@" || current.text == "?" || current.text == "=" ||
         current.text == ":" || current.text == "," || current.text == ")" ||
         current.text == "]")) {
        context.add_diagnostic(
            "invalid-statement-start", diagnostic_severity::error, current.span,
            "token cannot begin a statement");
        while (index < tokens.size() - 1 && context.token_at(index).text != ";" &&
               context.token_at(index).text != "}") {
            ++index;
        }
        if (context.token_at(index).text == ";") ++index;
        statement_begin = index;
        saw_expression_start = false;
        return step::next;
    }
    // `name: { ... }` is a declaration missing its `=`; only loops have labels.
    if (is_declaration_name(current) && context.token_at(index + 1).text == ":" &&
        context.token_at(index + 2).text == "{") {
        context.add_diagnostic("missing-equal-before-body", diagnostic_severity::error,
                               context.token_at(index + 2).span,
                               "expected '=' before '{' - did you mean '= {' ?");
        const auto close = bracket_close(tokens, index + 2, tokens.size() - 1);
        index = close ? *close + 1 : tokens.size() - 1;
        statement_begin = index;
        saw_expression_start = false;
        return step::next;
    }
    // `name: T = { ... }` or `name := { ... }` initializes an object with a block.
    const auto prefix = local_declaration_prefix_at(tokens, statement_begin, tokens.size());
    if (is_declaration_name(tokens[prefix.name]) &&
        (context.token_at(prefix.name + 1).text == ":" ||
         context.token_at(prefix.name + 1).text == ":=")) {
        const auto equals = context.token_at(prefix.name + 1).text == ":=" ? prefix.name + 1
            : type_end(tokens, prefix.name + 2, tokens.size() - 1,
                       context.experiment_enabled("type-operators"));
        const auto close = equals < tokens.size() - 1 &&
                (equals == prefix.name + 1 || tokens[equals].text == "=")
            ? context.diagnose_block_initializer(equals + 1, tokens[prefix.name].span)
            : std::nullopt;
        if (close) {
            index = *close + 1;
            if (context.token_at(index).text == ";") ++index;
            statement_begin = index;
            saw_expression_start = false;
            return step::next;
        }
    }
    ++index;
    if (context.trace_enabled()) {
        context.trace_event("expression start begin=" + std::to_string(statement_begin) +
            " index=" + std::to_string(index));
    }
    // The first `(`, `[` or `{` left open.
    auto unclosed = std::optional<std::size_t>{};
    index = find_top_level(tokens, statement_begin, tokens.size() - 1, angle_rule::expression,
        [&](std::size_t position) {
            return position != statement_begin &&
                (is_terminator(tokens[position]) || tokens[position].text == ";");
        },
        [&](std::size_t open) {
            if (!unclosed || open < *unclosed) unclosed = open;
        });
    if (unclosed) {
        const auto& open = tokens[*unclosed].text;
        context.add_diagnostic(
            open == "(" ? "unmatched-open-parenthesis"
                : open == "[" ? "unmatched-open-bracket" : "unmatched-open-brace",
            diagnostic_severity::error, tokens[*unclosed].span,
            open == "(" ? "expected ')'" : open == "[" ? "expected ']'" : "expected '}'");
        // The statement ends at the first `;` after the open bracket.
        auto end = *unclosed;
        while (end < index && tokens[end].text != ";") ++end;
        if (end < index) index = end + 1;
        statement_begin = index;
        saw_expression_start = false;
        return step::next;
    }
    if (const auto& next = context.token_at(index); index < tokens.size() - 1) {
        if (is_terminator(next)) {
            // A local function, which `flush` reports, may end at its body's `}`.
            const auto local_function = tokens[statement_begin + 1].text == ":" &&
                is_function_signature(tokens, statement_begin + 2, index);
            if (next.text == "}" && !local_function) {
                context.add_diagnostic(
                    "missing-semicolon", diagnostic_severity::error,
                    context.token_at(index - 1).span, "expected ';' at end of statement");
            }
        }
        else if (next.text == ")") {
            context.add_diagnostic(
                "unmatched-close-parenthesis", diagnostic_severity::error,
                next.span, "')' has no matching '('");
        }
        else if (next.text == "]") {
            context.add_diagnostic(
                "unmatched-close-bracket", diagnostic_severity::error,
                next.span, "']' has no matching '['");
        }
        else if (next.text == ";") {
            ++index;
        }
    }
    if (context.trace_enabled()) {
        context.trace_event("expression end begin=" + std::to_string(statement_begin) +
            " index=" + std::to_string(index));
    }
    if (index < tokens.size()) {
        flush();
    }
    return step::next;
}

}

auto statement_parser::parse_block(std::vector<std::unique_ptr<statement>>& statements,
                                   std::size_t& index, const token& terminating_token,
                                   std::size_t depth, bool single_statement) -> void {
    // Braces are bounded by `max_nesting_depth`; this bounds bodies nested
    // without braces, as in `for r do (x) for s do (y) f(x, y);`.
    if (depth > max_nesting_depth) {
        context.add_diagnostic(
            "statement-depth-limit-exceeded", diagnostic_severity::error,
            terminating_token.span,
            "statements are nested more than " + std::to_string(max_nesting_depth) +
                " levels deep");
        return;
    }
    if (context.trace_enabled()) {
        context.trace_event("statements enter index=" + std::to_string(index) +
            " terminator=" + terminating_token.text +
            '@' + std::to_string(terminating_token.span.begin.line) + ':' +
            std::to_string(terminating_token.span.begin.column));
    }
    auto block = block_parser{context, tokens, statements, index, terminating_token, depth};
    while (index < tokens.size() - 1) {
        if (single_statement && !statements.empty() &&
            !(context.token_at(index).kind == token_kind::keyword &&
              context.token_at(index).text == "else")) {
            break;
        }
        const auto before = index;
        if (block.parse_statement(context.token_at(index)) == step::stop) break;
        if (index == before) {
            context.add_diagnostic(
                "statement-progress-stalled", diagnostic_severity::error,
                context.token_at(index).span, "statement parsing made no progress");
            break;
        }
    }
    if (depth == 0) {
        auto labels = std::vector<std::string>{};
        check_jumps(context, statements, labels);
    }
}

}
