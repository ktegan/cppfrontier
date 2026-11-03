#include "parser_context.hpp"

#include "cpp1_expression_renderer.hpp"

#include <algorithm>
#include <array>
#include <iostream>

namespace cppfrontier::parser_detail {

auto empty_initializer(const token_list& tokens, std::size_t begin, std::size_t end)
    -> std::optional<std::size_t> {
    const auto equals = find_top_level(tokens, begin, end, angle_rule::declaration,
        [&](std::size_t position) {
            return tokens[position].text == "=" || tokens[position].text == ":=";
        });
    if (equals == end || (tokens[equals].text != "=" && tokens[equals].text != ":=")) {
        return std::nullopt;
    }
    auto next = equals + 1;
    while (next < end && tokens[next].text == ";") ++next;
    if (next != end && tokens[next].text != "}") return std::nullopt;
    return equals;
}

namespace {

auto token_text(const token_list& tokens) {
    return [&](std::size_t index) -> std::string_view {
        return index < tokens.size() ? std::string_view{tokens[index].text} : std::string_view{};
    };
}

auto token_operand(const token_list& tokens) {
    return [&](std::size_t index) {
        if (index >= tokens.size()) return false;
        const auto kind = tokens[index].kind;
        return (kind == token_kind::identifier || kind == token_kind::integer_literal ||
                kind == token_kind::string_literal || kind == token_kind::character_literal) &&
            !detail::is_statement_word(tokens[index].text);
    };
}

}

auto template_argument_close(const token_list& tokens, std::size_t open)
    -> std::optional<std::size_t> {
    return detail::template_argument_close(token_text(tokens), token_operand(tokens), open,
                                           tokens.size());
}

auto template_argument_closes(const token_list& tokens)
    -> std::vector<std::optional<std::size_t>> {
    return detail::template_argument_closes(token_text(tokens), token_operand(tokens),
                                            tokens.size());
}

auto is_type_marker(const token_list& tokens, std::size_t position) -> bool {
    if (position + 1 >= tokens.size() || tokens[position].kind != token_kind::keyword ||
        tokens[position].text != "type") {
        return false;
    }
    const auto& next = tokens[position + 1];
    return next.text == "::" || next.text == "decltype" || next.kind == token_kind::identifier;
}

auto is_requires_body(const token_list& tokens, std::size_t open) -> bool {
    if (open == 0) return false;
    auto before = open - 1;
    if (tokens[before].text == ")") {
        const auto parameters = bracket_open(tokens, 0, before);
        if (!parameters || *parameters == 0) return false;
        before = *parameters - 1;
    }
    return tokens[before].text == "requires";
}

auto template_list_close(const token_list& tokens, std::size_t open, std::size_t end, bool type)
    -> std::optional<std::size_t> {
    if (type || (open != 0 && tokens[open - 1].text == ":")) return angle_close(tokens, open, end);
    if (open == 0 || tokens[open - 1].kind != token_kind::identifier) return std::nullopt;
    const auto close = template_argument_close(tokens, open);
    return close && *close < end ? close : std::nullopt;
}

auto top_level_items(const token_list& tokens, std::size_t begin, std::size_t end, angle_rule rule)
    -> std::vector<std::pair<std::size_t, std::size_t>> {
    auto result = std::vector<std::pair<std::size_t, std::size_t>>{};
    for (auto item = begin; item < end;) {
        const auto comma = find_top_level(tokens, item, end, rule,
            [&](std::size_t position) { return tokens[position].text == ","; });
        if (comma > item) result.emplace_back(item, comma);
        if (comma < end && tokens[comma].text != ",") break;
        item = comma + 1;
    }
    return result;
}

auto is_type_start(const token_list& tokens, std::size_t position) -> bool {
    if (position >= tokens.size()) return false;
    const auto& value = tokens[position];
    return value.text == "*" || value.text == "const" || is_type_name(value) ||
        is_type_marker(tokens, position) ||
        (value.text == "::" && position + 1 < tokens.size() && is_type_name(tokens[position + 1]));
}

auto type_end(const token_list& tokens, std::size_t begin, std::size_t end, bool type_operators)
    -> std::size_t {
    if (begin >= end) return begin;
    auto position = begin;
    while (position < end && (tokens[position].text == "*" || tokens[position].text == "const")) {
        ++position;
    }
    if (position < end && is_type_marker(tokens, position)) ++position;
    if (position >= end) return end;
    // Qualifiers alone omit the `_` wildcard: `x: const = 1;`.
    if (position > begin && (tokens[position].text == "=" || tokens[position].text == "==")) {
        return position;
    }
    if (tokens[position].text == "(") {
        const auto close = bracket_close(tokens, position, end);
        if (!close) return end;
        const auto arrow = *close + 1;
        if (arrow < end && tokens[arrow].text == "->") {
            return type_end(tokens, arrow + 1, end, type_operators);
        }
        // `(A | B) | C`: another operand follows the group.
        if (type_operators && arrow + 1 < end &&
            (tokens[arrow].text == "|" || tokens[arrow].text == "&")) {
            return type_end(tokens, arrow + 1, end, type_operators);
        }
        return arrow;
    }
    // `::name` is qualified from the global namespace.
    if (tokens[position].text == "::" && position + 1 < end) ++position;
    if (!is_type_name(tokens[position])) return end;

    ++position;
    // `decltype(expression)` and `type_of(expression)` are single type operands.
    if ((tokens[position - 1].text == "decltype" || tokens[position - 1].text == "type_of") &&
        position < end && tokens[position].text == "(") {
        const auto close = bracket_close(tokens, position, end);
        if (!close) return end;
        position = *close + 1;
    }
    return find_top_level(tokens, position, end, angle_rule::type, [&](std::size_t at) {
        const auto& value = tokens[at];
        // A contract `pre(...)` or `post<...>(...)` follows a complete type.
        const auto is_contract = (value.text == "pre" || value.text == "post") &&
            value.kind == token_kind::identifier && tokens[at - 1].text != "::" &&
            at + 1 < end && (tokens[at + 1].text == "(" || tokens[at + 1].text == "<");
        const auto operand = type_operators &&
            (tokens[at - 1].text == "|" || tokens[at - 1].text == "&");
        return value.text == "," || value.text == "=" || value.text == "==" ||
            value.text == ";" || value.text == "{" || value.text == "requires" || is_contract ||
            (value.text == "(" && tokens[at - 1].text != "decltype" && !operand);
    });
}

auto type_grammar_end(const token_list& tokens, std::size_t begin, std::size_t end,
                      bool type_operators) -> std::size_t {
    const auto text = [&](std::size_t at) -> std::string_view {
        return at < end ? std::string_view{tokens[at].text} : std::string_view{};
    };
    // The `>` closing the list at `open`; a `>>` at `end` may close it too. A
    // `>>` before `end` closes only lists the type opens.
    const auto list_end = [&](std::size_t open) -> std::optional<std::size_t> {
        if (const auto close = angle_close(tokens, open, end)) {
            auto depth = std::size_t{0};
            for (auto at = open; at < *close; ++at) {
                if (detail::is_open_bracket(tokens[at].text)) at = *bracket_close(tokens, at, end);
                else if (tokens[at].text == "<") ++depth;
                else depth -= detail::angle_closes(tokens[at].text);
            }
            return detail::angle_closes(tokens[*close].text) > depth ? std::optional{*close}
                                                                     : *close + 1;
        }
        const auto outer = angle_close(tokens, open, std::min(end + 1, tokens.size()));
        return outer && *outer == end ? std::optional{end} : std::nullopt;
    };
    auto at = begin;
    while (text(at) == "*" || text(at) == "const") ++at;
    if (at < end && is_type_marker(tokens, at)) ++at;
    if (at >= end) return at;
    if (text(at) == "(") {
        const auto close = bracket_close(tokens, at, end);
        if (!close) return at;
        at = *close + 1;
        if (text(at) == "->") {
            ++at;
            if (text(at) == "forward" || text(at) == "forward_ref") ++at;
            return type_grammar_end(tokens, at, end, type_operators);
        }
        // `(A | B) | C` (experiment `type-operators`).
        if (!type_operators) return at;
    }
    else if ((text(at) == "decltype" || text(at) == "type_of") && text(at + 1) == "(") {
        const auto close = bracket_close(tokens, at + 1, end);
        if (!close) return at;
        at = *close + 1;
    }
    else if (text(at) == "_" && text(at + 1) == "is") {
        return type_grammar_end(tokens, at + 2, end, type_operators);
    }
    else if (text(at) == "auto") {
        ++at;
    }
    else if (is_cpp1_fundamental_word(text(at)) && !text(at + 1).starts_with("::")) {
        while (is_cpp1_fundamental_word(text(at))) ++at;
    }
    else {
        if (text(at) == "::") ++at;
        if (at >= end || !is_type_name(tokens[at])) return at;
        ++at;
    }
    // Template lists and `::` member names.
    for (;;) {
        if (text(at) == "<") {
            const auto close = list_end(at);
            if (!close) return at;
            at = *close;
        }
        else if (text(at) == "::" && at + 1 < end &&
                 (is_type_name(tokens[at + 1]) || tokens[at + 1].text == "template")) {
            at += tokens[at + 1].text == "template" ? 3 : 2;
        }
        else {
            break;
        }
    }
    // A pack `T...` or pack index `T...[I]`, and a Cpp1 reference `T&` or `T&&`, which is
    // reported later.
    if (text(at) == "...") {
        if (text(at + 1) != "[") return at + 1;
        const auto close = bracket_close(tokens, at + 1, end);
        return close ? *close + 1 : at + 1;
    }
    if (type_operators && (text(at) == "|" || text(at) == "&") && at + 1 < end) {
        return type_grammar_end(tokens, at + 1, end, type_operators);
    }
    if ((text(at) == "&" || text(at) == "&&") && at + 1 == end) return end;
    return at;
}

auto destructuring_names_end(const token_list& tokens, std::size_t begin)
    -> std::optional<std::size_t> {
    return detail::destructuring_names_end(tokens, begin, [](const token& value) {
        return is_declaration_name(value) || value.text == "_";
    });
}

token_cursor::token_cursor(const source_text& source,
                           const token_list& tokens,
                           std::vector<diagnostic>& diagnostics,
                           trace_sink trace,
                           const std::vector<source_replacement>* replacements,
                           const experiment_set* experiments)
    : source_{&source}, tokens_{&tokens}, diagnostics_{&diagnostics}, trace_{trace},
      replacements_{replacements}, experiments_{experiments} {}

auto token_cursor::experiment_enabled(experiment_id id) const noexcept -> bool {
    return experiments_ != nullptr && experiments_->enabled(id);
}

auto token_cursor::token_at(std::size_t position) const -> const token& {
    return position < tokens_->size() ? (*tokens_)[position] : tokens_->back();
}

auto token_cursor::token_count() const noexcept -> std::size_t {
    return tokens_->size();
}

auto token_cursor::trace_enabled() const noexcept -> bool {
    return trace_.kind == trace_sink_kind::stderr_stream;
}

auto token_cursor::trace_event(std::string_view message) const -> void {
    if (trace_.kind != trace_sink_kind::stderr_stream) return;
    std::cerr << message << '\n';
}

auto token_cursor::source_at(source_position begin, source_position end) const -> std::string {
    if (!replacements_) return source_->text_between(begin, end);
    const auto key = [](source_position value) { return std::pair{value.line, value.column}; };
    auto result = std::string{};
    auto position = begin;
    // Replacements are ordered by start; each one taken covers those nested in it.
    for (const auto& replacement : *replacements_) {
        if (key(replacement.span.begin) < key(position)) continue;
        if (key(replacement.span.end) > key(end)) {
            if (key(replacement.span.begin) >= key(end)) break;
            continue;
        }
        result += source_->text_between(position, replacement.span.begin) + replacement.text;
        position = replacement.span.end;
    }
    return result + source_->text_between(position, end);
}

auto token_cursor::quoted(const token& written) const -> std::string {
    auto text = source_->text_between(written.span.begin, written.span.end);
    return text.empty() ? written.text : text;
}

auto token_cursor::syntax_text(std::size_t begin, std::size_t end) const -> std::string {
    if (begin >= end || begin >= token_count()) return {};
    end = std::min(end, token_count());
    auto text = source_at(token_at(begin).span.begin, token_at(end - 1).span.end);
    auto position = std::size_t{0};
    while ((position = text.find("> >", position)) != std::string::npos) {
        text.replace(position, 3, ">>");
    }
    return text;
}

auto token_cursor::has_unlexed_text(std::size_t begin, std::size_t end) const -> bool {
    // The gap after the last token holds text the lexer dropped from the end, too.
    end = std::min(end + 1, token_count());
    for (auto position = begin; position + 1 < end; ++position) {
        const auto gap = source_at(token_at(position).span.end, token_at(position + 1).span.begin);
        const auto first = gap.find_first_not_of(" \t\r\n");
        if (first != std::string::npos && gap.compare(first, 2, "//") != 0 &&
            gap.compare(first, 2, "/*") != 0) {
            return true;
        }
    }
    return false;
}

auto token_cursor::syntax_type(std::size_t begin, std::size_t end) -> type_id {
    if (begin >= end || begin >= token_count()) return {};
    end = std::min(end, token_count());
    auto result = parse_type(syntax_text(begin, end),
                             {token_at(begin).span.begin, token_at(end - 1).span.end},
                             type_options());
    check_type_operators(result, false);
    check_type_tokens(begin, end);
    check_type_parts(result, true, true);
    check_type_arguments(result, begin, end);
    return result;
}

auto token_cursor::check_type_arguments(const type_id& type, std::size_t begin, std::size_t end)
    -> void {
    for (const auto& child : type.children) check_type_arguments(child, begin, end);
    for (const auto& component : type.components) {
        for (const auto& argument : component.arguments) {
            if (argument.kind != type_kind::expression) {
                check_type_arguments(argument, begin, end);
                continue;
            }
            // The tokens [first, last) that spell the argument.
            const auto starts_at = [&](std::size_t at, source_position position) {
                return token_at(at).span.begin.line == position.line &&
                    token_at(at).span.begin.column == position.column;
            };
            auto first = begin;
            while (first < end && !starts_at(first, argument.span.begin)) ++first;
            auto last = first;
            while (last < end && !(token_at(last).span.end.line == argument.span.end.line &&
                                   token_at(last).span.end.column == argument.span.end.column)) {
                ++last;
            }
            if (last == end) continue;
            diagnose_juxtaposition(expression(argument.text, argument.span.begin), first,
                                   last + 1, false);
        }
    }
}

auto token_cursor::check_type_tokens(std::size_t begin, std::size_t end) -> void {
    // A type operator without its experiment is reported by `check_type_operators`.
    const auto used = type_grammar_end(*tokens_, begin, end, true);
    if (used >= end) return;
    const auto& stray = token_at(used);
    add_diagnostic("unexpected-type-token", diagnostic_severity::error, stray.span,
                   stray.text == "["
                       ? std::string{"C-style array types are not allowed, use std::array instead"}
                       : "'" + stray.text + "' is not part of the type - is a ',', '=' or ';' "
                         "missing?");
}

auto token_cursor::type_options() const noexcept -> type_parse_options {
    return {.type_operators = experiment_enabled("type-operators")};
}

auto token_cursor::check_alias_type(std::size_t begin, std::size_t end) -> void {
    if (experiment_enabled("type-operators")) static_cast<void>(syntax_type(begin, end));
    else if (begin < end) {
        const auto parsed = parse_type(syntax_text(begin, end),
                                       {token_at(begin).span.begin, token_at(end - 1).span.end});
        check_type_operators(parsed, false);
        check_type_parts(parsed, true, true);
        check_type_arguments(parsed, begin, end);
    }
}

namespace {

// The first combination `A | B` or `A & B` of two written operands in `type`.
auto find_type_operator(const type_id& type) -> const type_id* {
    if (type.is_operator() && std::none_of(type.children.begin(), type.children.end(),
                                           [](const type_id& child) { return child.empty(); })) {
        return &type;
    }
    for (const auto& child : type.children) {
        if (const auto* found = find_type_operator(child)) return found;
    }
    for (const auto& component : type.components) {
        for (const auto& argument : component.arguments) {
            if (const auto* found = find_type_operator(argument)) return found;
        }
    }
    return nullptr;
}

}

auto token_cursor::check_type_operators(const type_id& type, bool constraint) -> void {
    // Without the experiment, `|` and `&` are not parsed as type operators.
    if (!experiment_enabled("type-operators")) {
        const auto parsed = parse_type(type.text, type.span, {.type_operators = true});
        if (const auto* found = find_type_operator(parsed)) {
            add_diagnostic("type-operator-experiment", diagnostic_severity::error, found->span,
                           "the type operator '" +
                               std::string{found->kind == type_kind::union_type ? "|" : "&"} +
                               "' requires '-experiment=type-operators'");
        }
        return;
    }
    if (type.is_operator()) {
        const auto symbol = type.kind == type_kind::union_type ? "|" : "&";
        // Outside a constraint, `int&` is a Cpp1 reference.
        if (type.kind == type_kind::intersection_type && !constraint &&
            type.children.size() == 2 && !type.children.front().empty() &&
            type.children.back().empty()) {
            add_diagnostic("cpp1-reference-type", diagnostic_severity::error, type.span,
                           "'" + type.text + "' is a Cpp1 reference type; Cpp2 types have no "
                           "references - use a passing style such as 'inout' instead");
            return;
        }
        if (std::any_of(type.children.begin(), type.children.end(),
                        [](const type_id& child) { return child.empty(); })) {
            add_diagnostic("type-operator-single", diagnostic_severity::error, type.span,
                           "the type operator '" + std::string{symbol} +
                               "' needs a type on both sides");
            return;
        }
        if (type.kind == type_kind::intersection_type && !constraint) {
            add_diagnostic("type-operator-constraint", diagnostic_severity::error, type.span,
                           "'&' combines constraints, not types: an intersection of types has "
                           "no Cpp1 lowering, so it is allowed only in a template parameter or "
                           "'_ is' constraint");
            return;
        }
    }
    // `_ is C` holds its constraint as its child.
    const auto in_constraint = type.is_operator() ? constraint : type.kind == type_kind::deduced;
    for (const auto& child : type.children) check_type_operators(child, in_constraint);
    for (const auto& component : type.components) {
        for (const auto& argument : component.arguments) {
            check_type_operators(argument, false);
            if (argument.kind != type_kind::expression) continue;
            // `std::vector<int | std::string>` is an expression, which `int` cannot be part of.
            const auto combined = parse_type(argument.text, argument.span, type_options());
            if (combined.is_operator() &&
                std::any_of(combined.children.begin(), combined.children.end(),
                            [](const type_id& operand) {
                                return operand.kind == type_kind::fundamental ||
                                    operand.kind == type_kind::pointer ||
                                    operand.kind == type_kind::constant;
                            })) {
                add_diagnostic("type-operator-argument", diagnostic_severity::error,
                               argument.span,
                               "in a template argument '|' and '&' are Cpp1 operators; write "
                               "'type " + argument.text + "' for a type combination");
            }
        }
    }
}

// `_` lowers to Cpp1's `auto`, so it may be what a declarator qualifies:
// the whole type, below `*` and `const`, or a function type's return type.
auto token_cursor::check_type_parts(const type_id& type, bool placeholder, bool top) -> void {
    if (type.kind == type_kind::deduced) {
        const auto* constraint = type.constraint();
        if (!placeholder) {
            add_diagnostic("nested-wildcard", diagnostic_severity::error, type.span,
                           "'" + type.text + "' cannot be part of a template argument, a type "
                           "operator or a function type's parameter; only a declared type, "
                           "its pointee or a function type's return type can be deduced");
        }
        else if (!top && constraint != nullptr && constraint->is_operator()) {
            add_diagnostic("nested-wildcard", diagnostic_severity::error, type.span,
                           "an operator constraint '" + constraint->text +
                               "' applies only to a whole deduced type '_'");
        }
        else if (constraint != nullptr) {
            check_type_parts(*constraint, false, false);
        }
        return;
    }
    // A generated `-> forward_ref _` return is a reference not written `&`.
    if (type.kind == type_kind::reference && type.text.ends_with('&')) {
        add_diagnostic("cpp1-reference-type", diagnostic_severity::error, type.span,
                       "'" + type.text + "' is a Cpp1 reference type; Cpp2 types have no "
                       "references - use a passing style such as 'inout' instead");
        return;
    }
    if (type.is_function() && !type.children.empty() &&
        std::any_of(type.children.begin(), std::prev(type.children.end()),
                    [](const type_id& child) { return child.empty(); })) {
        add_diagnostic("function-type-parameter", diagnostic_severity::error, type.span,
                       "a function type's parameter is 'name: type', with any passing style "
                       "but 'forward'; it cannot be 'this', a pack or have a default");
        return;
    }
    if (type.is_function() && !type.children.empty() &&
        type.children.back().text.starts_with('(')) {
        add_diagnostic("function-type-named-returns", diagnostic_severity::error,
                       type.children.back().span,
                       "a function type cannot have named return values");
        return;
    }
    // A `forward_ref _` return, the only reference left, is a placeholder.
    const auto qualifies = type.kind == type_kind::pointer || type.kind == type_kind::constant ||
        type.kind == type_kind::reference;
    for (const auto& child : type.children) {
        const auto returned = type.is_function() && &child == &type.children.back();
        check_type_parts(child, placeholder && (qualifies || returned), false);
    }
    for (const auto& component : type.components) {
        for (const auto& argument : component.arguments) check_type_parts(argument, false, false);
    }
}

auto token_cursor::parameter_passing_style(std::string_view text) const
    -> std::optional<passing_style> {
    if (text == "copy") return passing_style::copy;
    if (text == "inout") return passing_style::inout;
    if (text == "move") return passing_style::move;
    if (text == "forward") return passing_style::forward;
    if (text == "out") return passing_style::out;
    if (text == "in") return passing_style::in;
    if (text == "in_ref") return passing_style::in_ref;
    return std::nullopt;
}

auto token_cursor::parse_parameter_range(std::size_t begin, std::size_t end,
                                         parameter& output) -> bool {
    if (begin >= end) return false;

    auto position = begin;
    // `implicit out this` makes a converting constructor; constructors are
    // never emitted `explicit`, so the modifier needs no further handling.
    if (token_at(position).text == "implicit") {
        if (position + 2 >= end || token_at(position + 1).text != "out" ||
            token_at(position + 2).text != "this") {
            add_diagnostic("invalid-implicit-parameter", diagnostic_severity::error,
                           token_at(position).span,
                           "only an 'out this' parameter may be declared implicit");
            return false;
        }
        ++position;
    }
    else if (const auto& text = token_at(position).text;
             text == "virtual" || text == "override" || text == "final") {
        if (const auto& next = token_at(position + 1).text; position + 1 < end &&
            (next == "virtual" || next == "override" || next == "final")) {
            add_diagnostic("invalid-this-specifier", diagnostic_severity::error,
                           token_at(position + 1).span,
                           "a 'this' parameter takes one of virtual, override and final, "
                           "where final also overrides");
            return false;
        }
        const auto receiver_position = position + 1 < end &&
                parameter_passing_style(token_at(position + 1).text)
            ? position + 2 : position + 1;
        if (receiver_position >= end || token_at(receiver_position).text != "this") {
            add_diagnostic("invalid-this-specifier", diagnostic_severity::error,
                           token_at(position).span,
                           "only a 'this' parameter may be declared " + text);
            return false;
        }
        output.this_specifier = text == "virtual" ? virtual_specifier::virtual_function
            : text == "override" ? virtual_specifier::override_function
                                 : virtual_specifier::final_function;
        ++position;
    }
    if (const auto passing = parameter_passing_style(token_at(position).text)) {
        output.passing = *passing;
        ++position;
    }
    if (position >= end) return false;

    if (token_at(position).text == "forward_ref") {
        add_diagnostic("invalid-forward-ref-parameter", diagnostic_severity::error,
                       token_at(position).span,
                       "'forward_ref' is only valid for a deduced return type");
        return false;
    }

    const auto& name = token_at(position);
    const auto receiver = name.kind == token_kind::keyword &&
        (name.text == "this" || name.text == "that");
    if ((!is_declaration_name(name) || name.text.find("::") != std::string::npos) &&
        !receiver) {
        return false;
    }
    output.name = name.text;
    output.name_span = name.span;
    ++position;
    if (!receiver && position < end && token_at(position).text == "...") {
        output.is_variadic = true;
        ++position;
    }

    // `name := value` has a deduced type and a default value.
    const auto missing_default = [&](std::size_t equals) {
        add_diagnostic("empty-default-argument", diagnostic_severity::error,
                       token_at(equals).span,
                       "expected a default argument after '" + token_at(equals).text + "'");
        return false;
    };
    if (!receiver && position < end && token_at(position).text == ":=") {
        if (position + 1 == end) return missing_default(position);
        output.default_value = parsed_expression{syntax_text(position + 1, end)};
        return true;
    }
    if (position < end) {
        if (token_at(position).text != ":") return false;
        if (receiver) {
            add_diagnostic("invalid-receiver-type", diagnostic_severity::error,
                           token_at(position).span,
                           "a 'this' or 'that' parameter knows its type; remove the ':'");
            return true;
        }
        ++position;
        const auto type_end = find_top_level(*tokens_, position, end, angle_rule::type,
            [&](std::size_t at) { return token_at(at).text == "="; });
        if (type_end + 1 == end) return missing_default(type_end);
        if (type_end > position) output.type = syntax_type(position, type_end);
        check_cpp1_auto(output.type, output.name, output.name + ": _");
        if (output.type.kind == type_kind::pack) {
            add_diagnostic("parameter-pack-type", diagnostic_severity::error, output.type.span,
                           "a parameter pack is declared as '" + output.name + "...: " +
                               output.type.children.front().text + "'");
        }
        if (type_end < end) {
            output.default_value = parsed_expression{syntax_text(type_end + 1, end)};
        }
        // `x: T = name: U` initializes with a declaration rather than an expression.
        if (type_end + 2 < end && is_declaration_name(token_at(type_end + 1)) &&
            (token_at(type_end + 2).text == ":" || token_at(type_end + 2).text == ":=")) {
            add_diagnostic("parameter-initializer-not-expression", diagnostic_severity::error,
                           end < token_count() ? token_at(end).span : token_at(end - 1).span,
                           "parameter must be initialized with an expression");
        }
        else if (type_end < end) {
            diagnose_juxtaposition(output.default_value, type_end + 1, end, false, true);
        }
    }
    return true;
}

auto token_cursor::diagnose_top_level_comma(std::size_t begin, std::size_t end) -> bool {
    end = std::min(end, token_count());
    const auto items = top_level_items(*tokens_, begin, end);
    if (items.size() < 2) return false;
    const auto comma = items.front().first == begin ? items.front().second : begin;
    add_diagnostic("top-level-comma", diagnostic_severity::error, token_at(comma).span,
                   "unexpected ',' - Cpp2 has no comma operator, and a declaration declares "
                   "one name; write each as its own statement");
    return true;
}

namespace {

struct expression_problem final {
    std::string_view code;
    // The token at fault.
    std::size_t token = 0;
    std::string word;
    // In an interpolation of the string literal `token`, the offset of the
    // fault in the literal's text.
    std::optional<std::size_t> offset;
    // The token after a misleadingly spaced postfix operator.
    std::string after;
};

// The first problem in `tree`, then in the interpolations of its string
// literals, whose `token` is the literal's.
auto find_expression_problem(const expression_text& tree, bool statement, bool list,
                             bool try_operator, bool comprehension)
    -> std::optional<expression_problem> {
    const auto problem = [&](std::string_view code, std::size_t token) {
        return expression_problem{code, token, tree.tokens[token].text};
    };
    // With the `comprehension` experiment, a `for` that does not begin one is
    // missing its `yield`.
    if (comprehension) {
        if (const auto stray = find_stray_for(tree)) return problem("comprehension-yield", *stray);
    }
    // A statement keyword outside every parsed expression is never an operand.
    const auto unparsed = find_unparsed_tokens(tree);
    const auto keyword = std::find_if(unparsed.begin(), unparsed.end(), [&](auto index) {
        const auto& text = tree.tokens[index].text;
        return tree.tokens[index].kind == expression_token_kind::identifier &&
            (text == "if" || text == "while" || text == "do" || text == "for");
    });
    if (keyword != unparsed.end()) return problem("statement-as-initializer", *keyword);
    if (const auto postfix = find_misleading_postfix(tree)) {
        auto found = problem("misleading-postfix-spacing", *postfix);
        found.after = tree.tokens[*postfix + 1].text;
        return found;
    }
    if (const auto step = find_address_arithmetic(tree)) return problem("pointer-arithmetic", *step);
    if (const auto type = find_type_operand(tree)) {
        return problem(type->after_is ? "type-after-is" : "type-as-operand", type->token);
    }
    auto found = find_juxtaposed_operand(tree);
    auto code = std::string_view{"juxtaposed-operands"};
    if (const auto brace = find_braced_list(tree); brace && (!found || *brace < *found)) {
        found = brace;
        code = "braced-list";
    }
    // A parameter passing style other than these marks no argument, as `f(inout x)`.
    constexpr auto parameter_only = std::array<std::string_view, 4>{"in", "in_ref", "copy", "inout"};
    if (found && code == "juxtaposed-operands" && *found >= 2 &&
        std::ranges::find(parameter_only, tree.tokens[*found - 1].text) != parameter_only.end() &&
        (tree.tokens[*found - 2].text == "(" || tree.tokens[*found - 2].text == ",")) {
        return problem("argument-passing", *found - 1);
    }
    if (found) return problem(code, *found);
    // Otherwise the first token outside every parsed expression. An
    // assignment whose right operand does not parse, or an operator whose
    // parenthesized operand does not, leaves its operator unparsed; the
    // operand's first token is the error.
    if (!unparsed.empty()) {
        constexpr auto assignments = std::array<std::string_view, 9>{
            "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^="};
        constexpr auto brackets = std::string_view{"()[]{};"};
        const auto before_operand = [&](std::size_t at) {
            const auto& text = tree.tokens[at].text;
            const auto& next = tree.tokens[at + 1].text;
            return std::find(assignments.begin(), assignments.end(), text) != assignments.end() ||
                (tree.tokens[at].kind == expression_token_kind::punctuation &&
                 !(text.size() == 1 && brackets.find(text[0]) != std::string_view::npos) &&
                 next == "(");
        };
        auto first = unparsed.begin();
        for (;;) {
            const auto next = std::next(first);
            if (next != unparsed.end() && *next == *first + 1 && before_operand(*first)) {
                ++first;
                continue;
            }
            // An unparsed `(` or `[` left open is the error; one closed holds it.
            if (tree.tokens[*first].text != "(" && tree.tokens[*first].text != "[") break;
            const auto close = detail::bracket_close(tree.tokens, *first, tree.tokens.size());
            if (!close) {
                return problem(tree.tokens[*first].text == "(" ? "unmatched-open-parenthesis"
                                                               : "unmatched-open-bracket",
                               *first);
            }
            if (next == unparsed.end() || *next > *close) break;
            ++first;
        }
        return problem(tree.tokens[*first].text == "{" ? "braced-list" : "incomplete-expression",
                       *first);
    }
    if (const auto assignment = find_value_assignment(tree, statement)) {
        return problem("chained-assignment", *assignment);
    }
    if (const auto group = find_comma_list(tree, list)) return problem("comma-operator", *group);
    for (auto index = std::size_t{0}; index < tree.tokens.size(); ++index) {
        if (tree.tokens[index].kind != expression_token_kind::string) continue;
        const auto& literal = tree.tokens[index].text;
        const auto interpolations = interpolated_expressions(literal);
        for (std::size_t ordinal = 0; ordinal < interpolations.size(); ++ordinal) {
            const auto& nested = interpolations[ordinal];
            if (nested.find_first_not_of(" \t\r\n") == std::string::npos) {
                return problem("empty-interpolation", index);
            }
            const auto parsed = parse_expression_text(nested, try_operator, comprehension);
            if (auto inner = find_expression_problem(parsed, false, false, try_operator,
                                                     comprehension)) {
                auto at = inner->offset.value_or(0);
                for (std::size_t token = 0; token <= inner->token; ++token) {
                    const auto& value = parsed.tokens[token];
                    at += value.leading.size();
                    if (token < inner->token) {
                        at += (value.spelling.empty() ? value.text : value.spelling).size();
                    }
                }
                inner->offset = interpolated_offset(literal, ordinal, at);
                inner->token = index;
                return inner;
            }
        }
    }
    return std::nullopt;
}

}

auto token_cursor::tree_token_position(const expression_text& tree, std::size_t index,
                                       std::size_t begin, std::size_t end) const
    -> std::optional<std::size_t> {
    end = std::min(end, token_count());
    // The tree and the lexer spell the same characters in different tokens: the
    // lexer joins `p*`, `a::b`, `>=` and `>>`, and splits `..<`. A local
    // declaration's text spells its `:=` as `=` and `x: T =` as `x =`.
    {
        auto position = begin;
        auto offset = std::size_t{0};
        const auto next_character = [&] {
            while (position < end && offset == token_at(position).text.size()) {
                ++position;
                offset = 0;
            }
            return position < end;
        };
        auto aligned = true;
        for (auto at = std::size_t{0}; aligned && at < index; ++at) {
            for (const auto character : tree.tokens[at].text) {
                if (!next_character()) {
                    aligned = false;
                    break;
                }
                const auto& written = token_at(position).text;
                if (written[offset] == character) {
                    ++offset;
                    continue;
                }
                if (at == 1 && character == '=' && offset == 0 &&
                    (written == ":=" || written == ":")) {
                    while (position < end && token_at(position).text != ":=" &&
                           token_at(position).text != "=") {
                        ++position;
                    }
                    if (position < end) {
                        offset = token_at(position).text.size();
                        continue;
                    }
                }
                aligned = false;
                break;
            }
        }
        if (aligned && next_character()) return position;
    }
    // Otherwise by its ordinal among the tokens of its spelling. The lexer joins
    // a qualified name into one token, named by its first part, and a name and a
    // `*` after it, `p*`, into one token.
    const auto& target = tree.tokens[index].text;
    const auto counted = [&](std::size_t at) {
        return tree.tokens[at].text == target && (at == 0 || tree.tokens[at - 1].text != "::");
    };
    auto ordinal = std::size_t{0};
    for (auto at = std::size_t{0}; at < index; ++at) ordinal += counted(at) ? 1 : 0;
    // A local declaration's text spells its `:=` as `=`, and the expression
    // lexer reads `:=` as `:` `=`.
    const auto split = std::ranges::any_of(tree.tokens, [&](const expression_token& token) {
        return &token != &tree.tokens.back() && token.text == ":" && (&token + 1)->text == "=" &&
            (&token + 1)->leading.empty();
    });
    const auto matches = [&](std::string_view text) {
        return text == target || (text == ":=" && (target == "=" || (target == ":" && split))) ||
            (text.starts_with(target) && text.substr(target.size()).starts_with("::"));
    };
    for (auto position = begin; position < end; ++position) {
        const auto& found = token_at(position);
        if (found.kind == token_kind::pointer_dereference) {
            const auto name = std::string_view{found.text}.substr(0, found.text.size() - 1);
            if ((target == name || target == "*") && ordinal-- == 0) return position;
            continue;
        }
        if (matches(found.text) && ordinal-- == 0) return position;
    }
    return std::nullopt;
}

auto token_cursor::expression_end(std::size_t begin, std::size_t end) const -> std::size_t {
    const auto value = expression(syntax_text(begin, end));
    const auto& tree = value.tree();
    auto found = find_juxtaposed_operand(tree);
    const auto unparsed = find_unparsed_tokens(tree);
    if (!unparsed.empty() && (!found || unparsed.front() < *found)) found = unparsed.front();
    if (!found) return end;
    return tree_token_position(tree, *found, begin, end).value_or(end);
}

// Experiment `condition-bindings`: each binding must be a name, bound once
// in the text, and its pattern test an operand of the top-level `&&` chain
// of an `if` or `while` condition.
auto token_cursor::check_pattern_tests(const parsed_expression& value, std::size_t begin,
                                       std::size_t end,
                                       std::vector<statement::binding>* bindings) -> void {
    if (!experiment_enabled("condition-bindings")) return;
    const auto& tree = value.tree();
    const auto& tokens = tree.tokens;
    auto operands = std::vector<const cppfrontier::expression*>{};
    if (bindings != nullptr && tree.root.children.size() == 1) {
        const auto& whole = tree.root.children.front();
        const auto chain = whole.kind == expression_kind::binary &&
            tokens[whole.children.front().last].text == "&&";
        if (!chain) operands.push_back(&whole);
        for (const auto& operand : whole.children) {
            if (chain) operands.push_back(&operand);
        }
    }
    // The written token of the tree's token `index`.
    const auto written_at = [&](std::size_t index) -> const token& {
        return token_at(tree_token_position(tree, index, begin, end).value_or(begin));
    };
    auto names = std::vector<std::string_view>{};
    const auto invalid = [&](std::size_t index, const std::string& message) {
        add_diagnostic("invalid-condition-binding", diagnostic_severity::error,
                       written_at(index).span, message);
    };
    const auto bind = [&](const cppfrontier::expression& name, bool allowed) {
        const auto& text = tokens[name.first].text;
        if (name.kind != expression_kind::name || name.last != name.first + 1) {
            invalid(name.first, "a destructuring lists names and '_', as in 'is T(a, _)'");
            return;
        }
        if (text == "_") return;
        const auto& written = written_at(name.first);
        if (!is_declaration_name(written)) {
            invalid(name.first, "'" + text + "' cannot be bound - a binding must be a name");
            return;
        }
        if (std::find(names.begin(), names.end(), text) != names.end()) {
            invalid(name.first, "'" + text + "' is bound twice in a condition");
            return;
        }
        names.push_back(text);
        if (allowed) bindings->push_back({text, written.span, invalid_symbol});
    };
    const auto walk = [&](const auto& self, const cppfrontier::expression& node) -> void {
        for (const auto& child : node.children) self(self, child);
        if (node.kind != expression_kind::pattern_test) return;
        const auto allowed = std::find(operands.begin(), operands.end(), &node) != operands.end();
        if (!allowed) {
            add_diagnostic("condition-binding-position", diagnostic_severity::error,
                           written_at(node.op).span,
                           "a pattern test that binds names must be an operand of the "
                           "top-level '&&' chain of an 'if' or 'while' condition");
        }
        const auto parts = inspect_alternative_of(tokens, node.children[1]);
        if (parts.destructuring != nullptr) {
            if (parts.destructuring->children.empty()) {
                invalid(parts.destructuring->first,
                        "a destructuring lists one or more names and '_'");
            }
            for (const auto& item : parts.destructuring->children) bind(item, allowed);
        }
        if (parts.binding != nullptr) {
            binding_operators_.push_back(written_at(parts.binding->first - 1).span);
            if (tokens[parts.binding->first].text == "_") {
                invalid(parts.binding->first, "'as _' binds nothing - remove it");
            }
            else {
                bind(*parts.binding, allowed);
            }
        }
    };
    walk(walk, tree.root);
}

// The type of a typed value `:T = v` takes the checks of a declared type.
// The tree's text may differ from the source's around the expression, so each
// typed value is found as the next `: T =` in the source with its spelling.
auto token_cursor::check_typed_values(const parsed_expression& value, std::size_t begin,
                                      std::size_t end) -> void {
    const auto& tree = value.tree();
    end = std::min(end, token_count());
    auto from = begin;
    const auto walk = [&](const auto& self, const cppfrontier::expression& node) -> void {
        if (node.kind == expression_kind::typed_value) {
            auto spelling = std::string{};
            for (auto at = node.children[0].first; at < node.children[0].last; ++at) {
                spelling += tree.tokens[at].text;
            }
            for (auto colon = from; colon < end; ++colon) {
                if (token_at(colon).text != ":") continue;
                auto written = std::string{};
                auto at = colon + 1;
                while (at < end && written.size() < spelling.size()) written += token_at(at++).text;
                if (written != spelling || at >= end || token_at(at).text != "=") continue;
                static_cast<void>(syntax_type(colon + 1, at));
                from = at;
                break;
            }
        }
        for (const auto& child : node.children) self(self, child);
    };
    walk(walk, tree.root);
}

auto token_cursor::diagnose_juxtaposition(const parsed_expression& value, std::size_t begin,
                                          std::size_t end, bool statement, bool list,
                                          std::vector<statement::binding>* bindings) -> bool {
    check_pattern_tests(value, begin, end, bindings);
    check_typed_values(value, begin, end);
    const auto& tree = value.tree();
    const auto problem = find_expression_problem(tree, statement, list,
                                                 experiment_enabled("try-operator"),
                                                 experiment_enabled("comprehension"));
    // Validation reports each `@`, and each `?` without the `try-operator` experiment.
    if (!problem || problem->word == "@" ||
        (problem->word == "?" && !experiment_enabled("try-operator"))) {
        return false;
    }
    // Cpp2 has no `->` member access.
    const auto code = problem->word == "->" ? std::string_view{"cpp1-member-arrow"}
                                            : problem->code;
    auto word = problem->word;
    end = std::min(end, token_count());
    auto span = std::optional<source_span>{};
    if (const auto position = tree_token_position(tree, problem->token, begin, end)) {
        const auto& found = token_at(*position);
        span = found.span;
        // The `*` of a joined `p*`.
        if (word == "*" && found.kind == token_kind::pointer_dereference) {
            span->begin.column = span->end.column - 1;
        }
        else if (word == tree.tokens[problem->token].text) {
            word = quoted(found);
        }
        // A fault in an interpolation of a literal written as the tree spells it,
        // not one whose captures a function expression renamed.
        if (problem->offset && quoted(found) == tree.tokens[problem->token].text) {
            const auto advance = [](source_position position, std::string_view text) {
                for (const auto value : text) {
                    position.column = value == '\n' ? 1 : position.column + 1;
                    if (value == '\n') ++position.line;
                }
                return position;
            };
            span->begin = advance(found.span.begin,
                                  std::string_view{found.text}.substr(0, *problem->offset));
            span->end = advance(span->begin, word);
        }
    }
    // Text the lexer could not tokenize has its own diagnostic.
    if (code == "incomplete-expression" && has_unlexed_text(begin, end)) return true;
    if (!span) span = token_at(begin).span;
    add_diagnostic(code, diagnostic_severity::error, *span,
                   code == "cpp1-member-arrow"
                       ? std::string{"'->' is not Cpp2 dereference syntax - write '*.' instead"}
                   : code == "comprehension-yield"
                       ? std::string{"a for-comprehension is 'for range do (name) [if guard] "
                                     "yield value' - 'yield' is missing"}
                   : code == "argument-passing"
                       ? "'" + word + "' cannot mark an argument - an argument is marked only "
                         "'out', 'move' or 'forward'"
                   : code == "statement-as-initializer"
                       ? "'" + word + "' begins a statement, not an expression"
                   : code == "empty-interpolation"
                       ? std::string{"a string interpolation '(...)$' needs an expression "
                                     "inside its parentheses"}
                   : code == "unmatched-open-parenthesis" ? std::string{"expected ')'"}
                   : code == "unmatched-open-bracket" ? std::string{"expected ']'"}
                   : code == "incomplete-expression"
                       ? "'" + word + "' is not part of an expression - is an operand missing?"
                   : code == "type-as-operand" ? "'" + word + "' is a type, not a value"
                   : code == "type-after-is"
                       ? "'" + word + "' is a type, not a value - 'v is T' tests one type; to "
                         "test several, write 'v is A || v is B'"
                   : code == "pointer-arithmetic"
                       ? word + " - pointer arithmetic is illegal - use std::span or gsl::span "
                         "instead"
                   : code == "misleading-postfix-spacing"
                       ? "this '" + word + "' is a postfix " +
                         (word == "*" ? "dereference" : "address-of") + " of the operand " +
                         "before it, not a binary '" + word + "' - " +
                         (problem->after == "+" || problem->after == "-"
                              ? "to " + std::string{word == "*" ? "multiply" : "take a bitwise and"} +
                                    ", parenthesize the right operand, as 'a " + word +
                                    " (-b)'; otherwise remove the space before '" + word + "'"
                              : problem->after == "..."
                              ? "remove the space before '" + word + "'; for a fold over '" +
                                    word + "', write '(... " + word + " xs)'"
                              : "remove the space before '" + word + "'; to " +
                                    (word == "*" ? "multiply" : "take a bitwise and") +
                                    ", write the right operand with postfix operators, as 'a " +
                                    word + " p*'")
                   : code == "chained-assignment"
                       ? std::string{"an assignment is not a value and cannot be chained - "
                                     "instead of 'c = b = a;', write 'b = a; c = b;'"}
                   : code == "braced-list"
                       ? std::string{"a braced list '{...}' is not Cpp2; write '(...)' or a "
                                     "typed value ':T = (...)'"}
                   : code == "comma-operator"
                       ? std::string{"Cpp2 has no comma operator, and a list '(a, b)' is "
                                     "not one value here - write a typed value ':T = (a, b)', "
                                     "or each expression as its own statement"}
                       : "unexpected text '" + word + "' after an expression - is an operator "
                         "or ';' missing?");
    return true;
}

auto token_cursor::check_cpp1_auto(const type_id& type, std::string_view name,
                                   std::string_view replacement) -> void {
    if (!type.is_cpp1_auto()) return;
    add_diagnostic("cpp1-auto-type", diagnostic_severity::error, type.span,
                   "Cpp2 writes deduced types as '" + std::string{replacement} +
                       "'; remove the explicit 'auto' on '" + std::string{name} + "'");
}

auto token_cursor::check_anonymous_object(std::string_view name, const type_id& type,
                                          source_span name_span) -> void {
    if (name != "_" || !type.core().is_wildcard()) return;
    add_diagnostic("anonymous-object", diagnostic_severity::error, name_span,
                   "an object can have an anonymous name or an anonymous type, but not both "
                   "at the same time (rationale: if '_ := f();' were allowed to keep the "
                   "returned object alive, that syntax would be dangerously close to "
                   "'_ = f();' to discard the returned object, and such importantly opposite "
                   "meanings deserve more than a one-character typo distance; and explicit "
                   "discarding gets the nice syntax because it's likely more common)");
}

auto token_cursor::diagnose_block_initializer(std::size_t open, source_span name)
    -> std::optional<std::size_t> {
    if (open >= token_count() || token_at(open).text != "{") return std::nullopt;
    const auto close = bracket_close(*tokens_, open, token_count());
    if (!close) return std::nullopt;
    for (auto position = open + 1; position < *close; ++position) {
        if (token_at(position).text == ";") {
            add_diagnostic("object-initializer-not-expression", diagnostic_severity::error,
                           name, "an object initializer must be an expression");
            return close;
        }
    }
    return std::nullopt;
}

auto token_cursor::check_in_parameter_type(const parameter& value) -> void {
    if ((value.passing == passing_style::in || value.passing == passing_style::in_ref) &&
        value.type.kind == type_kind::constant) {
        add_diagnostic("const-in-parameter", diagnostic_severity::error, value.type.span,
                       "an 'in' parameter is always const, 'const' isn't needed and isn't "
                       "allowed");
    }
}

auto token_cursor::template_parameters(std::size_t& index,
                                       std::vector<template_parameter>& output) -> bool {
    if (index >= token_count() || token_at(index).text != "<") return false;
    const auto open = index;
    const auto close = angle_close(*tokens_, open, token_count());
    if (!close) {
        add_diagnostic("unterminated-template-parameters", diagnostic_severity::error,
                       token_at(open).span, "expected '>' to close template parameter list");
        return false;
    }
    auto position = open + 1;
    auto parsed = template_parameter{};
    auto kind_begin = std::optional<std::size_t>{};
    auto default_begin = std::optional<std::size_t>{};
    auto default_token = std::optional<std::size_t>{};
    auto name_token = std::optional<std::size_t>{};
    auto parameter_text = [&](std::size_t begin, std::size_t end) {
        auto text = syntax_text(begin, end);
        if (end == *close && token_at(*close).text.size() > 1) {
            text.append(token_at(*close).text.size() - 1, '>');
        }
        return text;
    };
    // The text of [begin, end) as a type, written where its tokens are.
    auto parameter_type = [&](std::size_t begin, std::size_t end) {
        auto result = parse_type(parameter_text(begin, end),
                                 {token_at(begin).span.begin, token_at(end - 1).span.end},
                                 type_options());
        check_type_arguments(result, begin, end);
        return result;
    };
    auto flush = [&] {
        if (parsed.name.empty()) return;
        const auto kind_end = default_token.value_or(position);
        if (kind_begin && kind_end > *kind_begin &&
            !(kind_end == *kind_begin + 1 && token_at(*kind_begin).text == "type")) {
            check_type_tokens(*kind_begin, kind_end);
            parsed.type = parameter_type(*kind_begin, kind_end);
            // `T: A & B` or `T: A | B` (experiment `type-operators`) constrains
            // a type parameter.
            check_type_operators(parsed.type, parsed.type.is_operator());
            if (parsed.type.is_operator()) parsed.constraint = std::exchange(parsed.type, {});
            else check_type_parts(parsed.type, true, true);
        }
        if (default_begin && *default_begin < position) {
            if (parsed.is_type_parameter()) {
                parsed.default_type = parameter_type(*default_begin, position);
                check_type_operators(parsed.default_type, false);
            }
            else {
                parsed.default_value = parsed_expression{parameter_text(*default_begin, position)};
                diagnose_juxtaposition(parsed.default_value, *default_begin, position, false,
                                       true);
            }
        }
        if (name_token && token_at(*name_token).text == "_") parsed.name.clear();
        output.push_back(std::move(parsed));
        parsed = template_parameter{};
        kind_begin.reset();
        default_begin.reset();
        default_token.reset();
        name_token.reset();
    };
    while (position < *close) {
        const auto& current = token_at(position);
        if (current.text == ",") {
            flush();
            ++position;
            continue;
        }
        if (current.text == "=" && !default_begin) {
            default_token = position;
            default_begin = position + 1;
            ++position;
            continue;
        }
        if (current.text == ":" && !default_begin) {
            kind_begin = position + 1;
            ++position;
            continue;
        }
        // Before its `:` or `=`, a parameter is a name and an optional `...`.
        if (!kind_begin && !default_begin) {
            if (!parsed.name.empty() || !is_declaration_name(current)) {
                add_diagnostic("invalid-template-parameter", diagnostic_severity::error,
                               current.span, parsed.name.empty()
                                   ? "expected a template parameter name"
                                   : "expected ':', '=' or ',' after template parameter '" +
                                         parsed.name + "'");
                return false;
            }
            parsed.name = current.text;
            name_token = position;
            if (position + 1 < *close && token_at(position + 1).text == "...") {
                parsed.is_variadic = true;
                ++position;
            }
            ++position;
            continue;
        }
        // A nested bracket or template list; `>>` may also close this list.
        const auto group_close = detail::is_open_bracket(current.text)
            ? bracket_close(*tokens_, position, *close)
            : current.text == "<" ? angle_close(*tokens_, position, *close + 1)
                                  : std::optional<std::size_t>{};
        if (group_close) {
            position = std::min(*group_close + 1, *close);
            continue;
        }
        ++position;
    }
    flush();
    if (output.empty()) {
        add_diagnostic("empty-template-parameter-list", diagnostic_severity::error,
                       token_at(open).span, "template parameter list must not be empty");
        return false;
    }
    index = *close + 1;
    return true;
}

auto token_cursor::contract(std::size_t& index, contract_clause& output,
                            std::vector<function_contract::capture>* captures) -> bool {
    const auto& keyword = token_at(index);
    auto open = index + 1;
    if (token_at(open).text == "<") {
        const auto close = angle_close(*tokens_, open, token_count());
        if (!close || token_at(*close).text != ">") {
            add_diagnostic("invalid-contract", diagnostic_severity::error, keyword.span,
                           "expected '>' after the contract group");
            return false;
        }
        const auto items = top_level_items(*tokens_, open + 1, *close);
        for (const auto& [begin, end] : items) {
            if (begin == items.front().first) {
                output.group = syntax_text(begin, end);
                output.group_begin = token_at(begin).span.begin;
            }
            else {
                output.flags.emplace_back(syntax_text(begin, end), false, false, false,
                                          token_at(begin).span.begin);
            }
        }
        open = *close + 1;
    }
    const auto close = token_at(open).text == "(" ? bracket_close(*tokens_, open, token_count())
                                                  : std::optional<std::size_t>{};
    if (token_at(open).text == "(" && !close) {
        add_diagnostic("unmatched-open-parenthesis", diagnostic_severity::error,
                       token_at(open).span, "expected ')'");
        return false;
    }
    const auto items = close ? top_level_items(*tokens_, open + 1, *close)
                             : std::vector<std::pair<std::size_t, std::size_t>>{};
    if (items.empty() || items.size() > 2) {
        add_diagnostic("invalid-contract", diagnostic_severity::error, keyword.span,
                       "expected '" + keyword.text + "(condition)' or '" + keyword.text +
                           "(condition, message)'");
        return false;
    }
    // The text of [begin, end) with each `$` capture, also one in a string
    // interpolation, replaced by the name of a new capture; nullopt after an error.
    const auto captured_text = [&](std::size_t begin,
                                   std::size_t end) -> std::optional<std::string> {
        auto text = std::string{};
        auto piece = begin;
        const auto add = [&](const std::string& expression, source_position at) {
            auto name = "_" + std::to_string(captures->size());
            captures->push_back({name, parsed_expression{expression, false, false, false, at}});
            return name;
        };
        for (auto position = begin; captures != nullptr && position < end; ++position) {
            const auto& value = token_at(position);
            if (value.kind == token_kind::string_literal) {
                auto literal = replace_interpolated_captures(value.text,
                    [&](const std::string& expression) {
                        return add(expression, value.span.begin);
                    });
                if (literal == value.text) continue;
                const auto before = syntax_text(piece, position + 1);
                text += before.substr(0, before.size() - value.text.size()) + literal;
                piece = position + 1;
                continue;
            }
            if (value.text != "$") continue;
            const auto captured = capture_begin(*tokens_, piece, position);
            if (!captured) {
                add_diagnostic("invalid-capture", diagnostic_severity::error, value.span,
                               "a '$' capture in a contract must follow an expression in a "
                               "postcondition");
                return std::nullopt;
            }
            if (*captured > piece) {
                text += syntax_text(piece, *captured) +
                    std::string(token_at(*captured).span.begin.column >
                                token_at(*captured - 1).span.end.column ? " " : "");
            }
            text += add(syntax_text(*captured, position), token_at(*captured).span.begin);
            piece = position + 1;
            if (piece < end && token_at(piece).span.begin.column > value.span.end.column) {
                text += " ";
            }
        }
        return text + syntax_text(piece, end);
    };
    // A text with captures is rewritten, so its tokens are not where it begins.
    const auto rewritten = [&](std::size_t begin,
                               std::size_t end) -> std::optional<parsed_expression> {
        const auto text = captured_text(begin, end);
        if (!text) return std::nullopt;
        const auto unchanged = *text == syntax_text(begin, end);
        auto result = parsed_expression{*text, false, false, false,
            unchanged ? std::optional{token_at(begin).span.begin} : std::nullopt};
        diagnose_juxtaposition(result, begin, end);
        return result;
    };
    auto condition = rewritten(items.front().first, items.front().second);
    if (!condition) return false;
    output.condition = *std::move(condition);
    if (items.size() == 2) {
        auto message = rewritten(items[1].first, items[1].second);
        if (!message) return false;
        output.message = *std::move(message);
    }
    index = *close + 1;
    return true;
}

auto token_cursor::using_declaration(std::size_t& index, std::string& target) const
    -> using_parse_result {
    if (index >= tokens_->size() || (*tokens_)[index].kind != token_kind::keyword ||
        (*tokens_)[index].text != "using") {
        return using_parse_result::not_using;
    }
    ++index;
    if (index >= tokens_->size()) return using_parse_result::invalid;
    if ((*tokens_)[index].text == "namespace") return using_parse_result::invalid;
    auto result = std::string{};
    if (index < tokens_->size() && (*tokens_)[index].text == "::") {
        result = "::";
        ++index;
    }
    auto append_component = [&]() -> bool {
        if (index >= tokens_->size()) return false;
        const auto& value = (*tokens_)[index];
        if (value.kind != token_kind::identifier && value.kind != token_kind::keyword && value.text != "_") {
            return false;
        }
        result += value.text;
        ++index;
        return true;
    };
    if (!append_component()) return using_parse_result::invalid;
    while (index < tokens_->size() && (*tokens_)[index].text == "::") {
        result += "::";
        ++index;
        if (!append_component()) return using_parse_result::invalid;
    }
    if (result.empty() || index >= tokens_->size() || (*tokens_)[index].text != ";") {
        return using_parse_result::invalid;
    }
    ++index;
    target = std::move(result);
    return using_parse_result::valid;
}

auto token_cursor::diagnose_invalid_using(std::size_t index) -> void {
    const auto next = std::min(index + 1, tokens_->size() - 1);
    const auto& value = (*tokens_)[next];
    if (value.text == "namespace") {
        add_diagnostic("invalid-using-declaration", diagnostic_severity::error,
                       (*tokens_)[index].span,
                       "in Cpp2, write 'using the_namespace_name::_' to bring all names in the "
                       "namespace into scope using the '_' wildcard");
    }
    else if (value.kind != token_kind::identifier && value.kind != token_kind::keyword &&
             value.text != "::" && value.text != "_") {
        add_diagnostic("invalid-using-declaration", diagnostic_severity::error, value.span,
                       "expected valid id-expression after 'using'");
    }
    else {
        add_diagnostic("invalid-using-declaration", diagnostic_severity::error,
                       (*tokens_)[index].span,
                       "expected a qualified name or a namespace '::_' wildcard after 'using'");
    }
}

auto token_cursor::unsupported_expression(std::size_t begin, std::size_t end) const
    -> std::optional<source_span> {
    end = std::min(end, tokens_->size());
    for (auto position = begin; position < end; ++position) {
        const auto& current = (*tokens_)[position];
        if (current.text != ":") continue;
        if (position + 1 >= end) continue;
        const auto& next = (*tokens_)[position + 1];
        if (position > begin) {
            const auto& previous = (*tokens_)[position - 1];
            if (previous.text != "(" && previous.text != "[" && previous.text != "{" &&
                previous.text != "," && previous.text != "=" && previous.text != "?" &&
                previous.text != "return" && previous.text != ":=") {
                continue;
            }
        }
        if (next.text == "(" || next.text == "<" || next.text == ":") return current.span;
    }
    return std::nullopt;
}

auto token_cursor::add_diagnostic(std::string_view code,
                                  diagnostic_severity severity,
                                  source_span span,
                                  std::string_view message) -> void {
    diagnostics_->push_back({std::string{code}, severity, span, std::string{message}});
}

trace::trace(std::string_view name, trace_sink sink)
    : enabled_{sink.kind == trace_sink_kind::stderr_stream} {
    if (enabled_) std::cerr << "parse " << name << '\n';
}

auto trace::enabled() const noexcept -> bool {
    return enabled_;
}

auto trace::event(std::string_view message) const -> void {
    if (!enabled_) return;
    std::cerr << message << '\n';
}

}
