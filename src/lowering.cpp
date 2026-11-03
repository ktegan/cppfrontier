#include <cppfrontier/include_policy.hpp>
#include <cppfrontier/pipeline.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include "ir_walk.hpp"
#include "lowering_detail.hpp"
#include "autodiff_metafunction.hpp"
#include "cpp1_expression_renderer.hpp"
#include "cpp1_text.hpp"
#include "generated_members.hpp"
#include "mentions.hpp"

namespace cppfrontier {

using lowering_detail::line_text;

// The function body `output` that keeps statements on their source lines, the
// line at its end, and where the function begins.
struct line_anchor final {
    const line_text* output = nullptr;
    lowering_detail::line_counter lines;
    source_position function;
};

// The options for the expressions at the current point, with the state of the
// function being rendered.
struct lowering_options final : cpp1_expression_options {
    const initialization_plan* plan = nullptr;
    // The last uses, by text, in a function with a `move that` parameter,
    // which include those of `that`; see `initialization_plan`.
    const last_use_map* that_last_uses = nullptr;
    // Keeps statements on their source lines.
    line_anchor* anchor = nullptr;
    // What a bare `return;` returns in a function with named return values.
    std::string named_return_value;
    // A named-value return first runs the postconditions.
    bool run_postconditions = false;
    // Experiment `defer`: the names that the defers registered so far in the
    // enclosing blocks mention.
    std::vector<std::string> deferred_names;
    // The function has one typed return, so a returned expression list such
    // as `(1, 2)` is list-initialized.
    bool braced_return_list = false;
    // `[captures]<T>(params) mutable -> R`, which replaces the signature of the
    // function declaration standing in for a function expression.
    std::string lambda_head;
    // The constexpr member functions defined after all types.
    const std::set<const ir_declaration*>* members_defined_late = nullptr;
};

namespace {

auto ends_with_namespace_wildcard(std::string_view name) -> bool {
    return name.ends_with("::_");
}

auto render_using(std::string_view name) -> std::string {
    const auto namespace_wildcard = ends_with_namespace_wildcard(name);
    auto target = std::string{name};
    if (namespace_wildcard) target.resize(target.size() - 3);
    return "using " + (namespace_wildcard ? std::string{"namespace "} : std::string{}) +
        target + ";";
}

auto render_name(std::string_view name) -> std::string {
    return cpp1_identifier(name);
}

// A local named `_` gets a name unique by its position, as a namespace-scope
// object or data member does in the IR.
auto render_object_name(std::string_view name, source_position position) -> std::string {
    return name == "_" ? anonymous_object_name(position) : render_name(name);
}

auto render_parameter_name(const parameter& value, std::size_t ordinal) -> std::string {
    if (value.name == "_") return "unnamed_param_" + std::to_string(ordinal);
    return render_name(value.name);
}

// A Cpp1 template parameter `kind name = default_argument`.
struct cpp1_template_parameter final {
    std::string kind;
    // A non-type parameter's type.
    const type_id* value_type = nullptr;
    std::string name;
    std::string default_argument;
    bool is_variadic = false;
};

// A type alias's target is a type position.
auto alias_type_options(const cpp1_expression_options& options) -> type_parse_options {
    return {.type_operators = options.experiments.enabled("type-operators")};
}

auto render_default_argument(const parsed_expression& text) -> std::string;

auto render_template_parameter(const template_parameter& parameter) -> cpp1_template_parameter {
    auto result = cpp1_template_parameter{};
    result.kind = parameter.is_type_parameter() ? "typename" : render_cpp1_type(parameter.type);
    result.name = render_name(parameter.name);
    if (!parameter.is_type_parameter()) result.value_type = &parameter.type;
    if (!parameter.default_type.empty()) {
        result.default_argument = render_cpp1_type(parameter.default_type);
    }
    else if (!parameter.default_value.empty()) {
        result.default_argument = render_default_argument(parameter.default_value);
    }
    result.is_variadic = parameter.is_variadic;
    return result;
}

// `<kind name, ...>`; `suffix` keeps unnamed parameters of a member template
// distinct from those of its type.
auto render_template_parameter_list(const std::vector<cpp1_template_parameter>& parameters,
                                    std::string_view suffix = {}) -> std::string {
    auto result = std::string{"<"};
    for (auto index = std::size_t{0}; index < parameters.size(); ++index) {
        const auto& parameter = parameters[index];
        if (index != 0) result += ", ";
        auto name = parameter.name;
        if (name.empty()) name = "UnnamedTemplateParam" + std::to_string(index + 1) + std::string{suffix};
        // A function pointer declarator encloses the name: `int(*F)(int)`.
        if (parameter.value_type != nullptr &&
            !render_cpp1_type_declaration(*parameter.value_type, name).ends_with(name)) {
            result += render_cpp1_type_declaration(*parameter.value_type,
                                                   (parameter.is_variadic ? "..." : "") + name);
        }
        else {
            result += parameter.kind + (parameter.is_variadic ? " ..." : " ") + name;
        }
        if (!parameter.default_argument.empty()) result += " = " + parameter.default_argument;
    }
    return result + ">";
}

auto render_template_parameters(const std::vector<cpp1_template_parameter>& parameters,
                                std::string_view suffix = {}) -> std::string {
    if (parameters.empty()) return {};
    return "template" + render_template_parameter_list(parameters, suffix) + " ";
}

auto render_template_parameters(const std::vector<template_parameter>& parameters)
    -> std::string {
    auto rendered = std::vector<cpp1_template_parameter>{};
    for (const auto& parameter : parameters) rendered.push_back(render_template_parameter(parameter));
    return render_template_parameters(rendered);
}

// The template arguments `<T, Ts...>` naming a declaration's own template parameters.
auto render_template_arguments(const ir_declaration& type) -> std::string {
    if (type.template_parameters.empty()) return {};
    const auto suffix = type.scope == declaration_scope::type_scope ? "_" + type.name : "";
    auto result = std::string{"<"};
    for (auto index = std::size_t{0}; index < type.template_parameters.size(); ++index) {
        const auto& parameter = type.template_parameters[index];
        if (index != 0) result += ", ";
        auto name = render_name(parameter.name);
        if (name.empty()) name = "UnnamedTemplateParam" + std::to_string(index + 1) + suffix;
        result += name + (parameter.is_variadic ? "..." : "");
    }
    return result + ">";
}

auto parameter_unused_prefix(const parameter& value) -> std::string_view {
    return value.name == "_" ? "[[maybe_unused]] " : "";
}

// Adds the identifiers in `tokens` to `names`, except member names as in `x.name`.
auto add_identifiers(const std::vector<expression_token>& tokens, std::set<std::string>& names)
    -> void {
    for (auto index = std::size_t{0}; index < tokens.size(); ++index) {
        if (tokens[index].kind != expression_token_kind::identifier) continue;
        if (index > 0 && (tokens[index - 1].text == "." || tokens[index - 1].text == "..")) continue;
        names.insert(tokens[index].text);
    }
}

// Adds the names in `type` to `names`, including each part of a qualified name.
auto add_type_names(const type_id& type, std::set<std::string>& names) -> void {
    switch (type.kind) {
    case type_kind::name:
        for (const auto& component : type.components) {
            names.insert(component.identifier);
            for (const auto& argument : component.arguments) add_type_names(argument, names);
        }
        break;
    case type_kind::decltype_of:
    case type_kind::expression:
    case type_kind::cpp1:
        add_identifiers(parse_expression_text(type.text).tokens, names);
        break;
    default:
        for (const auto& child : type.children) add_type_names(child, names);
    }
}

// The names that the declaration of `function` uses: in its template
// parameters, parameter and return types, default arguments and `requires` clause.
auto signature_names(const declaration_signature& function) -> std::set<std::string> {
    auto names = std::set<std::string>{};
    for (const auto& parameter : function.template_parameters) {
        add_type_names(parameter.type, names);
        add_type_names(parameter.default_type, names);
        add_type_names(parameter.constraint, names);
        add_identifiers(parameter.default_value.tree().tokens, names);
    }
    for (const auto& parameter : function.parameters) {
        add_type_names(parameter.type, names);
        add_identifiers(parameter.default_value.tree().tokens, names);
    }
    for (const auto& named : function.named_returns) add_type_names(named.value.type, names);
    add_type_names(function.type, names);
    add_identifiers(function.requires_clause.tree().tokens, names);
    return names;
}

// The name that a using-declaration `using a::b;` declares, `b`.
auto using_declared_name(const ir_declaration& declaration) -> std::string {
    const auto& name = declaration.using_name;
    const auto separator = name.rfind("::");
    return separator == std::string::npos ? name : name.substr(separator + 2);
}

// The names that the in-class declaration of `member` uses: a function's
// signature, an object's type, an alias's value, and those of a nested type's members.
auto member_declaration_names(const ir_declaration& member) -> std::set<std::string> {
    auto names = std::set<std::string>{};
    if (member.kind == declaration_kind::function) return signature_names(member);
    if (member.kind == declaration_kind::object) add_type_names(member.type, names);
    if (!member.alias.empty()) add_identifiers(parse_expression_text(member.alias).tokens, names);
    if (member.kind == declaration_kind::type) {
        for (const auto& nested : member.members) names.merge(member_declaration_names(nested));
    }
    return names;
}

// The first name of each type that an object of `type` holds whole: not
// through a pointer or a template argument, so a type that needs it complete.
auto add_held_type_names(const type_id& type, std::set<std::string>& names) -> void {
    if (type.kind == type_kind::name) names.insert(type.components.front().identifier);
    if (type.kind == type_kind::constant || type.kind == type_kind::union_type) {
        for (const auto& child : type.children) add_held_type_names(child, names);
    }
}

// The names of the types that `member` needs complete where it is defined:
// an object's type, and those of a nested type's objects and bases.
auto complete_type_names(const ir_declaration& member) -> std::set<std::string> {
    auto names = std::set<std::string>{};
    if (member.kind == declaration_kind::object && !member.is_lazy) {
        add_held_type_names(member.type, names);
    }
    if (member.kind == declaration_kind::type && member.alias.empty()) {
        for (const auto& nested : member.members) names.merge(complete_type_names(nested));
    }
    return names;
}

// A type naming any template parameter, including a non-type one such as
// `N` in `std::array<T, N>`: `cpp2::impl::in<...>` would make it non-deducible.
auto is_dependent_parameter_type(const ir_declaration& declaration, const type_id& type)
    -> bool {
    if (type.core().has_type_marker) return true;
    return std::any_of(declaration.template_parameters.begin(),
                       declaration.template_parameters.end(), [&](const auto& parameter) {
                           return !parameter.name.empty() && mentions_name(type, parameter.name);
                       });
}

auto is_member_that(const ir_declaration& declaration, std::size_t index) -> bool {
    return declaration.scope == declaration_scope::type_scope && index == 1 &&
        that_parameter(declaration) != nullptr;
}

auto is_empty_initializer(std::string_view text) -> bool {
    auto first = std::size_t{0};
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first]))) ++first;
    auto last = text.size();
    while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1]))) --last;
    if (last - first < 2) return false;
    const auto opening = text[first];
    const auto closing = text[last - 1];
    if (!((opening == '(' && closing == ')') || (opening == '{' && closing == '}'))) {
        return false;
    }
    for (auto position = first + 1; position < last - 1; ++position) {
        if (!std::isspace(static_cast<unsigned char>(text[position]))) return false;
    }
    return true;
}

// The expression `parsed` holds when it is one, else its root sequence.
auto single_expression(const expression_text& parsed) -> const expression& {
    return parsed.root.kind == expression_kind::sequence && parsed.root.children.size() == 1
        ? parsed.root.children.front() : parsed.root;
}

// Whether `text` is one parenthesized expression list, such as `(1, 2)`.
auto is_expression_list(const parsed_expression& text) -> bool {
    const auto& root = single_expression(text.tree());
    return root.kind == expression_kind::group && root.first == 0 &&
        root.last == text.tree().tokens.size();
}

auto render_object_initializer(const parsed_expression& text,
                               cpp1_expression_options expression_options) -> std::string {
    if (text.empty() || is_empty_initializer(text.source())) return "{}";
    if (is_expression_list(text)) {
        const auto rendered = render_cpp1_expression(text, expression_options);
        return "{" + (rendered.size() >= 2 && rendered.front() == '(' &&
                      rendered.back() == ')'
            ? rendered.substr(1, rendered.size() - 2) : rendered) + "}";
    }
    const auto rendered = render_cpp1_expression(text, expression_options);
    if (rendered.size() >= 2 && rendered.front() == '{' && rendered.back() == '}') {
        return rendered;
    }
    return "{" + rendered + "}";
}

auto render_object_initializer(std::string_view text,
                               cpp1_expression_options expression_options) -> std::string {
    return render_object_initializer(parsed_expression{std::string{text}}, expression_options);
}

// Whether `text` is a list `(a, b)` of two or more elements; `(x)` and `(x,)`
// are grouping. Cpp2 has no comma operator, so a list is list-initialized.
auto is_list_expression(const parsed_expression& text) -> bool {
    const auto& root = single_expression(text.tree());
    return root.kind == expression_kind::group && root.children.size() > 1 &&
        root.first == 0 && root.last == text.tree().tokens.size();
}

// Whether `text` is list-initialized where a braced list is expected: `()`, or
// a parenthesized expression list.
auto is_braced_initializer(const parsed_expression& text) -> bool {
    return is_empty_initializer(text.source()) || is_expression_list(text);
}

// `= 5` stays an expression; `= (1, 2)` and `= ()` are list-initialized.
auto render_default_argument(const parsed_expression& text) -> std::string {
    auto options = cpp1_expression_options{};
    options.nonlocal = true;
    if (is_braced_initializer(text)) return render_object_initializer(text, options);
    return render_cpp1_expression(text, options);
}

// Options for an initializer or alias outside a function body.
auto nonlocal_options(cpp1_expression_options options) -> cpp1_expression_options {
    options.nonlocal = true;
    return options;
}

// `wrapped` holds the deferred locals and `out` parameters the clause may
// name in `decltype`.
auto render_requires_clause(
    const parsed_expression& text,
    const std::vector<cpp1_expression_options::wrapped_parameter>& wrapped = {}) -> std::string {
    auto options = nonlocal_options({});
    options.wrapped_parameters = wrapped;
    return render_cpp1_expression(text, options);
}

// A loop's rendered `next` clause, where `_ = x` discards `x`.
auto discard_next_clause(std::string next) -> std::string {
    if (!next.starts_with("_ =") || next.starts_with("_ ==")) return next;
    const auto semicolon = next.ends_with(';');
    if (semicolon) next.pop_back();
    const auto begin = next.find_first_not_of(' ', 3);
    return "static_cast<void>(" + next.substr(begin == std::string::npos ? next.size() : begin) +
        ")" + (semicolon ? ";" : "");
}

// A loop's rendered `next` clause, one expression without a `;`, or with
// experiment `next-list` each item of a list `(a, b)`.
auto render_next_items(const parsed_expression& next, const cpp1_expression_options& options)
    -> std::vector<std::string> {
    auto items = options.experiments.enabled("next-list")
        ? render_cpp1_expression_items(next, options)
        : std::vector{render_cpp1_expression(next, options)};
    for (auto& item : items) {
        item = discard_next_clause(std::move(item));
        while (!item.empty() && (item.back() == ';' ||
               std::isspace(static_cast<unsigned char>(item.back())))) {
            item.pop_back();
        }
    }
    return items;
}

// The `next` items as statements on one line.
auto next_statements(const std::vector<std::string>& items) -> std::string {
    auto result = std::string{};
    for (const auto& item : items) result += item + "; ";
    return result;
}

// The `next` items as one expression; a list runs in a lambda, one statement
// per item, so they run in order.
auto next_expression(const std::vector<std::string>& items) -> std::string {
    return items.size() == 1 ? items.front() : "[&]{ " + next_statements(items) + "}()";
}

// `auto`, or `C auto` for a deduced type constrained by `is C`; Cpp1 has no
// `A & B auto`, so an operator constraint is asserted by the caller.
auto render_deduced_type(const type_id& type) -> std::string {
    const auto* constraint = type.constraint();
    if (constraint == nullptr || constraint->is_operator()) return "auto";
    return render_cpp1_type(*constraint) + " auto";
}

// Whether `type` is deduced, as `_` or with `_` below `*` and `const`.
auto has_deduced_type(const type_id& type) -> bool { return type.core().is_wildcard(); }

// `static_assert(...);` for an operator constraint on the deduced type of
// `name`, which `render_deduced_type` leaves out; empty otherwise.
auto deduced_constraint_assertion(const type_id& type, const std::string& name) -> std::string {
    const auto* constraint = type.constraint();
    if (!type.is_wildcard() || constraint == nullptr || !constraint->is_operator()) return {};
    return "static_assert(" + render_cpp1_constraint(*constraint, "CPP2_TYPEOF(" + name + ")") +
        ");";
}

auto render_range_parameter(const parameter& value) -> std::string {
    auto type = value.type.is_wildcard() ? render_deduced_type(value.type) :
        render_cpp1_type(value.type);
    if (value.passing == passing_style::in && !has_deduced_type(value.type)) {
        return std::string{parameter_unused_prefix(value)} + "cpp2::impl::in<" + type + "> " +
            render_parameter_name(value, 1);
    }
    auto qualifier = std::string_view{};
    switch (value.passing) {
    case passing_style::in:
    case passing_style::in_ref: qualifier = " const&"; break;
    case passing_style::copy: break;
    case passing_style::inout:
    case passing_style::out: qualifier = "&"; break;
    case passing_style::move:
    case passing_style::forward: qualifier = "&&"; break;
    }
    return std::string{parameter_unused_prefix(value)} + type + std::string{qualifier} + " " +
        render_parameter_name(value, 1);
}

auto render_catch_parameter(const parameter& value) -> std::string {
    if (value.name == "_" && value.type.is_wildcard()) return "...";
    auto type = value.type.is_wildcard() ? std::string{"auto"} :
        render_cpp1_type(value.type);
    if (value.passing == passing_style::in && !value.type.is_wildcard()) {
        type = "cpp2::impl::in<" + type + ">";
    }
    else if (value.passing == passing_style::in_ref) {
        type += " const&";
    }
    else if (value.passing == passing_style::inout || value.passing == passing_style::out) {
        type += "&";
    }
    else if (value.passing == passing_style::move || value.passing == passing_style::forward) {
        type += "&&";
    }
    return type + " " + render_parameter_name(value, 1);
}

struct loop_context final {
    bool wraps_next = false;
    std::string break_flag;
};

// The definite last uses in `text`.
auto last_uses_in(const lowering_options& options, const text_key& text)
    -> const std::vector<last_use>* {
    if (options.that_last_uses != nullptr) {
        if (const auto found = options.that_last_uses->find(text);
            found != options.that_last_uses->end()) {
            return &found->second;
        }
    }
    return options.plan == nullptr ? nullptr : options.plan->last_uses_in(text);
}

// Options for rendering `text`, whose definite last uses become rvalues.
auto text_options(const lowering_options& options, const text_key& text)
    -> cpp1_expression_options {
    auto result = cpp1_expression_options{options};
    result.last_uses = last_uses_in(options, text);
    return result;
}

// Options for rendering the text of the expression statement `value`, whose
// assignments construct the deferred locals the safety analysis recorded for it.
auto statement_text_options(const lowering_options& options, const statement& value)
    -> cpp1_expression_options {
    auto result = text_options(options, statement_key(value));
    if (options.plan != nullptr) result.constructions = options.plan->constructions_in(value.id);
    return result;
}

// A range list `(a, b)` iterates the list `{ a, b }`.
auto render_range(const parsed_expression& range, cpp1_expression_options options)
    -> std::string {
    const auto rendered = render_cpp1_expression(range, options);
    if (!is_list_expression(range)) return rendered;
    return "{ " + rendered.substr(1, rendered.size() - 2) + " }";
}

// Options for text at `point`, where it is known.
auto options_at(lowering_options options, scope_point point) -> lowering_options {
    if (point.valid()) options.point = point;
    return options;
}

auto render_statements(const std::vector<std::unique_ptr<statement>>& statements,
                       line_text& output, int depth, lowering_options& expression_options,
                       const loop_context* loop = nullptr) -> void;

auto body_breaks_loop(const statement_list& body, std::string_view loop_label) -> bool;

auto breaks_loop(const statement& statement, std::string_view loop_label) -> bool {
    if (statement.kind == statement_kind::break_statement) {
        const auto& label = statement.as<statement::jump_form>().label;
        return label.empty() || loop_label.empty() || label == loop_label;
    }
    if (statement.is<statement::loop_form>()) return false;
    auto breaks = false;
    for_each_nested_list(statement, [&](const auto& nested) {
        breaks = breaks || body_breaks_loop(nested, loop_label);
    });
    return breaks;
}

auto body_breaks_loop(const std::vector<std::unique_ptr<statement>>& body,
                      std::string_view loop_label) -> bool {
    return std::any_of(body.begin(), body.end(), [&](const auto& statement) {
        return statement && breaks_loop(*statement, loop_label);
    });
}

auto render_labeled_iteration(std::string_view label,
                              const std::vector<std::unique_ptr<statement>>& body,
                              line_text& output, int depth,
                              lowering_options& expression_options,
                              const loop_context* loop = nullptr) -> void {
    if (label.empty()) {
        render_statements(body, output, depth, expression_options, loop);
        return;
    }
    // The body is a block of its own, so a `goto` to the labels after it does not
    // cross the initialization of a local.
    const auto indent = std::string(depth * 4, ' ');
    output += indent + "{\n";
    render_statements(body, output, depth + 1, expression_options, loop);
    output += indent + "}\n";
    output += indent + "CPP2_CONTINUE_BREAK(" + std::string{label} + ")\n";
}

// Places the next line of `output`, which `anchor` describes, on source line
// `line`: with newlines while that is ahead, else with a `#line`. A line that
// continues an output line stays there.
auto place_on_line(line_anchor& anchor, line_text& output, std::uint32_t line) -> void {
    if (line == 0 || !output.ends_with_newline()) return;
    const auto current = anchor.lines.at_end(output);
    if (line > current) output += std::string(line - current, '\n');
    else if (line < current) output.begin_line(line);
}

// Places the next line of `output` on the source line of `statement`, or on
// `line` if given, if `output` is the function body that `options` keeps on
// its source lines. A generated statement, at a generated column or where its
// function begins, has no source line of its own.
auto place_on_line(const lowering_options& options, line_text& output,
                   const statement& statement, std::optional<std::uint32_t> line = {})
    -> void {
    auto* anchor = options.anchor;
    const auto begin = statement.span.begin;
    if (anchor == nullptr || anchor->output != &output || begin.column >= generated_column ||
        (begin.line == anchor->function.line && begin.column == anchor->function.column)) {
        return;
    }
    place_on_line(*anchor, output, line.value_or(begin.line));
}

// `if (flags && group.is_active() && !(condition) ) { group.report_violation(message); }`,
// with the last uses of the contract statement `source`, if any.
auto render_contract_check(const statement::contract_form& form,
                           const lowering_options& expression_options,
                           const statement* source = nullptr) -> std::string {
    // A function's contracts have no last uses.
    const auto options = [&](text_key::part which, std::size_t index = 0) {
        auto result = cpp1_expression_options{expression_options};
        result.last_uses = source == nullptr ? nullptr
            : last_uses_in(expression_options, statement_key(*source, which, index));
        return result;
    };
    auto group = form.group.empty() || form.group == "_" || form.group == "default"
        ? std::string{"cpp2_default"} : form.group;
    static constexpr auto predefined = std::array<std::string_view, 5>{
        "cpp2_default", "bounds_safety", "null_safety", "type_safety", "testing"};
    if (std::find(predefined.begin(), predefined.end(), group) != predefined.end()) {
        group = "cpp2::" + group;
    }
    else {
        // A group may be an expression, such as `pre<this>` or `assert<group*>`.
        auto group_options = cpp1_expression_options{expression_options};
        group_options.last_uses = nullptr;
        group = render_cpp1_expression(group, group_options);
        // An operand of `.`: a name, or parenthesized.
        const auto parenthesized = [&] {
            if (group.front() != '(' || group.back() != ')') return false;
            auto depth = 0;
            for (auto index = std::size_t{0}; index + 1 < group.size(); ++index) {
                if (group[index] == '(') ++depth;
                else if (group[index] == ')' && --depth == 0) return false;
            }
            return true;
        };
        if (group.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                    "0123456789_:") != std::string::npos &&
            !parenthesized()) {
            group = "(" + group + ")";
        }
    }
    auto condition = std::string{};
    for (auto index = std::size_t{0}; index < form.flags.size(); ++index) {
        condition += render_cpp1_expression(form.flags[index],
                                            options(text_key::part::flag, index)) + " && ";
    }
    const auto message = form.message.empty() ? std::string{"\"\""}
        : "CPP2_CONTRACT_MSG(" + render_cpp1_expression(form.message,
              options(text_key::part::message)) + ")";
    return "if (" + condition + group + ".is_active() && !(" +
        render_cpp1_expression(form.condition, options(text_key::part::condition)) +
        ") ) { " + group +
        ".report_violation(" + message + "); }";
}

auto has_postconditions(const ir_declaration& declaration) -> bool {
    return std::any_of(declaration.contracts.begin(), declaration.contracts.end(),
        [](const auto& contract) { return contract.is_post && contract.group != "unevaluated"; });
}

// Experiment `defer`: whether `statements` hold a defer statement.
auto has_defer(const statement_list& statements) -> bool {
    return std::any_of(statements.begin(), statements.end(), [](const auto& value) {
        auto found = value->kind == statement_kind::defer_statement;
        for_each_nested_list(*value, [&](const statement_list& nested) {
            found = found || has_defer(nested);
        });
        return found;
    });
}

auto add_mentioned_names(const expression_text& text, std::vector<std::string>& names) -> void {
    for (const auto& token : text.tokens) {
        if (token.kind == expression_token_kind::identifier) names.push_back(token.text);
        if (token.kind != expression_token_kind::string) continue;
        for (const auto& nested : interpolated_expressions(token.text)) {
            add_mentioned_names(parse_expression_text(nested), names);
        }
    }
}

// Adds the names that the texts of `statements` mention to `names`.
auto add_mentioned_names(const statement_list& statements, std::vector<std::string>& names)
    -> void {
    for (const auto& value : statements) {
        std::visit([&](const auto& form) {
            using form_type = std::remove_cvref_t<decltype(form)>;
            if constexpr (std::is_same_v<form_type, statement::expression_form> ||
                          std::is_same_v<form_type, statement::declaration_form>) {
                add_mentioned_names(form.text.tree(), names);
            }
            else if constexpr (std::is_same_v<form_type, statement::destructuring_form>) {
                add_mentioned_names(form.initializer.tree(), names);
            }
            else if constexpr (std::is_same_v<form_type, statement::block_form>) {
                for (const auto& parameter : form.parameters) {
                    add_mentioned_names(parameter.initializer.tree(), names);
                }
            }
            else if constexpr (std::is_same_v<form_type, statement::if_form>) {
                add_mentioned_names(form.condition.tree(), names);
            }
            else if constexpr (std::is_same_v<form_type, statement::loop_form>) {
                add_mentioned_names(form.condition.tree(), names);
                add_mentioned_names(form.next.tree(), names);
            }
            else if constexpr (std::is_same_v<form_type, statement::contract_form>) {
                add_mentioned_names(form.condition.tree(), names);
                add_mentioned_names(form.message.tree(), names);
                for (const auto& flag : form.flags) add_mentioned_names(flag.tree(), names);
            }
        }, value->form);
        for_each_nested_list(*value, [&](const statement_list& nested) {
            add_mentioned_names(nested, names);
        });
    }
}

// The name that `return name;` returns, or empty.
auto returned_name(const parsed_expression& text) -> std::string_view {
    const auto& tokens = text.tree().tokens;
    const auto count = !tokens.empty() && tokens.back().text == ";" ? tokens.size() - 1
                                                                    : tokens.size();
    return count == 1 && tokens.front().kind == expression_token_kind::identifier
        ? std::string_view{tokens.front().text} : std::string_view{};
}

// For a returned text that lowering must check (`initialization_plan::value_returns`),
// a `static_assert` that its lowered `value` is not a reference, which the
// deduced return type would return; otherwise empty.
auto value_return_check(const lowering_options& options, const text_key& key,
                        const std::string& value) -> std::string {
    if (options.plan == nullptr || !options.plan->value_returns.contains(key)) return {};
    return "static_assert(!std::is_reference_v<decltype(" + value + ")>, \"" +
        std::string{forward_return_message} + "\"); ";
}

// The guard that runs a function's postconditions on successful return.
auto postcondition_guard(const ir_declaration& declaration) -> std::string {
    return has_postconditions(declaration)
        ? "    cpp2::finally_presuccess cpp2_finally_presuccess;\n" : "";
}

// The name a capture declares: `..._0` for one of an unexpanded parameter
// pack, as `xs$...`, which is a pack itself.
auto capture_name(const auto& capture, const lowering_options& options) -> std::string {
    auto pack = false;
    if (options.scopes != nullptr) {
        for_each_mention(capture.expression.tree(), [&](const mention& value) {
            const auto* entry = options.scopes->lookup(value.name(), options.point);
            pack = pack || (!value.expanded && !value.compile_time && entry != nullptr &&
                            entry->declared_parameter != nullptr &&
                            entry->declared_parameter->is_variadic);
        });
    }
    return (pack ? "..." : "") + capture.name;
}

// A precondition check, or a postcondition check registered to run on
// successful return, with its `$` captures taken where registered.
auto render_function_contract(const function_contract& contract,
                              const lowering_options& expression_options) -> std::string {
    const auto check = render_contract_check(contract, expression_options);
    if (!contract.is_post) return "    " + check + "\n";
    auto captures = std::string{"&"};
    for (const auto& capture : contract.captures) {
        captures += ", " + capture_name(capture, expression_options) + " = " +
            render_cpp1_expression(capture.expression, expression_options);
    }
    return "    cpp2_finally_presuccess.add([" + captures + "]{" + check + "} );\n";
}

// A function's evaluated preconditions (`post` false) or postconditions.
auto function_contracts(const ir_declaration& declaration, bool post) {
    auto result = std::vector<const function_contract*>{};
    for (const auto& contract : declaration.contracts) {
        if (contract.group != "unevaluated" && contract.is_post == post) {
            result.push_back(&contract);
        }
    }
    return result;
}

auto render_function_contracts(const ir_declaration& declaration, bool post,
                               const lowering_options& expression_options) -> std::string {
    auto result = std::string{};
    for (const auto* contract : function_contracts(declaration, post)) {
        result += render_function_contract(*contract, expression_options);
    }
    return result;
}

auto render_statement_text(const statement& statement, line_text& output, int depth,
                           lowering_options& expression_options,
                           const loop_context* loop) -> void;

auto render_statement(const statement& statement, line_text& output, int depth,
                      lowering_options& expression_options,
                      const loop_context* loop = nullptr) -> void {
    place_on_line(expression_options, output, statement);
    render_statement_text(statement, output, depth, expression_options, loop);
}

// The hoisted statements of the `?` operators rendered so far, taken, as one
// line; see `try_hoisting`.
auto take_try_hoists(const cpp1_expression_options& options) -> std::string {
    auto result = std::string{};
    if (options.try_hoists == nullptr) return result;
    for (const auto& hoisted : options.try_hoists->statements) result += hoisted + " ";
    options.try_hoists->statements.clear();
    return result;
}

// A suffix naming the generated locals of `statement` uniquely.
auto generated_suffix(const statement& statement) -> std::string {
    return std::to_string(statement.span.begin.line) + "_" +
        std::to_string(statement.span.begin.column);
}

// An `if` whose condition binds (experiment `condition-bindings`): a nested
// `if` for each pattern test and each run of other operands, and with an
// `else`, a flag that the body sets. A body that ends with `return`, `break` or
// `continue` needs no flag: the `else` body follows the nested `if`s, so the Cpp1
// compiler sees when both end.
auto render_bound_if(const statement& statement, const cpp1_expression_options& condition_options,
                     line_text& output, int depth, lowering_options& expression_options,
                     const loop_context* loop) -> void {
    const auto& form = statement.as<statement::if_form>();
    const auto indent = [](int level) { return std::string(level * 4, ' '); };
    const auto suffix = generated_suffix(statement);
    const auto flag = std::string{generated_name_prefix} + "matched_" + suffix;
    const auto operands = render_cpp1_condition(
        form.condition, condition_options,
        std::string{generated_name_prefix} + "subject_" + suffix + "_");
    const auto has_else = !form.else_body.empty();
    const auto body_jumps = !form.body.empty() &&
        (form.body.back()->kind == statement_kind::return_statement ||
         form.body.back()->kind == statement_kind::break_statement ||
         form.body.back()->kind == statement_kind::continue_statement);
    const auto flagged = has_else && !body_jumps;
    // An else-if continues the enclosing `} else ` line.
    const auto wrap = has_else || form.is_else_if;
    if (wrap) output += (form.is_else_if ? std::string{} : indent(depth)) + "{\n";
    const auto base = wrap ? depth + 1 : depth;
    if (flagged) output += indent(base) + "bool " + flag + " = false;\n";
    auto level = base;
    auto pending = std::string{};
    const auto flush = [&] {
        if (pending.empty()) return;
        output += indent(level++) + "if (" + pending + ") {\n";
        pending.clear();
    };
    for (const auto& operand : operands) {
        if (operand.subject.empty()) {
            pending += (pending.empty() ? "" : " && ") + operand.test;
            continue;
        }
        flush();
        const auto name = std::string{generated_name_prefix} + "subject_" + suffix + "_" +
            std::to_string(&operand - operands.data());
        output += indent(level) + "if (auto&& " + name + " = " + operand.subject + "; " +
            operand.test + ") {\n";
        ++level;
        output += indent(level) + operand.bindings + "\n";
    }
    flush();
    if (flagged) output += indent(level) + flag + " = true;\n";
    auto then_options = expression_options;
    for (const auto& nested : form.body) {
        render_statement(*nested, output, level, then_options, loop);
    }
    while (level > base) output += indent(--level) + "}\n";
    if (has_else) {
        output += indent(base) + (flagged ? "if (!" + flag + ") {\n" : std::string{"{\n"});
        auto else_options = expression_options;
        for (const auto& nested : form.else_body) {
            const auto else_if = nested->is<statement::if_form>() &&
                nested->as<statement::if_form>().is_else_if;
            if (else_if) output += indent(base + 1);
            render_statement(*nested, output, base + 1, else_options, loop);
        }
        output += indent(base) + "}\n";
    }
    if (wrap) output += indent(depth) + "}\n";
}

// A `while` whose condition binds (experiment `condition-bindings`): a
// `for (;; next)` whose body tests each operand in turn, leaving the loop
// when one fails, and declares the bindings of each pattern test.
auto render_bound_while(const statement& statement,
                        const cpp1_expression_options& condition_options,
                        const std::string& next, line_text& output, int depth,
                        lowering_options& expression_options) -> void {
    const auto& form = statement.as<statement::loop_form>();
    const auto indent = std::string(depth * 4, ' ');
    const auto inner = std::string((depth + 1) * 4, ' ');
    const auto prefix = std::string{generated_name_prefix} + "subject_" +
        generated_suffix(statement) + "_";
    const auto operands = render_cpp1_condition(form.condition, condition_options, prefix);
    output += indent + (next.empty() ? std::string{"while (true) {\n"}
                                     : "for (;; " + next + ") {\n");
    for (const auto& operand : operands) {
        if (!operand.subject.empty()) {
            output += inner + "auto&& " + prefix + std::to_string(&operand - operands.data()) +
                " = " + operand.subject + ";\n";
        }
        output += inner + "if (!(" + operand.test + ")) break;\n";
        if (!operand.bindings.empty()) output += inner + operand.bindings + "\n";
    }
    auto loop_options = expression_options;
    render_labeled_iteration(form.label, form.body, output, depth + 1, loop_options);
    output += indent + "}\n";
}

// A let-else declaration (experiment `let-else`): the value held in a
// reference, the `else` block run unless it is a `T`, then `declared`, the
// names, as references to `value as T`.
auto render_let_else(const statement& statement, const parsed_expression& initializer,
                     const statement_list& else_body, const std::string& declared,
                     const cpp1_expression_options& initializer_options, line_text& output,
                     int depth, lowering_options& expression_options,
                     const loop_context* loop) -> void {
    const auto indent = std::string(depth * 4, ' ');
    const auto name = std::string{generated_name_prefix} + "let_" + generated_suffix(statement);
    const auto parts = render_cpp1_let_else(initializer, initializer_options, name);
    output += indent + take_try_hoists(initializer_options) + "auto&& " + name + " = " +
        parts.subject + ";\n";
    output += indent + "if (!" + parts.test + ") {\n";
    auto else_options = expression_options;
    for (const auto& nested : else_body) {
        render_statement(*nested, output, depth + 1, else_options, loop);
    }
    output += indent + "}\n";
    output += indent + declared + " = " + parts.bindings + ";\n";
}

auto render_statement_text(const statement& statement, line_text& output, int depth,
                           lowering_options& expression_options,
                           const loop_context* loop) -> void {
    auto indent = std::string(depth * 4, ' ');
    if (expression_options.scopes != nullptr) {
        expression_options = options_at(expression_options, expression_options.scopes->at(statement));
    }
    const auto text_options_for = [&](text_key::part which) {
        return text_options(expression_options, statement_key(statement, which));
    };
    // `?` operands hoist statements before this one, on its line.
    const auto begin = output.tail().size();
    switch (statement.kind) {
    case statement_kind::using_declaration: {
        const auto& name = statement.as<statement::using_form>().name;
        output += indent + render_using(name) + "\n";
        break;
    }
    case statement_kind::declaration_statement: {
        const auto& form = statement.as<statement::declaration_form>();
        if (form.declaration) {
            const auto& data = *form.declaration;
            if (data.kind == declaration_kind::namespace_node && data.is_alias) {
                output += indent + "namespace " + render_name(data.name) + " = " + data.alias +
                    ";\n";
                break;
            }
            if (data.kind == declaration_kind::type && data.is_alias) {
                output += indent + render_template_parameters(data.template_parameters) +
                    "using " + render_name(data.name) + " = " +
                    render_cpp1_type(with_declared_types(parse_dependent_type(data.alias,
                        expression_options.scopes, expression_options.point, {},
                        alias_type_options(expression_options)), expression_options)) +
                    ";\n";
                break;
            }
            if (form.is_let_else) {
                render_let_else(statement, data.initializer, form.else_body,
                                "auto&& " + render_name(data.name),
                                text_options_for(text_key::part::initializer), output, depth,
                                expression_options, loop);
                break;
            }
            // A local named `_` is never used.
            output += indent + (data.name == "_" ? std::string{"[[maybe_unused]] "} : std::string{}) +
                (data.is_static ? std::string{"static "} : std::string{}) +
                (data.is_alias ? std::string{"constexpr "} : std::string{});
            const auto name = render_object_name(data.name, statement.span.begin);
            const auto declared =
                with_declared_types(data.type, expression_options);
            if (!data.is_alias && data.initializer.empty()) {
                const auto type = declared.is_wildcard() ? std::string{"auto"} :
                    render_cpp1_type(declared);
                output += "cpp2::impl::deferred_init<" + type + "> " + name;
            }
            else if (data.type.is_wildcard()) {
                output += render_deduced_type(data.type) + " " + name;
            }
            else {
                output += render_cpp1_type_declaration(declared, name);
            }
            if (!data.is_alias && data.initializer.empty()) {
                expression_options.wrapped_parameters.push_back({
                    data.name,
                    declared.is_wildcard() ? "auto" : render_cpp1_type(declared),
                    false,
                    expression_options.scopes == nullptr ? nullptr
                        : expression_options.scopes->declared_by(statement)});
            }
            if (!data.initializer.empty()) {
                output += " " + render_object_initializer(data.initializer,
                    text_options_for(text_key::part::initializer));
            }
            output += ";\n";
            if (const auto assertion = deduced_constraint_assertion(data.type, name);
                !assertion.empty() && !data.initializer.empty()) {
                output += indent + assertion + "\n";
            }
        }
        else {
            output += indent + render_cpp1_expression(form.text,
                text_options_for(text_key::part::text)) + "\n";
        }
        break;
    }
    case statement_kind::destructuring_statement: {
        // `_` binds a generated name, unique by its source position.
        const auto& form = statement.as<statement::destructuring_form>();
        auto names = std::string{};
        auto unused = false;
        for (const auto& binding : form.bindings) {
            if (!names.empty()) names += ", ";
            if (binding.name == "_") {
                unused = true;
                names += std::string{generated_name_prefix} + "unused_" +
                    std::to_string(binding.span.begin.line) + "_" +
                    std::to_string(binding.span.begin.column);
            }
            else names += render_name(binding.name);
        }
        if (form.is_let_else) {
            render_let_else(statement, form.initializer, form.else_body,
                            (unused ? "[[maybe_unused]] " : "") + std::string{"auto&& ["} +
                                names + "]",
                            text_options_for(text_key::part::initializer), output, depth,
                            expression_options, loop);
            break;
        }
        output += indent + (unused ? "[[maybe_unused]] " : "") + "auto [" + names + "] = " +
            render_cpp1_expression(form.initializer,
                                   text_options_for(text_key::part::initializer)) +
            ";\n";
        break;
    }
    case statement_kind::expression: {
        const auto& form = statement.as<statement::expression_form>();
        auto text = render_cpp1_expression(form.text,
            statement_text_options(expression_options, statement));
        if (!text.empty() && text.back() != ';') text += ';';
        output += indent + text + "\n";
        break;
    }
    case statement_kind::discard_statement:
        output += indent + "static_cast<void>(" +
            render_cpp1_expression(statement.as<statement::expression_form>().text,
                text_options_for(text_key::part::text)) +
            ");\n";
        break;
    case statement_kind::block: {
        const auto& form = statement.as<statement::block_form>();
        output += indent + "{\n";
        auto block_options = expression_options;
        for (auto index = std::size_t{0}; index < form.parameters.size(); ++index) {
            const auto& parameter = form.parameters[index];
            // `(x: T = (args))` constructs a new object, which can deduce T's
            // template arguments; `cpp2::impl::in<T>` could not.
            const auto point = block_options.scopes == nullptr ? scope_point{}
                : block_options.scopes->at(parameter);
            auto value = parameter.value;
            value.type = with_declared_types(std::move(value.type), options_at(block_options, point));
            const auto constructs = value.passing == passing_style::in &&
                !value.type.is_wildcard() && parameter.initializer.source().starts_with('(');
            const auto declarator = constructs
                ? std::string{parameter_unused_prefix(value)} + render_cpp1_type(value.type) +
                    " const " + render_parameter_name(value, 1)
                : render_range_parameter(value);
            output += indent + "    " + declarator + render_object_initializer(
                parameter.initializer,
                text_options(options_at(block_options, point),
                             statement_key(statement, text_key::part::parameter, index))) +
                ";\n";
            if (const auto assertion = deduced_constraint_assertion(value.type,
                    render_parameter_name(value, 1));
                !assertion.empty()) {
                output += indent + "    " + assertion + "\n";
            }
        }
        for (const auto& nested : form.body) {
            render_statement(*nested, output, depth + 1, block_options, loop);
        }
        output += indent + "}\n";
        break;
    }
    case statement_kind::unsafe_block: {
        // The body is rendered without the runtime checks the options enable.
        const auto& form = statement.as<statement::block_form>();
        output += indent + "{ // unsafe\n";
        auto block_options = expression_options;
        disable_runtime_checks(block_options);
        for (const auto& nested : form.body) {
            render_statement(*nested, output, depth + 1, block_options, loop);
        }
        output += indent + "}\n";
        break;
    }
    case statement_kind::defer_statement: {
        // A scope guard runs the body when the enclosing block exits.
        const auto& form = statement.as<statement::block_form>();
        output += indent + "auto " + std::string{generated_name_prefix} + "defer_" +
            std::to_string(statement.span.begin.line) + "_" +
            std::to_string(statement.span.begin.column) + " = cpp2::finally([&]{\n";
        auto body_options = expression_options;
        for (const auto& nested : form.body) {
            render_statement(*nested, output, depth + 1, body_options);
        }
        output += indent + "});\n";
        add_mentioned_names(form.body, expression_options.deferred_names);
        break;
    }
    case statement_kind::try_statement: {
        const auto& form = statement.as<statement::block_form>();
        output += indent + "try {\n";
        auto try_options = expression_options;
        for (const auto& nested : form.body) {
            render_statement(*nested, output, depth + 1, try_options, loop);
        }
        output += indent + "}";
        for (const auto& handler : form.handlers) {
            output += " catch (" + render_catch_parameter(handler.caught) + ") {\n";
            auto handler_options = expression_options;
            for (const auto& nested : handler.body) {
                render_statement(*nested, output, depth + 1, handler_options, loop);
            }
            output += indent + "}";
        }
        output += "\n";
        break;
    }
    case statement_kind::if_statement: {
        const auto& form = statement.as<statement::if_form>();
        if (!form.bindings.empty()) {
            render_bound_if(statement, text_options_for(text_key::part::condition), output,
                            depth, expression_options, loop);
            break;
        }
        const auto condition = render_cpp1_expression(form.condition,
                                                      text_options_for(text_key::part::condition));
        auto compact = condition;
        std::erase_if(compact, [](char value) { return value == ' '; });
        // An else-if continues the enclosing `} else ` line. `if consteval` and
        // `if !consteval` take no parentheses.
        output += (form.is_else_if ? std::string{} : indent) + "if " +
            (form.is_constexpr ? "constexpr " : "") +
            (compact == "consteval" || compact == "!consteval" ? compact
                                                               : "(" + condition + ")") +
            " {\n";
        auto then_options = expression_options;
        for (const auto& nested : form.body) {
            render_statement(*nested, output, depth + 1, then_options, loop);
        }
        if (form.else_body.empty()) {
            output += indent + "}\n";
        }
        else {
            auto else_options = expression_options;
            const auto& first = *form.else_body.front();
            if (first.is<statement::if_form>() && first.as<statement::if_form>().is_else_if) {
                output += indent + "} else ";
                render_statement(first, output, depth, else_options, loop);
            }
            else {
                output += indent + "} else {\n";
                for (const auto& nested : form.else_body) {
                    render_statement(*nested, output, depth + 1, else_options, loop);
                }
                output += indent + "}\n";
            }
        }
        break;
    }
    case statement_kind::while_statement: {
        const auto& form = statement.as<statement::loop_form>();
        const auto condition = render_cpp1_expression(form.condition,
                                                      text_options_for(text_key::part::condition));
        const auto next = next_expression(
            render_next_items(form.next, text_options_for(text_key::part::next)));
        auto loop_options = expression_options;
        if (!form.bindings.empty()) {
            render_bound_while(statement, text_options_for(text_key::part::condition), next,
                               output, depth, expression_options);
        }
        else if (form.next.empty()) {
            output += indent + "while (" + condition + ") {\n";
            render_labeled_iteration(form.label, form.body, output, depth + 1, loop_options);
            output += indent + "}\n";
        }
        else {
            output += indent + "for (; " + condition + "; " + next + ") {\n";
            render_labeled_iteration(form.label, form.body, output, depth + 1, loop_options);
            output += indent + "}\n";
        }
        break;
    }
    case statement_kind::do_statement: {
        const auto& form = statement.as<statement::loop_form>();
        auto condition = render_cpp1_expression(form.condition,
                                                text_options_for(text_key::part::condition));
        const auto next = next_statements(
            render_next_items(form.next, text_options_for(text_key::part::next)));
        auto loop_options = expression_options;
        while (!condition.empty() && (condition.back() == ';' ||
               std::isspace(static_cast<unsigned char>(condition.back())))) {
            condition.pop_back();
        }
        if (form.next.empty()) {
            output += indent + "do {\n";
            render_labeled_iteration(form.label, form.body, output, depth + 1, loop_options);
            output += indent + "} while (" + condition + ");\n";
        }
        else {
            output += indent + "do {\n";
            render_labeled_iteration(form.label, form.body, output, depth + 1, loop_options);
            output += indent + "} while ([&]{ " + next +
                "return true; }() && " + condition + ");\n";
        }
        break;
    }
    case statement_kind::for_statement: {
        const auto& form = statement.as<statement::loop_form>();
        const auto condition = render_range(form.condition,
                                            text_options_for(text_key::part::condition));
        auto range_parameter = form.range_parameter;
        if (range_parameter) {
            range_parameter->type = with_declared_types(std::move(range_parameter->type),
                                                        expression_options);
        }
        const auto parameter = range_parameter ? render_range_parameter(*range_parameter)
                                               : std::string{"auto"};
        const auto assertion = form.range_parameter
            ? deduced_constraint_assertion(form.range_parameter->type,
                                           render_parameter_name(*form.range_parameter, 1))
            : std::string{};
        const auto nested_indent = std::string((depth + 1) * 4, ' ');
        const auto open = indent + "for (" + parameter + " : " + condition + ") {\n" +
            (assertion.empty() ? "" : nested_indent + assertion + "\n");
        const auto next_items = render_next_items(form.next,
                                                  text_options_for(text_key::part::next));
        auto loop_options = expression_options;
        if (!form.next.empty()) {
            auto loop = loop_context{};
            if (body_breaks_loop(form.body, form.label)) {
                loop = loop_context{true,
                    "__cpp2_for_break_" + std::to_string(statement.span.begin.line)};
            }
            output += open;
            if (loop.wraps_next) {
                output += nested_indent + "bool " + loop.break_flag + " = false;\n";
            }
            output += nested_indent + "do {\n";
            render_statements(form.body, output, depth + 2, loop_options,
                              loop.wraps_next ? &loop : nullptr);
            output += nested_indent + "} while (false);\n";
            if (loop.wraps_next) {
                output += nested_indent + "if (" + loop.break_flag + ") break;\n";
            }
            // A labeled `continue` from a nested loop also runs the next clause.
            if (!form.label.empty()) {
                output += nested_indent + "goto CONTINUE_" + form.label + "; CONTINUE_" +
                    form.label + ": ;\n";
            }
            // The next clause follows the body, so its line goes back.
            if (const auto begin = form.next.position_of(0)) {
                place_on_line(expression_options, output, statement, begin->line);
            }
            for (const auto& item : next_items) output += nested_indent + item + ";\n";
            if (!form.label.empty()) {
                output += nested_indent + "continue; goto BREAK_" + form.label + "; BREAK_" +
                    form.label + ": break;\n";
            }
            output += indent + "}\n";
            break;
        }
        output += open;
        render_labeled_iteration(form.label, form.body, output, depth + 1, loop_options);
        output += indent + "}\n";
        break;
    }
    case statement_kind::break_statement: {
        const auto& label = statement.as<statement::jump_form>().label;
        if (label.empty() && loop != nullptr && loop->wraps_next) {
            output += indent + loop->break_flag + " = true; continue;\n";
        }
        else {
            output += indent + (label.empty() ? std::string{"break"} : "goto BREAK_" + label) +
                ";\n";
        }
        break;
    }
    case statement_kind::continue_statement: {
        const auto& label = statement.as<statement::jump_form>().label;
        output += indent + (label.empty() ? std::string{"continue"} : "goto CONTINUE_" + label) +
            ";\n";
        break;
    }
    case statement_kind::contract_statement: {
        const auto& form = statement.as<statement::contract_form>();
        if (form.group == "unevaluated") break;
        output += indent + render_contract_check(form, expression_options, &statement) + "\n";
        break;
    }
    case statement_kind::return_statement: {
        // As upstream, postconditions run before the returned value is computed.
        const auto& text = statement.as<statement::expression_form>().text;
        output += indent + (expression_options.run_postconditions
            ? "cpp2_finally_presuccess.run(); " : "");
        const auto name = returned_name(text);
        const auto options = text_options_for(text_key::part::text);
        const auto returned = [&](const std::string& value) {
            return value_return_check(expression_options, statement_key(statement), value) +
                "return " + value + ";\n";
        };
        if (!name.empty() && std::find(expression_options.deferred_names.begin(),
                                       expression_options.deferred_names.end(),
                                       name) != expression_options.deferred_names.end()) {
            // A copy, so that a defer neither sees the name moved from nor
            // changes the result.
            output += returned("std::as_const(" + render_cpp1_expression(text, options) + ")");
        }
        else if (is_list_expression(text) ||
                 (expression_options.braced_return_list && is_braced_initializer(text))) {
            output += "return " + render_object_initializer(text, options) + ";\n";
        }
        else if (!text.empty()) {
            output += returned(render_cpp1_expression(text, options));
        }
        else if (!expression_options.named_return_value.empty()) {
            output += "return " + expression_options.named_return_value + ";\n";
        }
        else {
            output += "return;\n";
        }
        break;
    }
    }
    if (const auto hoisted = take_try_hoists(expression_options); !hoisted.empty()) {
        output.tail().insert(begin + indent.size(), hoisted);
    }
}

auto render_statements(const std::vector<std::unique_ptr<statement>>& statements,
                       line_text& output, int depth, lowering_options& expression_options,
                       const loop_context* loop) -> void {
    for (const auto& statement : statements) {
        render_statement(*statement, output, depth, expression_options, loop);
    }
}

auto is_main_args_entry_point(const ir_declaration& declaration) -> bool {
    if (!declaration.is_entry_point || declaration.parameters.size() != 1) return false;
    return is_main_args_parameter(declaration.parameters.front());
}

auto is_assignment_operator(const ir_declaration& declaration) -> bool {
    return declaration.scope == declaration_scope::type_scope && is_assignment(declaration);
}

// A deduced parameter with a default, `y := 2`, deduces its type from an
// explicit template parameter whose default is the default argument's type,
// since Cpp1 deduces nothing from a default argument.
auto has_defaulted_type(const parameter& parsed_parameter) -> bool {
    return parsed_parameter.type.is_wildcard() && !parsed_parameter.default_value.empty() &&
        !parsed_parameter.is_variadic && parsed_parameter.passing != passing_style::out;
}

auto defaulted_type_name(const parameter& parsed_parameter) -> std::string {
    return "__cpp2_default_" + render_name(parsed_parameter.name);
}

// An `in`, `copy` or `inout` parameter `x: _ is C` deduces its value type, so
// it lowers to `C auto`, which Cpp1 orders by subsumption.
auto is_constrained_auto(const parameter& parsed_parameter) -> bool {
    const auto* constraint = parsed_parameter.type.constraint();
    // `A & B auto` is not Cpp1, so an operator constraint goes in a requires clause.
    return constraint != nullptr && !constraint->is_operator() &&
        (parsed_parameter.passing == passing_style::in ||
         parsed_parameter.passing == passing_style::in_ref ||
         parsed_parameter.passing == passing_style::copy ||
         parsed_parameter.passing == passing_style::inout);
}

auto render_function_parameters(const ir_declaration& declaration, bool with_defaults)
    -> std::string {
    if (is_main_args_entry_point(declaration)) return "int const argc_, char** argv_";
    const auto is_special_member_that = [&](const parameter& value, std::size_t index) {
        return index == 1 && is_that_parameter(value) &&
            (declaration.special_member ==
                 ir_declaration::special_member_kind::copy_assignment ||
             declaration.special_member ==
                 ir_declaration::special_member_kind::move_assignment);
    };
    auto result = std::string{};
    auto first = true;
    for (std::size_t index = 0; index < declaration.parameters.size(); ++index) {
        const auto& parsed_parameter = declaration.parameters[index];
        if (declaration.scope == declaration_scope::type_scope &&
            is_this_parameter(parsed_parameter)) {
            continue;
        }
        if (!first) result += ", ";
        first = false;
        // A typed pack deduces its elements, which a constraint converts.
        const auto is_typed_pack = parsed_parameter.is_variadic &&
            !has_deduced_type(parsed_parameter.type) &&
            !is_dependent_parameter_type(declaration, parsed_parameter.type);
        auto type = parsed_parameter.type.is_wildcard() || is_typed_pack ? std::string{"auto"} :
            render_cpp1_type(parsed_parameter.type);
        if (has_defaulted_type(parsed_parameter)) {
            type = defaulted_type_name(parsed_parameter);
        }
        else if (is_constrained_auto(parsed_parameter)) {
            type = render_cpp1_type(*parsed_parameter.type.constraint()) + " auto";
        }
        if (is_special_member_that(parsed_parameter, index) ||
            (is_member_that(declaration, index) && !declaration.special_member_owner.empty())) {
            type = render_cpp1_type(declaration.special_member_owner);
        }
        const auto name = (parsed_parameter.is_variadic ? "..." : "") +
            render_parameter_name(parsed_parameter, index + 1) +
            (with_defaults && !parsed_parameter.default_value.empty() &&
             !is_assignment_operator(declaration)
                ? " = " + render_default_argument(parsed_parameter.default_value)
                : std::string{});
        // `that` may be unused, as in a type with no members; upstream marks it there.
        const auto prefix = is_special_member_that(parsed_parameter, index) ||
                is_member_that(declaration, index)
            ? std::string{"[[maybe_unused]] "}
            : std::string{parameter_unused_prefix(parsed_parameter)};
        // A function type passed `in` or `copy` is passed by value; other
        // passing styles qualify it as one type name.
        if (parsed_parameter.type.core().is_function()) {
            if (parsed_parameter.passing == passing_style::in ||
                parsed_parameter.passing == passing_style::copy) {
                result += prefix + render_cpp1_type_declaration(parsed_parameter.type, name);
                continue;
            }
            type = "std::type_identity_t<" + render_cpp1_type(parsed_parameter.type) + ">";
        }
        if (parsed_parameter.passing == passing_style::in && !parsed_parameter.is_variadic &&
            !has_deduced_type(parsed_parameter.type) &&
            !is_dependent_parameter_type(declaration, parsed_parameter.type) &&
            !is_member_that(declaration, index)) {
            result += prefix + "cpp2::impl::in<" + type + "> " + name;
            continue;
        }
        if (parsed_parameter.passing == passing_style::out &&
            !has_deduced_type(parsed_parameter.type)) {
            result += prefix + "cpp2::impl::out<" + type + "> " + name;
            continue;
        }
        // A generic `out` parameter takes the caller's `cpp2::impl::out` by value.
        if (parsed_parameter.passing == passing_style::out && !is_this_parameter(parsed_parameter)) {
            result += prefix + "auto " + name + "_";
            continue;
        }
        auto qualifier = std::string_view{};
        switch (parsed_parameter.passing) {
        case passing_style::in: qualifier = " const&"; break;
        case passing_style::in_ref: qualifier = " const&"; break;
        case passing_style::inout:
        case passing_style::out: qualifier = "&"; break;
        case passing_style::move: qualifier = "&&"; break;
        case passing_style::forward:
            // A type naming a template parameter deduces, so it needs no constraint.
            if (!is_dependent_parameter_type(declaration, parsed_parameter.type) &&
                !has_defaulted_type(parsed_parameter)) {
                type = "auto";
            }
            qualifier = "&&";
            break;
        case passing_style::copy: break;
        }
        result += prefix + type + std::string{qualifier} + " " + name;
    }
    return result;
}

auto has_void_return(const ir_declaration& declaration) -> bool {
    const auto form = return_form_of(declaration);
    // `main: () = expr;` returns nothing, as `main: () = { expr; }` does.
    return form == return_form::implicit_void ||
        (form == return_form::implicit_deduced && declaration.is_entry_point) ||
        (form == return_form::declared && declaration.type.is_void());
}

auto function_return_type(const ir_declaration& declaration) -> std::string {
    const auto form = return_form_of(declaration);
    switch (form) {
    case return_form::deduced_forwarding: return "decltype(auto)";
    case return_form::deduced_forwarding_ref: return "auto&&";
    case return_form::deduced_value: return "auto";
    default: break;
    }
    // A `void` `main` is `int main` in Cpp1.
    if (declaration.is_entry_point && has_void_return(declaration)) return "int";
    switch (form) {
    case return_form::implicit_void:
    case return_form::implicit_deduced:
        if (is_assignment_operator(declaration) &&
            !declaration.special_member_owner.empty()) {
            return declaration.special_member_owner + "&";
        }
        return form == return_form::implicit_void ? "void" : "decltype(auto)";
    case return_form::declared_forwarding: {
        const auto receiver_is_const = declaration.scope == declaration_scope::type_scope &&
            has_receiver(declaration, passing_style::in);
        // A reference to a function type, as to a function type parameter, is to
        // one type name.
        const auto type = declaration.type.core().is_function()
            ? "std::type_identity_t<" + render_cpp1_type(declaration.type) + ">"
            : render_cpp1_type(declaration.type);
        return type + (receiver_is_const ? " const&" : "&");
    }
    default: break;
    }
    // The named-return struct of a function template is a template too.
    const auto arguments =
        declaration.named_returns.empty() ? std::string{} : render_template_arguments(declaration);
    return render_cpp1_type(declaration.type) + arguments;
}

auto function_prolog(const ir_declaration& declaration) -> std::string {
    auto result = is_main_args_entry_point(declaration)
        ? std::string{"auto const args = cpp2::make_args(argc_, argv_);"}
        : std::string{};
    for (std::size_t index = 0; index < declaration.parameters.size(); ++index) {
        const auto& parameter = declaration.parameters[index];
        if (parameter.passing != passing_style::out || !has_deduced_type(parameter.type) ||
            is_this_parameter(parameter)) {
            continue;
        }
        const auto name = render_parameter_name(parameter, index + 1);
        if (!result.empty()) result += " ";
        result += "auto " + name + " = cpp2::impl::out(" + name + "_);";
    }
    return result;
}

// One typed, unnamed return value, as `-> T` or `-> forward T`.
auto has_typed_return(const ir_declaration& declaration) -> bool {
    const auto form = return_form_of(declaration);
    return declaration.named_returns.empty() && !has_void_return(declaration) &&
        (form == return_form::declared || form == return_form::declared_forwarding);
}

auto uses_expression_statement_body(const ir_declaration& declaration) -> bool {
    return has_void_return(declaration) || has_receiver(declaration, passing_style::out) ||
        declaration.special_member == ir_declaration::special_member_kind::destructor;
}

auto parameter_forward_constraint(const ir_declaration& declaration,
                                  const parameter& parsed_parameter,
                                  std::size_t ordinal) -> std::string {
    if (parsed_parameter.passing != passing_style::forward || parsed_parameter.is_variadic ||
        has_deduced_type(parsed_parameter.type) ||
        is_dependent_parameter_type(declaration, parsed_parameter.type)) {
        return {};
    }
    return "std::is_convertible_v<CPP2_TYPEOF(" +
        render_parameter_name(parsed_parameter, ordinal) +
        "), std::add_const_t<" + render_cpp1_type(parsed_parameter.type) + ">&>";
}

// `!std::is_lvalue_reference_v<decltype(x)>` for a `move` parameter whose `&&`
// is a forwarding reference, as for `x: _`, a typed pack `xs...: X` and `x: T`
// with `T` a type parameter of the function, since a `move` parameter takes
// only rvalues.
auto parameter_move_constraint(const ir_declaration& declaration,
                               const parameter& parsed_parameter,
                               std::size_t ordinal) -> std::string {
    if (parsed_parameter.passing != passing_style::move || is_this_parameter(parsed_parameter) ||
        is_that_parameter(parsed_parameter)) {
        return {};
    }
    const auto& type = parsed_parameter.type;
    const auto forwarding = type.is_wildcard() ||
        (parsed_parameter.is_variadic && !is_dependent_parameter_type(declaration, type)) ||
        (type.kind == type_kind::name && type.components.size() == 1 && !type.is_global &&
         !type.components.front().has_arguments &&
         std::ranges::any_of(declaration.template_parameters, [&](const auto& value) {
             return value.is_type_parameter() && value.name == type.components.front().identifier;
         }));
    if (!forwarding) return {};
    const auto check = "!std::is_lvalue_reference_v<decltype(" +
        render_parameter_name(parsed_parameter, ordinal) + ")>";
    return parsed_parameter.is_variadic ? "(" + check + " && ...)" : check;
}

// `(std::is_convertible_v<CPP2_TYPEOF(x), T> && ...)` for a typed pack `x...: T`.
auto parameter_pack_constraint(const ir_declaration& declaration,
                               const parameter& parsed_parameter,
                               std::size_t ordinal) -> std::string {
    if (!parsed_parameter.is_variadic || has_deduced_type(parsed_parameter.type) ||
        is_dependent_parameter_type(declaration, parsed_parameter.type)) {
        return {};
    }
    return "(std::is_convertible_v<CPP2_TYPEOF(" +
        render_parameter_name(parsed_parameter, ordinal) + "), " +
        render_cpp1_type(parsed_parameter.type) + "> && ...)";
}

// One condition of a `requires` clause; a compound one, `a && b` or `a || b`
// from an operator constraint, is parenthesized when joined with others.
struct constraint_condition final {
    std::string text;
    bool compound = false;
};

// The check of `constraint` on `subject`, folded over `&&` for a pack.
auto constraint_check(const type_id& constraint, const std::string& subject, bool variadic)
    -> constraint_condition {
    auto check = render_cpp1_constraint(constraint, subject);
    if (!variadic) return {std::move(check), constraint.is_operator()};
    return {"(" + (constraint.is_operator() ? "(" + check + ")" : check) + " && ...)", false};
}

// `C<CPP2_TYPEOF(x), args...>` for a `move` or `forward` parameter
// `x: _ is C<args...>`, folded over `&&` for a pack `xs...: _ is C`.
auto parameter_type_constraint(const parameter& parsed_parameter,
                               std::size_t ordinal) -> constraint_condition {
    const auto* constraint = parsed_parameter.type.constraint();
    if (constraint == nullptr || is_constrained_auto(parsed_parameter)) return {};
    // A defaulted parameter's type is a template parameter; an operator
    // constraint on it is checked there, since it cannot be the parameter's kind.
    // A generic `out` parameter `x_` is the caller's `cpp2::impl::out<T>`, constraining `T`.
    const auto name = render_parameter_name(parsed_parameter, ordinal);
    const auto subject = constraint->is_operator() && has_defaulted_type(parsed_parameter)
        ? defaulted_type_name(parsed_parameter)
        : parsed_parameter.passing == passing_style::out && !is_this_parameter(parsed_parameter)
        ? "CPP2_TYPEOF(" + name + "_.value())"
        : "CPP2_TYPEOF(" + name + ")";
    return constraint_check(*constraint, subject, parsed_parameter.is_variadic);
}

// The conditions of the template parameters `T: A & B` and `T: A | B` of
// `declaration`, whose unnamed parameters are named as `render_template_prefix` does.
auto template_parameter_conditions(const declaration_signature& declaration)
    -> std::vector<constraint_condition> {
    const auto& parameters = declaration.template_parameters;
    const auto suffix = declaration.scope == declaration_scope::type_scope
        ? "_" + declaration.name : std::string{};
    auto conditions = std::vector<constraint_condition>{};
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        const auto& parameter = parameters[index];
        if (parameter.constraint.empty()) continue;
        auto name = render_name(parameter.name);
        if (name.empty()) name = "UnnamedTemplateParam" + std::to_string(index + 1) + suffix;
        conditions.push_back(constraint_check(parameter.constraint, name, parameter.is_variadic));
    }
    return conditions;
}

auto join_conditions(const std::vector<constraint_condition>& conditions) -> std::string {
    if (conditions.size() == 1) return conditions.front().text;
    auto result = std::string{};
    for (const auto& condition : conditions) {
        if (!result.empty()) result += " && ";
        result += condition.compound ? "(" + condition.text + ")" : condition.text;
    }
    return result;
}

auto function_constraint(
    const ir_declaration& declaration,
    const std::vector<cpp1_expression_options::wrapped_parameter>& wrapped = {}) -> std::string {
    auto conditions = template_parameter_conditions(declaration);
    if (!declaration.requires_clause.empty()) {
        // The function's own `out` parameters come first.
        auto names = std::vector<cpp1_expression_options::wrapped_parameter>{};
        for (const auto& parameter : declaration.parameters) {
            if (parameter.passing == passing_style::out && !is_this_parameter(parameter) &&
                !parameter.type.is_wildcard()) {
                names.push_back({parameter.name, render_cpp1_type(parameter.type), true, nullptr});
            }
        }
        names.insert(names.end(), wrapped.begin(), wrapped.end());
        conditions.push_back({render_requires_clause(declaration.requires_clause, names)});
    }
    for (std::size_t index = 0; index < declaration.parameters.size(); ++index) {
        const auto& parsed_parameter = declaration.parameters[index];
        for (auto condition : {parameter_type_constraint(parsed_parameter, index + 1),
                               constraint_condition{parameter_forward_constraint(
                                   declaration, parsed_parameter, index + 1)},
                               constraint_condition{parameter_move_constraint(
                                   declaration, parsed_parameter, index + 1)},
                               constraint_condition{parameter_pack_constraint(
                                   declaration, parsed_parameter, index + 1)}}) {
            if (!condition.text.empty()) conditions.push_back(std::move(condition));
        }
    }
    return join_conditions(conditions);
}

// The template parameters of `declaration`, with one for each defaulted
// parameter of deduced type. Default arguments belong only on the first declaration.
auto cpp1_template_parameters(const declaration_signature& declaration, bool with_defaults)
    -> std::vector<cpp1_template_parameter> {
    auto parameters = std::vector<cpp1_template_parameter>{};
    for (const auto& parameter : declaration.template_parameters) {
        parameters.push_back(render_template_parameter(parameter));
    }
    if (declaration.kind == declaration_kind::function) {
        for (const auto& parameter : declaration.parameters) {
            if (!has_defaulted_type(parameter)) continue;
            auto added = cpp1_template_parameter{};
            added.name = defaulted_type_name(parameter);
            const auto* constraint = parameter.type.constraint();
            added.kind = constraint != nullptr && !constraint->is_operator()
                ? render_cpp1_type(*constraint) : "typename";
            added.default_argument = "std::remove_cvref_t<decltype(" +
                render_default_argument(parameter.default_value) + ")>";
            parameters.push_back(std::move(added));
        }
    }
    if (!with_defaults) {
        for (auto& parameter : parameters) parameter.default_argument.clear();
    }
    return parameters;
}

auto render_template_prefix(const declaration_signature& declaration, bool with_defaults = true)
    -> std::string {
    return render_template_parameters(cpp1_template_parameters(declaration, with_defaults),
        declaration.scope == declaration_scope::type_scope ? "_" + declaration.name : "");
}

auto render_constraint(const declaration_signature& declaration) -> std::string {
    auto conditions = template_parameter_conditions(declaration);
    if (!declaration.requires_clause.empty()) {
        conditions.push_back({render_requires_clause(declaration.requires_clause)});
    }
    if (conditions.empty()) return {};
    return "requires (" + join_conditions(conditions) + ") ";
}

auto render_function_constraint(
    const ir_declaration& declaration,
    const std::vector<cpp1_expression_options::wrapped_parameter>& wrapped = {}) -> std::string {
    const auto conditions = function_constraint(declaration, wrapped);
    if (conditions.empty()) return {};
    return " requires (" + conditions + ")";
}

// `with_defaults` includes default arguments, which belong only on the first
// declaration of a function.
auto this_specifier(const ir_declaration& declaration) -> virtual_specifier {
    const auto* this_parameter = receiver(declaration);
    return this_parameter == nullptr ? virtual_specifier::none : this_parameter->this_specifier;
}

auto virtual_prefix(const ir_declaration& declaration) -> std::string {
    return this_specifier(declaration) == virtual_specifier::virtual_function ? "virtual " : "";
}

// A virtual function without a body is pure.
auto is_pure_virtual(const ir_declaration& declaration) -> bool {
    const auto specifier = this_specifier(declaration);
    return (specifier == virtual_specifier::virtual_function ||
            specifier == virtual_specifier::override_function) &&
        declaration.body.empty() && !declaration.function_statements;
}

// A member `operator==` or `operator<=>` without a body is defaulted.
auto is_defaulted_comparison(const ir_declaration& declaration) -> bool {
    return declaration.scope == declaration_scope::type_scope &&
        (declaration.name == "operator==" || declaration.name == "operator<=>") &&
        declaration.body.empty() && !declaration.function_statements;
}

// functions.md: a function that returns a value is `[[nodiscard]]`. As upstream,
// assignment, increment, decrement and stream operators are not, nor are
// function expressions.
auto nodiscard_prefix(const ir_declaration& declaration) -> std::string {
    static const auto exempt = std::set<std::string_view>{
        "operator+=", "operator-=", "operator*=", "operator/=", "operator%=",
        "operator&=", "operator|=", "operator^=", "operator<<=", "operator>>=",
        "operator++", "operator--", "operator<<", "operator>>"};
    const auto form = return_form_of(declaration);
    if (form == return_form::implicit_void || form == return_form::implicit_deduced ||
        declaration.special_member != ir_declaration::special_member_kind::none ||
        exempt.contains(declaration.name) ||
        is_function_expression_name(declaration.name) ||
        function_return_type(declaration) == "void") {
        return {};
    }
    return "[[nodiscard]] ";
}

// A `qualifier` such as `X::` names the type of an out-of-line member
// definition, which repeats no `explicit`, `virtual`, `override`, or `final`.
auto render_function_signature(const ir_declaration& declaration, bool with_defaults,
                               std::string_view qualifier = {}) -> std::string {
    const auto in_class = qualifier.empty();
    const auto owner = std::string{qualifier} + render_name(declaration.special_member_owner);
    if (declaration.special_member ==
        ir_declaration::special_member_kind::user_default_constructor) {
        return (in_class ? "explicit " : "") + owner + "()";
    }
    if (declaration.special_member ==
        ir_declaration::special_member_kind::defaulted_default_constructor) {
        return render_name(declaration.special_member_owner) + "() = default";
    }
    if (declaration.special_member ==
        ir_declaration::special_member_kind::copy_constructor) {
        return owner + "([[maybe_unused]] " + render_name(declaration.special_member_owner) +
            " const& that)";
    }
    if (declaration.special_member ==
        ir_declaration::special_member_kind::move_constructor) {
        return owner + "([[maybe_unused]] " + render_name(declaration.special_member_owner) +
            "&& that) noexcept";
    }
    if (declaration.special_member ==
        ir_declaration::special_member_kind::destructor) {
        return (in_class ? virtual_prefix(declaration) : std::string{}) +
            std::string{qualifier} + "~" + render_name(declaration.special_member_owner) +
            "() noexcept";
    }
    if (declaration.special_member ==
        ir_declaration::special_member_kind::converting_constructor) {
        return (in_class && declaration.is_explicit ? "explicit " : "") + owner + "(" +
            render_function_parameters(declaration, with_defaults) + ")";
    }
    auto result = "auto " + std::string{qualifier} + render_name(declaration.name) + "(" +
        render_function_parameters(declaration, with_defaults) + ")";
    if (declaration.special_member ==
        ir_declaration::special_member_kind::move_assignment) {
        result += " noexcept";
    }
    const auto specifier = this_specifier(declaration);
    if (const auto* this_parameter = receiver(declaration);
        this_parameter != nullptr && specifier != virtual_specifier::none) {
        // Virtual functions have no ref-qualifier, so they can override Cpp1 functions.
        if (this_parameter->passing == passing_style::in ||
            this_parameter->passing == passing_style::in_ref) {
            result += " const";
        }
    }
    else if (this_parameter != nullptr && !is_assignment_operator(declaration) &&
        declaration.scope == declaration_scope::type_scope) {
        switch (this_parameter->passing) {
        case passing_style::in: result += " const&"; break;
        case passing_style::in_ref: result += " const&"; break;
        case passing_style::inout: result += "&"; break;
        case passing_style::out: result += "&"; break;
        case passing_style::move: result += "&&"; break;
        case passing_style::forward: result += "&&"; break;
        case passing_style::copy: break;
        }
    }
    // As upstream, `swap(this, that)` is implicitly noexcept.
    if (declaration.name == "swap" && declaration.parameters.size() == 2 &&
        is_this_parameter(declaration.parameters[0]) && declaration.parameters[1].name == "that") {
        result += " noexcept";
    }
    result += " -> " + function_return_type(declaration);
    if (!in_class) return result;
    if (specifier == virtual_specifier::override_function) result += " override";
    if (specifier == virtual_specifier::final_function) result += " final";
    return virtual_prefix(declaration) + result;
}

// As upstream, `operator++` and `operator--` taking only their operand also
// define the postfix form, which returns a copy taken before the prefix
// operator, unless their type cannot be copied.
auto postfix_counterpart(const ir_declaration& declaration) -> std::optional<ir_declaration> {
    if (declaration.kind != declaration_kind::function || declaration.omits_postfix ||
        (declaration.name != "operator++" && declaration.name != "operator--") ||
        declaration.parameters.size() != 1) {
        return std::nullopt;
    }
    auto result = declaration;
    result.parameters.push_back({passing_style::copy, parse_type("int"), "_"});
    if (result.deduction == return_deduction::by_forwarding_typed) result.deduction = return_deduction::none;
    else if (result.deduction != return_deduction::none) result.deduction = return_deduction::by_value;
    result.contracts.clear();
    result.function_statements.reset();
    result.body = {};
    result.cpp1_body.clear();
    result.alias.clear();
    return result;
}

auto render_postfix_signature(const ir_declaration& postfix, std::string_view qualifier = {})
    -> std::string {
    return render_template_prefix(postfix) + (postfix.is_constexpr ? "constexpr " : "") +
        render_function_signature(postfix, false, qualifier) + render_function_constraint(postfix);
}

auto postfix_operand(const ir_declaration& postfix) -> std::string {
    return is_this_parameter(postfix.parameters.front())
        ? std::string{"*this"} : render_parameter_name(postfix.parameters.front(), 1);
}

// `struct f_ret { int a; int b; };` for `f: () -> (a: int, b: int)`, and
// `using f_ret = int;` for `f: () -> (a: int)`.
auto render_named_return_struct(const ir_declaration& declaration) -> std::string {
    if (declaration.named_returns.empty()) return {};
    // A function template's struct takes its template parameters.
    auto parameters = std::vector<cpp1_template_parameter>{};
    for (const auto& parameter : declaration.template_parameters) {
        parameters.push_back(render_template_parameter(parameter));
        parameters.back().default_argument.clear();
    }
    const auto head = render_template_parameters(parameters,
        declaration.scope == declaration_scope::type_scope ? "_" + declaration.name : "");
    if (declaration.named_returns.size() == 1) {
        return head + "using " + declaration.type.text + " = " +
            render_cpp1_type(declaration.named_returns.front().value.type) + ";\n";
    }
    auto result = head + "struct " + declaration.type.text + " {";
    for (const auto& named : declaration.named_returns) {
        result += " " + render_cpp1_type_declaration(named.value.type,
                                                     render_name(named.value.name)) + ";";
    }
    return result + " };\n";
}

// `{ std::move(a.value()), std::move(b) }`; deferred values are unwrapped. A
// single value returns as itself: `a`, or `std::move(a.value())` if deferred.
// With `copy` (in a function with a defer, which may read them), the values
// are copied rather than moved.
auto render_named_return_value(const ir_declaration& declaration, bool copy) -> std::string {
    const auto value = [&](const initialized_parameter& named) {
        const auto name = render_name(named.value.name);
        if (copy) {
            return named.initializer.empty() ? name + ".value()" : "std::as_const(" + name + ")";
        }
        return "std::move(" + name + (named.initializer.empty() ? ".value())" : ")");
    };
    if (declaration.named_returns.size() == 1) {
        const auto& named = declaration.named_returns.front();
        return named.initializer.empty() || copy ? value(named) : render_name(named.value.name);
    }
    auto result = std::string{"{ "};
    for (const auto& named : declaration.named_returns) {
        if (result.size() > 2) result += ", ";
        result += value(named);
    }
    return result + " }";
}

// A base copied or moved from `that` is cast so the base's own copy or move
// is chosen over any converting constructor or assignment.
auto render_base_from_that(const ir_declaration::member_initializer& base) -> std::string {
    return "static_cast<" + render_cpp1_type(base.member) + (base.move_that ? "&&" : " const&") +
        ">(that)";
}

// An expression statement's text as in a function body, without its semicolon.
auto render_statement_text(const statement& value, const lowering_options& options)
    -> std::string {
    const auto& text = value.as<statement::expression_form>().text;
    auto rendered =
        render_cpp1_expression(text, statement_text_options(options, value));
    while (!rendered.empty() && (rendered.back() == ';' || rendered.back() == ' ')) {
        rendered.pop_back();
    }
    return rendered;
}

// The last uses in the value of the `member = value;` statement `source`:
// those after the `=`.
auto initializer_last_uses(const statement* source, const lowering_options& options)
    -> std::vector<last_use> {
    if (source == nullptr || options.plan == nullptr) return {};
    const auto& text = source->as<statement::expression_form>().text;
    const auto* uses = last_uses_in(options, statement_key(*source));
    if (uses == nullptr) return {};
    const auto& tokens = text.tree().tokens;
    const auto equals = std::find_if(tokens.begin(), tokens.end(),
                                     [](const auto& token) { return token.text == "="; });
    const auto offset = static_cast<std::size_t>(equals - tokens.begin()) + 1;
    auto result = std::vector<last_use>{};
    for (auto use : *uses) {
        if (use.path.empty() || use.path.front() < offset) continue;
        use.path.front() -= offset;
        result.push_back(std::move(use));
    }
    return result;
}

// A constructor's precondition checks as one expression, or empty.
auto precondition_expression(const ir_declaration& declaration,
                             const lowering_options& expression_options) -> std::string {
    auto checks = std::string{};
    for (const auto* contract : function_contracts(declaration, false)) {
        checks += " " + render_contract_check(*contract, expression_options);
    }
    return checks.empty() ? checks : "[&]{" + checks + " }()";
}

struct rendered_member_initializers final {
    std::string text;
    // The `out` parameter initializations no value follows, for the body.
    std::string remaining;
    // Whether the precondition checks were placed before the first value.
    bool checked = false;
};

// The precondition checks `checks`, then the `out` parameter initializations,
// precede the first value an initializer evaluates, joined to its first
// element by the comma operator.
auto render_member_initializers(const ir_declaration& declaration,
                                lowering_options expression_options,
                                std::string_view checks = {}) -> rendered_member_initializers {
    auto output = rendered_member_initializers{};
    if (declaration.member_initializers.empty()) return output;
    auto& result = output.text;
    result = ": ";
    auto first = true;
    auto out_initializations = std::string{};
    for (const auto& initializer : declaration.member_initializers) {
        const auto* source = initializer_statement(declaration, initializer);
        if (initializer.member.empty()) {
            out_initializations += render_statement_text(*source, expression_options) + ", ";
            continue;
        }
        if (!first) result += ", ";
        first = false;
        const auto uses = initializer_last_uses(source, expression_options);
        auto options = expression_options;
        if (source != nullptr) options.last_uses = &uses;
        const auto value = parsed_expression{initializer.value};
        const auto name = initializer.is_base ? render_cpp1_type(initializer.member)
                                              : render_name(initializer.member);
        const auto from_that = initializer.is_base && initializer.value == "that";
        const auto prefix = (output.checked || checks.empty() ? std::string{}
                                                              : std::string{checks} + ", ") +
            out_initializations;
        if (!prefix.empty() && (from_that || !is_empty_initializer(value.source()))) {
            auto items = from_that ? std::vector{render_base_from_that(initializer)}
                : is_expression_list(value) ? render_cpp1_expression_items(value, options)
                : std::vector{render_cpp1_expression(value, options)};
            items.front() = "(" + prefix + items.front() + ")";
            result += name + "{";
            for (auto index = std::size_t{0}; index < items.size(); ++index) {
                result += (index == 0 ? "" : ", ") + items[index];
            }
            result += "}";
            output.checked = !checks.empty();
            out_initializations.clear();
            continue;
        }
        result += from_that
            ? name + "{" + render_base_from_that(initializer) + "}"
            : initializer.is_base || is_braced_initializer(value)
            ? name + render_object_initializer(value, options)
            : name + "{" + render_cpp1_expression(value, options) + "}";
    }
    if (out_initializations.size() >= 2) {
        out_initializations.resize(out_initializations.size() - 2);
        output.remaining = std::move(out_initializations);
    }
    return output;
}

// As upstream, the member functions of a type outside any function are declared
// in the type and defined after all type definitions.
enum class member_mode : std::uint8_t {
    // Member functions are defined in their type.
    inline_body,
    // Member functions are only declared in their type.
    declaration,
    // An out-of-line definition of a member function.
    definition,
};

// Experiment `lazy`: a lazy object is a `cpp2::lazy<T>` holder, whose
// initializer is a function it runs on the first use, which returns
// `T{init}`, or `std::type_identity_t<T>{init}` when `T` is not a simple type
// specifier, such as `int const*`.
auto render_lazy_declarator(const type_id& type, std::string_view name) -> std::string {
    return "cpp2::lazy<" + render_cpp1_type(type) + "> " + std::string{name};
}

auto render_lazy_initializer(const type_id& type, const parsed_expression& initializer,
                             const cpp1_expression_options& expression_options) -> std::string {
    const auto spelling = render_cpp1_type(type);
    return "{[]{ return " + (is_simple_type_specifier(type) ? spelling
                                 : "std::type_identity_t<" + spelling + ">") +
        render_object_initializer(initializer, expression_options) + "; }}";
}

// A namespace-scope object is declared `extern` in source order and defined
// after the function definitions, as upstream.
auto is_deferred_object(const ir_declaration& declaration) -> bool {
    return declaration.kind == declaration_kind::object && declaration.alias.empty() &&
        declaration.cpp1_text.empty() && declaration.template_parameters.empty() &&
        !declaration.type.is_wildcard() &&
        (declaration.scope == declaration_scope::file ||
         declaration.scope == declaration_scope::namespace_scope);
}

// Whether a type member function is defined outside its type in `declaration`
// mode. A constexpr one is defined in its type, so that a constant after the
// type can evaluate it, unless it is in `options.members_defined_late`.
auto is_defined_out_of_line(const ir_declaration& declaration, const lowering_options& options)
    -> bool {
    using kind = ir_declaration::special_member_kind;
    return declaration.kind == declaration_kind::function &&
        declaration.scope == declaration_scope::type_scope && declaration.cpp1_text.empty() &&
        declaration.alias.empty() &&
        (!declaration.is_constexpr || (options.members_defined_late != nullptr &&
                                       options.members_defined_late->contains(&declaration))) &&
        declaration.special_member != kind::defaulted_default_constructor &&
        declaration.special_member != kind::deleted_copy_constructor &&
        declaration.special_member != kind::deleted_copy_assignment &&
        !is_pure_virtual(declaration) && !is_defaulted_comparison(declaration);
}

// A type-scope object alias whose type names its enclosing type, which is
// incomplete in the class, is declared `static const` there and defined after
// all type definitions, as upstream.
auto is_alias_defined_out_of_line(const ir_declaration& type, const ir_declaration& member)
    -> bool {
    return member.kind == declaration_kind::object && !member.alias.empty() &&
        mentions_name(member.type, type.name);
}

auto render_declaration(const ir_declaration& declaration,
                        lowering_options expression_options,
                        member_mode mode = member_mode::inline_body,
                        std::string_view qualifier = {}, std::string_view heads = {})
    -> line_text;

// In `definition` mode, `qualifier` names the member's type, as in `X::`.
auto render_declaration_without_postfix(const ir_declaration& declaration,
                                        lowering_options expression_options,
                                        member_mode mode = member_mode::inline_body,
                                        std::string_view qualifier = {}) -> line_text {
    if (!declaration.cpp1_text.empty()) return declaration.cpp1_text;
    const auto lambda_head = std::exchange(expression_options.lambda_head, {});
    if (declaration.kind == declaration_kind::function && mode != member_mode::inline_body &&
        !is_defined_out_of_line(declaration, expression_options)) {
        mode = member_mode::inline_body;
    }
    if (expression_options.scopes != nullptr) {
        expression_options = options_at(expression_options,
                                        expression_options.scopes->inside(declaration.symbol));
    }
    // Members of a type are declared or defined in its mode.
    const auto members_mode = mode == member_mode::declaration ? mode : member_mode::inline_body;
    if (expression_options.plan != nullptr) {
        expression_options.that_last_uses =
            expression_options.plan->that_last_uses_in(declaration.symbol);
    }
    switch (declaration.special_member) {
    case ir_declaration::special_member_kind::default_constructor:
        if (mode == member_mode::definition) {
            return std::string{qualifier} + render_name(declaration.special_member_owner) +
                "() {}";
        }
        return "explicit " + render_name(declaration.special_member_owner) +
            (mode == member_mode::declaration ? "();" : "() {}");
    case ir_declaration::special_member_kind::defaulted_default_constructor:
        if (!declaration.member_initializers.empty()) {
            return render_name(declaration.special_member_owner) + "() " +
                render_member_initializers(declaration, expression_options).text + " { }";
        }
        return render_name(declaration.special_member_owner) + "() = default;";
    case ir_declaration::special_member_kind::user_default_constructor:
    case ir_declaration::special_member_kind::copy_constructor:
    case ir_declaration::special_member_kind::move_constructor:
    case ir_declaration::special_member_kind::converting_constructor:
        break;
    case ir_declaration::special_member_kind::copy_assignment:
    case ir_declaration::special_member_kind::move_assignment:
    case ir_declaration::special_member_kind::converting_assignment:
        break;
    case ir_declaration::special_member_kind::destructor: {
        auto result = render_function_signature(declaration, false,
            mode == member_mode::definition ? qualifier : std::string_view{});
        if (mode == member_mode::declaration) return result + ";";
        const auto contracts = postcondition_guard(declaration) +
            render_function_contracts(declaration, false, expression_options) +
            render_function_contracts(declaration, true, expression_options);
        if (declaration.function_statements) {
            auto body = line_text{result + " {\n" + contracts};
            render_statements(declaration.function_statements->statements, body, 1,
                              expression_options);
            body += "}";
            return body;
        }
        else if (!declaration.cpp1_body.empty()) {
            result += " " + declaration.cpp1_body;
        }
        else if (!declaration.body.empty()) {
            auto body_options = expression_options;
            if (body_options.plan != nullptr) {
                body_options.last_uses = body_options.plan->last_uses_in(
                    {declaration.symbol, text_key::part::body});
            }
            const auto expression = render_cpp1_expression(declaration.body, body_options);
            result += contracts.empty() ? " { " + expression + "; }"
                                        : " {\n" + contracts + "    " + expression + ";\n}";
        }
        else {
            result += contracts.empty() ? " { }" : " {\n" + contracts + "}";
        }
        return result;
    }
    case ir_declaration::special_member_kind::deleted_copy_constructor:
        return render_name(declaration.special_member_owner) + "(" +
            render_name(declaration.special_member_owner) +
            " const&) = delete;";
    case ir_declaration::special_member_kind::deleted_copy_assignment:
        return "auto operator=(" + render_name(declaration.special_member_owner) +
            " const&) -> void = delete;";
    case ir_declaration::special_member_kind::none:
        break;
    }
    auto expression_options_with_out = expression_options;
    for (const auto& parameter : declaration.parameters) {
        if (parameter.passing == passing_style::out && !is_this_parameter(parameter)) {
            expression_options_with_out.wrapped_parameters.push_back({parameter.name,
                parameter.type.is_wildcard() ? std::string{} : render_cpp1_type(parameter.type),
                true,
                expression_options.scopes == nullptr ? nullptr
                    : expression_options.scopes->parameter_of(declaration.symbol, parameter.name)});
        }
    }
    if (declaration.kind == declaration_kind::using_declaration) {
        return render_using(declaration.using_name);
    }
    if (!declaration.alias.empty()) {
        if (declaration.kind == declaration_kind::namespace_node) {
            return "namespace " + render_name(declaration.name) + " = " + declaration.alias + ";";
        }
        if (declaration.kind == declaration_kind::type) {
            // An alias has no scope of its own, so its template parameters are passed.
            auto parameters = std::vector<std::string>{};
            for (const auto& parameter : declaration.template_parameters) {
                parameters.push_back(parameter.name);
            }
            return render_template_prefix(declaration) + render_constraint(declaration) +
                "using " + render_name(declaration.name) + " = " +
                render_cpp1_type(parse_dependent_type(declaration.alias, expression_options.scopes,
                                                      expression_options.point,
                                                      std::move(parameters),
                                                      alias_type_options(expression_options))) +
                ";";
        }
    }
    switch (declaration.kind) {
    case declaration_kind::function: {
        // Members are defined where they are declared.
        const auto is_member = declaration.scope == declaration_scope::type_scope;
        // Members and lambdas have no separate first declaration to carry defaults.
        const auto with_defaults = (is_member && mode != member_mode::definition) ||
            is_function_expression_name(declaration.name);
        const auto definition_qualifier =
            mode == member_mode::definition ? qualifier : std::string_view{};
        // A member function without a `this` parameter is static.
        const auto is_static = mode != member_mode::definition &&
            (declaration.is_static_function ||
             (is_member && receiver(declaration) == nullptr &&
              declaration.special_member == ir_declaration::special_member_kind::none));
        auto result = !lambda_head.empty() ? lambda_head
            : render_template_prefix(declaration, with_defaults) + nodiscard_prefix(declaration) +
                (is_static ? "static " : "") + (declaration.is_constexpr ? "constexpr " : "") +
                render_function_signature(declaration, with_defaults, definition_qualifier);
        if (lambda_head.empty()) result += render_function_constraint(declaration);
        result += " ";
        if (declaration.scope == declaration_scope::type_scope && mode != member_mode::definition) {
            result = render_named_return_struct(declaration) + result;
        }
        if (mode == member_mode::declaration) {
            result.back() = ';';
            return result;
        }
        // A constructor checks its preconditions before its member initializers
        // evaluate anything, and an assignment before it assigns the members.
        const auto initialization = render_member_initializers(declaration,
            expression_options_with_out,
            is_constructor(declaration)
                ? precondition_expression(declaration, expression_options_with_out)
                : std::string{});
        const auto& member_initializers = initialization.text;
        const auto checks_before_assignments = !declaration.that_assignments.empty();
        auto that_assignments = std::string{};
        for (const auto& assignment : declaration.that_assignments) {
            const auto text = parsed_expression{assignment.value};
            const auto* source = initializer_statement(declaration, assignment);
            const auto uses = initializer_last_uses(source, expression_options);
            auto options = expression_options;
            if (source != nullptr) options.last_uses = &uses;
            // A leading member or `out` parameter assignment is rendered as in the body.
            if (source != nullptr && !assignment.is_base) {
                that_assignments +=
                    render_statement_text(*source, expression_options_with_out) + "; ";
                continue;
            }
            // A base list is assigned as a braced list, like a member's `m = {x}`, so
            // `B = (that)` binds `that` without constructing a `B`.
            const auto value = assignment.is_base && assignment.value == "that"
                ? render_base_from_that(assignment)
                : assignment.is_base && is_braced_initializer(text)
                ? render_object_initializer(text, options)
                : render_cpp1_expression(text, options);
            that_assignments += assignment.is_base
                ? render_cpp1_type(assignment.member) + "::operator=(" + value + "); "
                : render_name(assignment.member) + " = " + value + "; ";
        }
        expression_options_with_out.braced_return_list = has_typed_return(declaration);
        if (declaration.function_statements) {
            auto body = line_text{result + member_initializers + " {\n"};
            if (checks_before_assignments) {
                body += render_function_contracts(declaration, false, expression_options_with_out);
            }
            if (!that_assignments.empty()) body += "    " + that_assignments + "\n";
            if (const auto prolog = function_prolog(declaration); !prolog.empty()) {
                body += "    " + prolog + "\n";
            }
            const auto& statements = declaration.function_statements->statements;
            auto anchor = line_anchor{&body, {declaration.span.begin.line},
                                      declaration.span.begin};
            // The guard is declared first, or under experiment `defer` after the
            // named return values and before any defer, so that its destructor
            // checks the postconditions after the defers have run, on every exit
            // path, while the named return values exist.
            const auto defers = has_defer(statements);
            if (!defers) body += postcondition_guard(declaration);
            // The contracts follow the named return values they may read.
            // Each check is on the line of its condition.
            const auto add_contracts = [&](bool post) {
                for (const auto* contract : function_contracts(declaration, post)) {
                    const auto begin = contract->condition.position_of(0);
                    if (begin && begin->column < generated_column) {
                        place_on_line(anchor, body, begin->line);
                    }
                    body += render_function_contract(*contract, expression_options_with_out);
                }
            };
            const auto contracts = [&] {
                if (!initialization.checked && !checks_before_assignments) add_contracts(false);
                if (!initialization.remaining.empty()) {
                    body += "    " + initialization.remaining + ";\n";
                }
                if (defers) body += postcondition_guard(declaration);
                add_contracts(true);
            };
            if (!declaration.named_returns.empty()) {
                expression_options_with_out.named_return_value =
                    render_named_return_value(declaration, defers);
            }
            // An assignment operator's `return;` returns the assigned object.
            if (is_assignment_operator(declaration)) {
                expression_options_with_out.named_return_value = "*this";
            }
            expression_options_with_out.run_postconditions =
                has_postconditions(declaration) && !defers;
            auto postconditions = declaration.initializer_statements;
            while (postconditions < statements.size() &&
                   statements[postconditions]->is<statement::declaration_form>() &&
                   statements[postconditions]->as<statement::declaration_form>().declaration &&
                   statements[postconditions]->as<statement::declaration_form>().declaration->is_named_return) {
                ++postconditions;
            }
            // The named return values are declared on the lines after the
            // header; the contract checks and the statements keep their lines.
            for (auto index = declaration.initializer_statements; index < statements.size();
                 ++index) {
                if (index == postconditions) {
                    expression_options_with_out.anchor = &anchor;
                    contracts();
                }
                render_statement(*statements[index], body, 1, expression_options_with_out);
            }
            if (postconditions >= statements.size()) contracts();
            expression_options_with_out.anchor = nullptr;
            if (is_assignment_operator(declaration)) body += "    return *this;\n";
            if (!declaration.named_returns.empty() &&
                (statements.empty() ||
                 statements.back()->kind != statement_kind::return_statement)) {
                body += "    " + std::string{expression_options_with_out.run_postconditions
                    ? "cpp2_finally_presuccess.run(); " : ""} +
                    "return " + expression_options_with_out.named_return_value + ";\n";
            }
            body += "}";
            return body;
        }
        else if (!declaration.cpp1_body.empty()) {
            auto body = declaration.cpp1_body;
            if (is_assignment_operator(declaration)) {
                body.pop_back();
                body += "return *this; }";
            }
            result += member_initializers + " " + body;
        }
        else if (!declaration.body.empty()) {
            const auto prolog = function_prolog(declaration);
            auto prefix = prolog.empty() ? std::string{} : prolog + " ";
            if (!declaration.contracts.empty()) {
                prefix = "\n" + postcondition_guard(declaration) +
                    (initialization.checked ? std::string{}
                        : render_function_contracts(declaration, false,
                                                    expression_options_with_out)) +
                    render_function_contracts(declaration, true, expression_options_with_out) +
                    "    " + prefix;
            }
            auto body_options = expression_options_with_out;
            if (body_options.plan != nullptr) {
                body_options.last_uses = body_options.plan->last_uses_in(
                    {declaration.symbol, text_key::part::body});
            }
            // `= _ = value;` discards the value and returns nothing.
            const auto& tokens = declaration.body.tree().tokens;
            const auto is_discard = tokens.size() > 2 && tokens[0].text == "_" &&
                tokens[1].text == "=";
            // The analyses read a discarded value as the text after `=`.
            const auto body = is_discard
                ? "static_cast<void>(" + render_cpp1_expression(
                      std::string_view{declaration.body.source()}.substr(
                          declaration.body.source().find('=') + 1),
                      body_options) + ")"
                : uses_expression_statement_body(declaration) ||
                        is_assignment_operator(declaration)
                ? render_cpp1_expression(declaration.body, body_options)
                : is_list_expression(declaration.body) ||
                    (body_options.braced_return_list && is_braced_initializer(declaration.body))
                ? render_object_initializer(declaration.body, body_options)
                : render_cpp1_expression(declaration.body, body_options);
            result += member_initializers;
            const auto hoisted = take_try_hoists(body_options);
            if (is_assignment_operator(declaration)) {
                result += "{ " + prefix + that_assignments + hoisted + body + "; return *this; }";
            }
            else if (uses_expression_statement_body(declaration)) {
                result += "{ " + prefix + hoisted + body + "; }";
            }
            else {
                result += "{ " + prefix + hoisted +
                    value_return_check(body_options, {declaration.symbol, text_key::part::body},
                                       body) +
                    "return " + body + "; }";
            }
        }
        else if (is_pure_virtual(declaration)) {
            result += "= 0;";
        }
        else if (is_defaulted_comparison(declaration)) {
            result += "= default;";
        }
        else {
            result += member_initializers + (is_assignment_operator(declaration)
                ? " { " + that_assignments + "return *this; }" : " { }");
        }
        return result;
    }
    case declaration_kind::namespace_node: {
        auto result = line_text{"namespace " + render_name(declaration.name) + " {"};
        for (const auto& member : declaration.members) {
            result += "\n";
            result.append(render_declaration(member, expression_options));
        }
        result += "\n}";
        return result;
    }
    case declaration_kind::concept_declaration:
        return render_template_prefix(declaration) + render_constraint(declaration) +
            "concept " + render_name(declaration.name) + " = " +
            render_cpp1_expression(declaration.body, nonlocal_options(expression_options)) + ";";
    case declaration_kind::type: {
        const auto access_name = [](access_kind access, bool data_member) -> std::string_view {
            switch (access) {
            case access_kind::public_access: return "public";
            case access_kind::protected_access: return "protected";
            case access_kind::private_access: return "private";
            case access_kind::default_access: return data_member ? "private" : "public";
            }
            return data_member ? "private" : "public";
        };
        auto fields = line_text{};
        auto bases = std::string{};
        auto helpers = std::string{};
        auto current_access = std::string_view{"public"};
        // Separates a member so it begins on its source line: with newlines if
        // that is still ahead, else with a `#line` if it is after the previous
        // member's line. A written member shares the line the output is on; a
        // generated one, made by a metafunction or with the type's span, begins
        // a new line. The type begins after its base helpers, one per line.
        auto lines = lowering_detail::line_counter{declaration.span.begin.line +
            static_cast<std::uint32_t>(std::count_if(
                declaration.members.begin(), declaration.members.end(),
                [](const ir_declaration& member) { return !member.base_helper.empty(); }))};
        auto member_line = declaration.span.begin.line;
        const auto separate = [&](const ir_declaration& member, std::string_view same_line) {
            const auto line = lines.at_end(fields);
            const auto target = member.span.begin.line;
            const auto previous = std::exchange(member_line, std::max(member_line, target));
            const auto generated = declaration.is_generated || member.is_generated ||
                (member.span.begin.line == declaration.span.begin.line &&
                 member.span.begin.column == declaration.span.begin.column);
            if (target > line) {
                fields += std::string(target - line, '\n');
            }
            else if (target > previous) {
                fields += "\n";
                fields.begin_line(target);
            }
            else {
                fields += target == line && !generated ? std::string_view{" "} : same_line;
            }
        };
        // A member type alias or `==` constant that a member's declaration names
        // is declared before that member, and a nested type that a member needs
        // complete is defined before it, each after the later ones it names.
        auto aliases = std::multimap<std::string, const ir_declaration*>{};
        auto nested_types = std::multimap<std::string, const ir_declaration*>{};
        for (const auto& member : declaration.members) {
            if (!member.alias.empty() && (member.kind == declaration_kind::type ||
                                          member.kind == declaration_kind::object) &&
                !is_alias_defined_out_of_line(declaration, member)) {
                aliases.emplace(member.name, &member);
            }
            if (member.kind == declaration_kind::type && member.alias.empty()) {
                nested_types.emplace(member.name, &member);
            }
        }
        auto order = std::vector<const ir_declaration*>{};
        auto placed = std::set<const ir_declaration*>{};
        const auto place = [&](const auto& self, const ir_declaration& member) -> void {
            if (!placed.insert(&member).second) return;
            for (const auto& name : member_declaration_names(member)) {
                for (auto [entry, last] = aliases.equal_range(name); entry != last; ++entry) {
                    self(self, *entry->second);
                }
            }
            for (const auto& name : complete_type_names(member)) {
                for (auto [entry, last] = nested_types.equal_range(name); entry != last; ++entry) {
                    self(self, *entry->second);
                }
            }
            order.push_back(&member);
        };
        for (const auto& member : declaration.members) place(place, member);
        // A nested type that an earlier member's declaration names is declared
        // first, with its template parameter defaults.
        auto earlier_names = std::set<std::string>{};
        auto declared_first = std::set<const ir_declaration*>{};
        for (const auto* member : order) {
            if (member->kind == declaration_kind::type && member->alias.empty() &&
                earlier_names.contains(member->name)) {
                declared_first.insert(member);
                const auto member_access = access_name(member->access, false);
                if (member_access != current_access) {
                    fields += std::string{member_access} + ": ";
                    current_access = member_access;
                }
                fields += render_template_prefix(*member) + render_constraint(*member) +
                    "struct " + render_name(member->name) + "; ";
            }
            earlier_names.merge(member_declaration_names(*member));
        }
        for (const auto* placed_member : order) {
            const auto& member = *placed_member;
            if (member.kind == declaration_kind::object && member.name == "this") {
                bases += (bases.empty() ? " : public " : ", public ") +
                    render_cpp1_type(member.type);
                continue;
            }
            // A helper base compares as its member does, for the type's
            // defaulted comparisons.
            if (!member.base_helper.empty()) {
                helpers += "struct " + member.base_helper + " { " +
                    render_cpp1_type_declaration(member.type, render_name(member.name)) +
                    (member.body.empty() ? "" : " " +
                        render_object_initializer(member.body, expression_options)) +
                    "; auto operator<=>(" + member.base_helper +
                    " const&) const = default; };\n";
                bases += (bases.empty() ? " : public " : ", public ") + member.base_helper;
            }
            const auto member_rendered = member.special_member !=
                ir_declaration::special_member_kind::none
                ? render_declaration(member, expression_options, members_mode) : line_text{};
            const auto member_access = access_name(member.access,
                member.kind == declaration_kind::object && member.alias.empty());
            // Each member begins on its line, after an access label when it changes.
            auto label = std::string{};
            if (member_access != current_access) {
                label = std::string{member_access} + ": ";
                current_access = member_access;
            }
            // Separates the member, which begins with `label`; a data member on
            // the source line of the previous one shares its line.
            const auto separator = [&] {
                separate(member, member.kind == declaration_kind::object ? " " : "\n");
                return label;
            };
            if (!member_rendered.empty()) {
                fields += separator();
                fields.append(member_rendered);
            }
            else if (member.kind == declaration_kind::function ||
                member.kind == declaration_kind::type ||
                member.kind == declaration_kind::concept_declaration ||
                member.kind == declaration_kind::namespace_node) {
                fields += separator();
                if (declared_first.contains(&member)) {
                    auto definition = member;
                    definition.is_type_member = false;
                    fields.append(render_declaration(definition, expression_options, members_mode));
                }
                else {
                    fields.append(render_declaration(member, expression_options, members_mode));
                }
            }
            else if (!member.base_helper.empty()) {
                fields += " " + label + "using " + member.base_helper + "::" + render_name(member.name) + ";";
            }
            else if (member.is_lazy) {
                fields += separator() + "static " +
                    render_lazy_declarator(member.type, render_name(member.name)) + ";";
            }
            else if (is_alias_defined_out_of_line(declaration, member)) {
                fields += separator() + render_template_prefix(member) + render_constraint(member) +
                    "static const " +
                    render_cpp1_type_declaration(member.type, render_name(member.name)) + ";";
            }
            else if (!member.alias.empty()) {
                fields += separator() + render_template_prefix(member) +
                    render_constraint(member) + "static constexpr " + (member.type.is_wildcard()
                        ? std::string{"auto "} + render_name(member.name)
                        : render_cpp1_type_declaration(member.type, render_name(member.name))) +
                    " " +
                    render_object_initializer(member.alias, nonlocal_options(expression_options)) + ";";
            }
            else {
                fields += separator() + (member.type.is_wildcard()
                    ? std::string{"auto "} + render_name(member.name)
                    : render_cpp1_type_declaration(member.type, render_name(member.name)));
                if (!member.body.empty()) {
                    fields += " " + render_object_initializer(member.body, expression_options);
                }
                fields += ";";
            }
        }
        auto result = line_text{helpers +
            render_template_prefix(declaration, declaration.is_type_member) +
            render_constraint(declaration) + "struct " + render_name(declaration.name) +
            (declaration.is_final ? " final" : "") + bases + " {"};
        result.append(std::move(fields));
        result += "\n};";
        return result;
    }
    case declaration_kind::object:
        if (declaration.scope != declaration_scope::function_scope &&
            declaration.scope != declaration_scope::type_scope) {
            expression_options.nonlocal = true;
        }
        if (!declaration.alias.empty()) {
            const auto storage = declaration.scope == declaration_scope::function_scope
                ? std::string{"constexpr "}
                : std::string{"inline constexpr "};
            const auto name = render_name(declaration.name);
            // A typed list `== (1, 2)` or `== ()` is list-initialized.
            const auto braced = !declaration.type.is_wildcard() &&
                is_braced_initializer(parsed_expression{declaration.alias});
            auto result = render_template_prefix(declaration) + render_constraint(declaration) +
                storage + (declaration.type.is_wildcard()
                    ? render_deduced_type(declaration.type) + " " + name
                    : render_cpp1_type_declaration(declaration.type, name)) +
                (braced ? " " + render_object_initializer(declaration.alias, expression_options)
                        : " = " + render_cpp1_expression(declaration.alias, expression_options)) +
                ";";
            if (const auto assertion = deduced_constraint_assertion(declaration.type, name);
                !assertion.empty()) {
                result += "\n" + assertion;
            }
            return result;
        }
    {
        const auto name = render_name(declaration.name);
        if (declaration.is_lazy) {
            return render_template_prefix(declaration) + render_constraint(declaration) +
                render_lazy_declarator(declaration.type, name) +
                render_lazy_initializer(declaration.type, declaration.body, expression_options) +
                ";";
        }
        auto result = render_template_prefix(declaration) + render_constraint(declaration) +
            (declaration.type.is_wildcard()
                ? render_deduced_type(declaration.type) + " " + name
                : render_cpp1_type_declaration(declaration.type, name)) +
            (declaration.body.empty() ? ";" : " " +
             render_object_initializer(declaration.body, expression_options) + ";");
        if (const auto assertion = deduced_constraint_assertion(declaration.type, name);
            !assertion.empty()) {
            result += "\n" + assertion;
        }
        return result;
    }
    case declaration_kind::using_declaration:
        break;
    }
    return render_name(declaration.name);
}

// A rendered definition and the source line it begins on.
struct line_chunk final {
    std::uint32_t line = 0;
    line_text text;
    std::uint32_t end_line = 0;
};

// The out-of-line definitions of the member functions of `type` and its nested
// types, each at its own line; `heads` holds the template heads of the
// enclosing types.
auto render_out_of_line_members(const ir_declaration& type,
                                lowering_options expression_options,
                                std::vector<line_chunk>& result,
                                std::string heads = {}, std::string qualifier = {}) -> void {
    const auto in_template = !heads.empty();
    heads += render_template_prefix(type, false) + render_constraint(type);
    const auto outer = qualifier;
    qualifier += render_name(type.name) + render_template_arguments(type) + "::";
    for (const auto& member : type.members) {
        if (member.kind == declaration_kind::type && member.alias.empty()) {
            render_out_of_line_members(member, expression_options, result, heads, qualifier);
        }
        else if (is_defined_out_of_line(member, expression_options)) {
            result.push_back({member.span.begin.line,
                render_declaration(member, expression_options, member_mode::definition,
                                   qualifier, heads), member.span.end.line});
        }
        else if (member.is_lazy || is_alias_defined_out_of_line(type, member)) {
            // A nested type's name is qualified outside its enclosing type.
            // Nested type names are qualified outside their enclosing type.
            auto alias_type = member.type;
            if (!outer.empty()) {
                qualify_name(alias_type, type.name, parse_type(outer.substr(0, outer.size() - 2)),
                             in_template);
            }
            if (member.is_lazy) {
                result.push_back({member.span.begin.line,
                    heads + "inline " +
                    render_lazy_declarator(alias_type, qualifier + render_name(member.name)) +
                    render_lazy_initializer(alias_type, member.body,
                                            nonlocal_options(expression_options)) + ";",
                    member.span.end.line});
                continue;
            }
            const auto declarator = render_cpp1_type_declaration(
                alias_type, qualifier + render_name(member.name));
            result.push_back({member.span.begin.line,
                heads + render_template_prefix(member) + render_constraint(member) +
                "inline constexpr " + declarator + " " +
                render_object_initializer(member.alias, nonlocal_options(expression_options)) +
                ";", member.span.end.line});
        }
    }
}

auto render_declaration(const ir_declaration& declaration,
                        lowering_options expression_options, member_mode mode,
                        std::string_view qualifier, std::string_view heads) -> line_text {
    if (declaration.kind == declaration_kind::function && mode != member_mode::inline_body &&
        !is_defined_out_of_line(declaration, expression_options)) {
        mode = member_mode::inline_body;
    }
    // An out-of-line definition repeats the template heads of its enclosing types.
    auto result = line_text{mode == member_mode::definition ? std::string{heads} : std::string{}};
    result.append(
        render_declaration_without_postfix(declaration, expression_options, mode, qualifier));
    // Each statement takes the hoists of its `?` operands, and `?` appears nowhere else.
    assert(expression_options.try_hoists == nullptr ||
           expression_options.try_hoists->statements.empty());
    if (const auto postfix = postfix_counterpart(declaration)) {
        if (mode == member_mode::declaration) {
            result += "\n" + render_postfix_signature(*postfix) + ";";
            return result;
        }
        if (mode == member_mode::definition) {
            result += "\n\n" + std::string{heads} + render_postfix_signature(*postfix, qualifier);
        }
        else {
            result += "\n" + render_postfix_signature(*postfix);
        }
        result += " { auto ret = " +
            postfix_operand(*postfix) + "; " + postfix->name.substr(8) + postfix_operand(*postfix) +
            "; return ret; }";
    }
    return result;
}

}

// `options` with the out parameters and deferred locals that the names in
// `text` find at its point, which are read through `.value()`.
auto with_wrapped_names(lowering_options options, const parsed_expression& text)
    -> lowering_options {
    if (options.scopes == nullptr || !options.point.valid()) return options;
    for (const auto& token : text.tree().tokens) {
        if (token.kind != expression_token_kind::identifier) continue;
        const auto* found = options.scopes->lookup(token.text, options.point);
        if (found == nullptr) continue;
        const auto* parameter = found->declared_parameter;
        const auto* statement = found->declaring_statement;
        const auto* local = statement != nullptr &&
                statement->is<statement::declaration_form>()
            ? &statement->as<statement::declaration_form>().declaration : nullptr;
        const auto out = found->kind == scope_model::entry_kind::parameter &&
            parameter != nullptr && parameter->passing == passing_style::out &&
            !is_this_parameter(*parameter);
        const auto deferred = local != nullptr && *local && !(*local)->is_alias &&
            (*local)->initializer.empty();
        if (!out && !deferred) continue;
        auto type = found->type.is_wildcard() ? std::string{out ? "" : "auto"}
                                              : render_cpp1_type(found->type);
        options.wrapped_parameters.push_back({token.text, std::move(type), out, found});
    }
    return options;
}

// `[captures](params) mutable -> R { body }` from the function declaration
// standing in for an unnamed function expression.
// As upstream, a lambda directly inside a member function with `this` also
// captures by reference.
auto render_function_expression(const ir_declaration& declaration,
                                const function_expression& expression,
                                lowering_options expression_options,
                                bool capture_by_reference = false) -> line_text {
    // Captures are evaluated where the expression appears.
    const auto captures_options = expression_options.scopes == nullptr ? expression_options
        : options_at(expression_options, expression_options.scopes->outside(declaration.symbol));
    auto head = std::string{capture_by_reference ? "[&" : "["};
    for (auto index = std::size_t{0}; index < expression.captures.size(); ++index) {
        const auto& capture = expression.captures[index];
        if (head.size() > 1) head += ", ";
        head += capture_name(capture, captures_options) + " = " +
            render_cpp1_expression(capture.expression,
            text_options(with_wrapped_names(captures_options, capture.expression),
                         {declaration.symbol, text_key::part::capture,
                          static_cast<std::uint32_t>(index)}));
    }
    head += "]";
    // A lambda is implicitly constexpr, as upstream.
    if (const auto parameters = cpp1_template_parameters(declaration, true); !parameters.empty()) {
        head += render_template_parameter_list(parameters);
    }
    // A `decltype(x)` in the signature or `requires` clause of a deferred local
    // or `out` parameter outside the lambda is its declared type.
    auto signature = declaration;
    for (auto& parameter : signature.parameters) {
        parameter.type = with_declared_types(std::move(parameter.type), captures_options);
    }
    signature.type = with_declared_types(std::move(signature.type), captures_options);
    head += "(" + render_function_parameters(signature, true) + ")";
    // A `==` function expression's call operator stays const.
    if (!expression.captures.empty() && !declaration.is_constexpr) head += " mutable";
    expression_options.nonlocal = false;
    expression_options.lambda_head = head + " -> " + function_return_type(signature) +
        render_function_constraint(signature,
            with_wrapped_names(captures_options, declaration.requires_clause).wrapped_parameters);
    return render_declaration(declaration, expression_options);
}

// Replaces each identifier in `text` that names a function expression with
// its lambda, whose own function expressions are substituted in turn; `render`
// is called after the text around the name has been rendered. Literals and
// comments are copied unchanged, and each lambda is scanned once. The text
// pads the lines a lambda spans with blank lines after the line it ends, which
// its own lines, including its `#line` directives, replace. A line after a
// lambda that the padding does not bring back to the line it had in `text`
// begins with a `#line`. `text` begins on source line `line`, or 0 if unknown.
auto substitute_function_expressions(const line_text& text,
    const std::function<std::optional<line_text>(const std::string&)>& render,
    std::uint32_t line = 0) -> line_text {
    auto result = line_text{};
    auto padding = std::size_t{0};
    // The line that the compiler counts at the end of `result`, and the one it
    // counts there in `text`, once a line is known.
    auto known = line != 0;
    auto actual = line;
    auto expected = line;
    const auto newlines = [](std::string_view value) {
        return static_cast<std::uint32_t>(std::count(value.begin(), value.end(), '\n'));
    };
    for (const auto& piece : text.pieces()) {
        auto position = std::size_t{0};
        if (piece.line != 0) {
            result.begin_line(piece.line, piece.end);
            known = true;
            actual = expected = piece.line;
            // The newline that ends the `#line` directive ends the padded line.
            for (; padding > 0 && position < piece.text.size() && piece.text[position] == '\n';
                 --padding) {
                ++position;
            }
            padding = 0;
        }
        const auto& source = piece.text;
        const auto resynchronize = [&] {
            if (known && actual != expected && position < source.size()) {
                result.begin_line(expected);
                actual = expected;
            }
        };
        while (position < source.size()) {
            if (source[position] == '\n' && padding > 0) {
                result += "\n";
                ++actual;
                ++expected;
                ++position;
                for (; padding > 0 && position < source.size() && source[position] == '\n';
                     --padding) {
                    ++position;
                    ++expected;
                }
                padding = 0;
                resynchronize();
                continue;
            }
            auto end = std::max(detail::lexeme_at(source, position).end, position);
            if (end == position && (std::isalpha(static_cast<unsigned char>(source[position])) ||
                                    source[position] == '_')) {
                end = position + 1;
                while (end < source.size() && detail::is_identifier_continue(source[end])) ++end;
                if (const auto found = render(source.substr(position, end - position))) {
                    // `x[[]...]` would begin an attribute.
                    const auto& tail = result.tail();
                    const auto previous = tail.find_last_not_of(" \t\n");
                    const auto subscript = previous != std::string::npos && tail[previous] == '[';
                    auto lambda = substitute_function_expressions(*found, render,
                                                                  known ? actual : 0);
                    for (const auto& lambda_piece : lambda.pieces()) {
                        padding += newlines(lambda_piece.text);
                        if (lambda_piece.line != 0) {
                            ++padding;
                            actual = lambda_piece.line;
                        }
                        actual += newlines(lambda_piece.text);
                    }
                    if (subscript) result += "(";
                    result.append(std::move(lambda));
                    if (subscript) result += ")";
                    position = end;
                    continue;
                }
            }
            if (end == position) ++end;
            const auto copied = std::string_view{source}.substr(position, end - position);
            result += copied;
            actual += newlines(copied);
            expected += newlines(copied);
            position = end;
            if (copied == "\n") resynchronize();
        }
    }
    return result;
}

// Names that Cpp1 lines declare as types, namespaces or concepts, or bring in
// with a using-declaration: `struct X`, `class X`, `union X`, `enum (class) X`,
// `using X =`, `namespace X`, `concept X`, `using a::X;` and `typedef ... X;`.
auto cpp1_declared_names(const source_text& source) -> std::set<std::string> {
    auto result = std::set<std::string>{};
    // The words of the Cpp1 lines; a declaration may span lines.
    auto words = std::vector<std::string_view>{};
    for (const auto& line : source.lines()) {
        if (line.category != source_category::cpp1) {
            words.emplace_back(";");
            continue;
        }
        const auto text = std::string_view{line.text};
        for (auto position = std::size_t{0}; position < text.size();) {
            const auto value = text[position];
            if (std::isalpha(static_cast<unsigned char>(value)) || value == '_') {
                auto end = position + 1;
                while (end < text.size() &&
                       (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_')) {
                    ++end;
                }
                words.push_back(text.substr(position, end - position));
                position = end;
            }
            else {
                if (value == '=') words.emplace_back("=");
                if (value == ';') words.emplace_back(";");
                ++position;
            }
        }
    }
    for (auto index = std::size_t{0}; index + 1 < words.size(); ++index) {
        const auto word = words[index];
        if (word == "struct" || word == "class" || word == "union" || word == "enum") {
            auto name = index + 1;
            if (word == "enum" && (words[name] == "class" || words[name] == "struct")) ++name;
            if (name < words.size() && words[name] != "=") result.emplace(words[name]);
        }
        else if (word == "using" && index + 2 < words.size() && words[index + 2] == "=") {
            result.emplace(words[index + 1]);
        }
        else if (word == "namespace" || word == "concept") {
            if (words[index + 1] != "=" && words[index + 1] != ";") {
                result.emplace(words[index + 1]);
            }
        }
        else if (word == "using" || word == "typedef") {
            auto end = index + 1;
            while (end < words.size() && words[end] != ";" && words[end] != "=") ++end;
            if (end < words.size() && words[end] == ";") result.emplace(words[end - 1]);
        }
    }
    return result;
}

// The leading lines of a module unit: an optional global module fragment
// (`module;` and preprocessor lines), the module declaration, and the imports
// that follow it. Generated includes go in the global module fragment, and
// generated declarations after the prologue.
struct module_prologue final {
    // The first line after `module;`, or the module declaration without one.
    std::size_t fragment_begin = 0;
    std::size_t end = 0;
};

auto find_module_prologue(const source_text& source) -> std::optional<module_prologue> {
    const auto& lines = source.lines();
    const auto text_at = [&](std::size_t index) {
        auto view = std::string_view{lines[index].text};
        while (!view.empty() && (view.front() == ' ' || view.front() == '\t')) view.remove_prefix(1);
        return view;
    };
    const auto skippable = [&](std::size_t index) {
        return lines[index].category == source_category::empty ||
            lines[index].category == source_category::comment;
    };
    const auto is_declaration = [&](std::size_t index) {
        const auto view = text_at(index);
        return lines[index].category == source_category::cpp1 && view != "module;" &&
            (view.starts_with("module ") || view.starts_with("export module "));
    };
    auto index = std::size_t{0};
    while (index < lines.size() && skippable(index)) ++index;
    if (index == lines.size()) return std::nullopt;
    auto result = module_prologue{};
    if (lines[index].category == source_category::cpp1 && text_at(index) == "module;") {
        result.fragment_begin = ++index;
        while (index < lines.size() &&
               (skippable(index) || lines[index].category == source_category::preprocessor)) {
            ++index;
        }
        if (index == lines.size() || !is_declaration(index)) return std::nullopt;
    }
    else if (is_declaration(index)) {
        result.fragment_begin = index;
    }
    else {
        return std::nullopt;
    }
    result.end = ++index;
    while (index < lines.size() &&
           (skippable(index) || lines[index].category == source_category::import ||
            (lines[index].category == source_category::cpp1 &&
             text_at(index).starts_with("export import ")))) {
        if (!skippable(index)) result.end = index + 1;
        ++index;
    }
    return result;
}

auto emit_cpp1(const ir_program& input, const source_text& source,
               const token_list& tokens, const translation_options& options,
               const initialization_plan& plan) -> emit_result {
    return emit_cpp1(input, scope_model{input}, source, tokens, options, plan);
}

auto emit_cpp1(const ir_program& input, const scope_model& scopes, const source_text& source,
               const token_list& tokens, const translation_options& options,
               const initialization_plan& plan) -> emit_result {
    // As upstream, a file without Cpp2 passes through unchanged.
    if (!source.has_cpp2() && options.standard_library == standard_library_mode::inferred) {
        auto result = emit_result{source.data(), {}};
        for (std::uint32_t line = 1; line <= source.lines().size(); ++line) {
            result.source_map.push_back({{line, 1}, {line, 1}});
        }
        return result;
    }
    // Function expressions are rendered where they are used, not as declarations.
    auto program = ir_program{};
    auto lambda_declarations = std::vector<const ir_declaration*>{};
    for (const auto& declaration : input.declarations) {
        if (is_function_expression_name(declaration.name)) {
            lambda_declarations.push_back(&declaration);
        }
        else {
            program.declarations.push_back(declaration);
        }
    }
    auto expression_options = lowering_options{};
    expression_options.check_division_by_zero = options.check_division_by_zero;
    expression_options.check_subscripts = options.check_subscripts;
    expression_options.check_null_dereference = options.check_null_dereference;
    expression_options.check_comparisons = options.check_comparisons;
    expression_options.experiments = options.experiments;
    auto try_hoists = try_hoisting{};
    if (options.experiments.enabled("try-operator")) expression_options.try_hoists = &try_hoists;
    expression_options.check_arithmetic = options.experiments.enabled("checked-arithmetic");
    expression_options.plan = &plan;
    expression_options.scopes = &scopes;
    auto output = lowering_detail::output_builder{options.source_path};
    const auto prologue = find_module_prologue(source);
    if (prologue) output.add_raw("module;\n");
    const auto all_std = options.standard_library != standard_library_mode::inferred;
    if (source.has_cpp2() || all_std) {
        if (options.emit_version_info) {
            output.add_raw("// Generated by cppfrontier " CPPFRONTIER_VERSION "\n");
        }
        if (!options.header_guard.empty()) {
            output.add_raw("#ifndef " + options.header_guard + "\n#define " +
                           options.header_guard + "\n\n");
        }
        if (options.standard_library == standard_library_mode::include_all) {
            output.add_raw("#define CPP2_INCLUDE_STD         Yes\n");
        }
        else if (options.standard_library == standard_library_mode::import_all) {
            output.add_raw("#define CPP2_IMPORT_STD          Yes\n");
        }
        if (!options.exceptions) output.add_raw("#define CPP2_NO_EXCEPTIONS       Yes\n");
        if (!options.rtti) output.add_raw("#define CPP2_NO_RTTI             Yes\n");
        output.add_include("\"cpp2util.h\"");
    }
    const auto uses_metaclass = [&](std::string_view name) {
        const auto uses = [&](const auto& self, const ir_declaration& declaration) -> bool {
            return std::find(declaration.metaclasses.begin(), declaration.metaclasses.end(),
                             name) != declaration.metaclasses.end() ||
                std::any_of(declaration.members.begin(), declaration.members.end(),
                            [&](const ir_declaration& member) { return self(self, member); });
        };
        return std::any_of(program.declarations.begin(), program.declarations.end(),
                           [&](const ir_declaration& declaration) {
                               return uses(uses, declaration);
                           });
    };
    if (uses_metaclass("regex")) output.add_include("\"cpp2regex.h\"");
    if (options.experiments.enabled("newtype") && uses_metaclass("unit")) {
        output.add_include("\"cpp2units.h\"");
    }
    // Every mention of a lazy object reads it through `.value()`.
    auto lazy_objects = std::vector<lazy_object>{};
    if (options.experiments.enabled("lazy")) {
        ir_detail::visit_declarations(program.declarations, [&](const ir_declaration& declaration) {
            if (declaration.is_lazy) lazy_objects.push_back({declaration.name, declaration.symbol});
        });
        if (!lazy_objects.empty()) output.add_include("\"cpp2lazy.h\"");
    }
    const auto lazy_scope = lazy_objects_scope{std::move(lazy_objects)};
    for (const auto& include : autodiff_includes(program.declarations)) {
        output.add_include(include);
    }
    if (!all_std) {
        for (const auto& include : standard_include_directives(source, tokens, options)) {
            output.add_include(include);
        }
    }

    // Phases follow upstream: type declarations, then type definitions and
    // function declarations in source order, then function definitions. The
    // types defined last follow the second phase, and trailing Cpp1 text the
    // third.
    auto function_names = std::set<std::string>{};
    auto type_names = std::set<std::string>{};
    auto concept_names = std::set<std::string>{};
    auto collect_names = [&](const auto& self, const ir_declaration& declaration) -> void {
        // A using-declaration names a namespace member, not a member function.
        if (declaration.kind == declaration_kind::function &&
            declaration.scope != declaration_scope::type_scope) {
            function_names.insert(declaration.name);
        }
        if (declaration.kind == declaration_kind::type ||
            declaration.kind == declaration_kind::concept_declaration) {
            type_names.insert(declaration.name);
        }
        if (declaration.kind == declaration_kind::concept_declaration) {
            concept_names.insert(declaration.name);
        }
        if (declaration.kind != declaration_kind::namespace_node &&
            declaration.kind != declaration_kind::type) {
            return;
        }
        for (const auto& member : declaration.members) self(self, member);
    };
    for (const auto& declaration : program.declarations) collect_names(collect_names, declaration);
    auto function_declaration = [&](const ir_declaration& declaration) {
        const auto postfix = postfix_counterpart(declaration);
        return render_named_return_struct(declaration) + render_template_prefix(declaration) +
            nodiscard_prefix(declaration) +
            (declaration.is_constexpr || !declaration.alias.empty() ? "constexpr " : "") +
            render_function_signature(declaration, true) + render_function_constraint(declaration) +
            ";" + (postfix ? "\n" + render_postfix_signature(*postfix) + ";" : std::string{});
    };
    // Function declarations whose signatures name no Cpp2 type or concept,
    // namespace-scope object, namespace alias, using-declaration, function that
    // is not hoisted, or anything Cpp1 lines declare, and that follow no
    // using-directive, are hoisted ahead of all type definitions; the rest
    // wait in source order for the names they may need.
    auto unhoisted_names = cpp1_declared_names(source);
    // The first line of a using-directive, `using namespace n;` or `using n::_;`.
    auto directive_line = std::numeric_limits<std::uint32_t>::max();
    for (std::size_t index = 0; index < source.lines().size(); ++index) {
        const auto& line = source.lines()[index];
        auto text = std::string_view{line.text};
        text.remove_prefix(std::min(text.size(), text.find_first_not_of(" \t")));
        if (line.category != source_category::cpp1 || !text.starts_with("using")) continue;
        text.remove_prefix(5);
        const auto blank = text.find_first_not_of(" \t");
        if (blank != 0 && blank != std::string_view::npos &&
            text.substr(blank).starts_with("namespace")) {
            directive_line = static_cast<std::uint32_t>(index + 1);
            break;
        }
    }
    const auto collect_hoisting_limits = [&](const auto& self,
                                             const ir_declaration& declaration) -> void {
        if (declaration.kind == declaration_kind::using_declaration) {
            const auto last = using_declared_name(declaration);
            if (last == "_") directive_line = std::min(directive_line, declaration.span.begin.line);
            else unhoisted_names.insert(last);
        }
        if (declaration.kind == declaration_kind::object) unhoisted_names.insert(declaration.name);
        if (declaration.kind != declaration_kind::namespace_node) return;
        if (!declaration.alias.empty()) unhoisted_names.insert(declaration.name);
        for (const auto& member : declaration.members) self(self, member);
    };
    for (const auto& declaration : program.declarations) {
        collect_hoisting_limits(collect_hoisting_limits, declaration);
    }
    auto is_namespace = [](const ir_declaration& declaration) {
        return declaration.kind == declaration_kind::namespace_node && declaration.alias.empty();
    };
    // Each name of a function that is not hoisted becomes unhoisted in turn,
    // with the functions whose signatures name it.
    auto hoisted = std::set<const ir_declaration*>{};
    {
        auto waiting = std::multimap<std::string, const ir_declaration*>{};
        auto unhoisted = std::vector<std::string>{};
        // A function expression, as in a default argument, may name anything
        // declared before it.
        const auto names_late = [&](const std::set<std::string>& used) {
            return std::any_of(used.begin(), used.end(), [&](const auto& name) {
                return type_names.contains(name) || unhoisted_names.contains(name) ||
                    is_function_expression_name(name);
            });
        };
        // A signature naming a function declared after it waits for that declaration.
        auto last_function_at = std::map<std::string, std::pair<std::uint32_t, std::uint32_t>>{};
        ir_detail::visit_declarations(program.declarations, [&](const ir_declaration& declaration) {
            if (declaration.kind != declaration_kind::function) return;
            auto& last = last_function_at[declaration.name];
            last = std::max(last, std::pair{declaration.span.begin.line, declaration.span.begin.column});
        });
        const auto names_later_function = [&](const ir_declaration& declaration,
                                              const std::set<std::string>& used) {
            const auto begin = std::pair{declaration.span.begin.line, declaration.span.begin.column};
            return std::any_of(used.begin(), used.end(), [&](const auto& name) {
                const auto found = last_function_at.find(name);
                return found != last_function_at.end() && found->second > begin;
            });
        };
        const auto collect = [&](const auto& self, const ir_declaration& declaration) -> void {
            if (declaration.kind == declaration_kind::function) {
                const auto used = signature_names(declaration);
                if (declaration.span.begin.line > directive_line || names_late(used) ||
                    names_later_function(declaration, used)) {
                    unhoisted.push_back(declaration.name);
                    return;
                }
                hoisted.insert(&declaration);
                for (const auto& name : used) waiting.emplace(name, &declaration);
            }
            if (!is_namespace(declaration)) return;
            for (const auto& member : declaration.members) self(self, member);
        };
        for (const auto& declaration : program.declarations) collect(collect, declaration);
        while (!unhoisted.empty()) {
            const auto name = std::move(unhoisted.back());
            unhoisted.pop_back();
            if (!unhoisted_names.insert(name).second) continue;
            for (auto [entry, last] = waiting.equal_range(name); entry != last; ++entry) {
                if (hoisted.erase(entry->second) != 0) unhoisted.push_back(entry->second->name);
            }
        }
    }
    auto is_hoisted_function = [&](const ir_declaration& declaration) {
        return hoisted.contains(&declaration);
    };
    // A using-declaration of a Cpp2 function must follow the function's declaration.
    auto is_function_using = [&](const ir_declaration& declaration) {
        return declaration.kind == declaration_kind::using_declaration &&
            function_names.contains(using_declared_name(declaration));
    };
    auto contains = [&](const auto& self, const ir_declaration& declaration,
                        const auto& predicate) -> bool {
        if (predicate(declaration)) return true;
        if (!is_namespace(declaration)) return false;
        return std::any_of(declaration.members.begin(), declaration.members.end(),
            [&](const auto& member) { return self(self, member, predicate); });
    };
    auto is_type = [](const ir_declaration& declaration) {
        return declaration.kind == declaration_kind::type && declaration.alias.empty();
    };
    // Concepts are emitted with the type declarations, which may use them as
    // template parameter constraints.
    auto is_concept = [](const ir_declaration& declaration) {
        return declaration.kind == declaration_kind::concept_declaration;
    };
    // The identifier tokens by position, for the names in a declaration's text.
    auto identifiers = std::vector<const token*>{};
    for (const auto& value : tokens) {
        if (value.kind == token_kind::identifier) identifiers.push_back(&value);
    }
    const auto at = [](source_position value) { return std::pair{value.line, value.column}; };
    std::stable_sort(identifiers.begin(), identifiers.end(), [&](const auto* left, const auto* right) {
        return at(left->span.begin) < at(right->span.begin);
    });
    // Whether the text of `declaration` names a Cpp2 type or concept, or
    // anything not declared ahead of all type definitions.
    const auto names_late = [&](const ir_declaration& declaration) {
        auto first = std::lower_bound(identifiers.begin(), identifiers.end(),
            at(declaration.span.begin),
            [&](const auto* value, const auto& position) { return at(value->span.begin) < position; });
        for (; first != identifiers.end() && at((*first)->span.begin) <= at(declaration.span.end);
             ++first) {
            // Each part of a qualified name `N::x`.
            const auto& text = (*first)->text;
            for (std::size_t begin = 0; begin < text.size();) {
                const auto end = std::min(text.find("::", begin), text.size());
                const auto part = text.substr(begin, end - begin);
                if (type_names.contains(part) || unhoisted_names.contains(part)) return true;
                begin = end + 2;
            }
        }
        return false;
    };
    // A hoisted constexpr function that names nothing declared later is defined
    // where it appears, so Cpp1 code after it, such as a `static_assert`, and
    // constants can evaluate it. The others are defined with the other functions.
    auto is_defined_in_place = [&](const ir_declaration& declaration) {
        return declaration.is_constexpr && is_hoisted_function(declaration) &&
            !names_late(declaration);
    };
    // Likewise a constexpr member function that names nothing declared later is
    // defined in its type.
    auto members_defined_late = std::set<const ir_declaration*>{};
    ir_detail::visit_declarations(program.declarations, [&](const ir_declaration& declaration) {
        if (declaration.kind == declaration_kind::function && declaration.is_constexpr &&
            declaration.scope == declaration_scope::type_scope && names_late(declaration)) {
            members_defined_late.insert(&declaration);
        }
    });
    expression_options.members_defined_late = &members_defined_late;
    auto is_phase2 = [&](const ir_declaration& declaration) {
        return (declaration.kind == declaration_kind::function &&
                !is_defined_in_place(declaration)) ||
            is_function_using(declaration) || is_deferred_object(declaration);
    };
    // The type forward declarations and hoisted function declarations come
    // first, then the concepts, which may name those types, then (`late`) the
    // forward declarations of types that a concept constrains, each in source order.
    const auto is_constrained_type = [&](const ir_declaration& declaration) {
        const auto names = signature_names(declaration);
        return std::any_of(names.begin(), names.end(),
            [&](const auto& name) { return concept_names.contains(name); });
    };
    auto render_type_declarations = [&](const auto& self, const ir_declaration& declaration,
                                        bool late) -> line_text {
        const auto in_stage = [&](const ir_declaration& value) {
            return is_type(value) ? is_constrained_type(value) == late
                                  : !late && is_hoisted_function(value);
        };
        // Each declaration begins on its source line.
        const auto on_line = [&](line_text text) {
            auto result = line_text{};
            result.begin_line(declaration.span.begin.line, declaration.span.begin.line);
            result.append(std::move(text));
            result += "\n";
            return result;
        };
        if (in_stage(declaration)) {
            if (is_type(declaration)) {
                return on_line(render_template_prefix(declaration) +
                               render_constraint(declaration) + "struct " +
                               render_name(declaration.name) + ";");
            }
            return on_line(function_declaration(declaration));
        }
        if (!is_namespace(declaration) || !contains(contains, declaration, in_stage)) return {};
        auto result = line_text{"namespace " + render_name(declaration.name) + " {\n"};
        for (const auto& member : declaration.members) result.append(self(self, member, late));
        result += "}\n";
        return result;
    };
    // A namespace-scope object or `==` constant that the Cpp1 text of an
    // earlier member of its namespace names is declared before that member,
    // with the later objects and constexpr functions it names, so that a
    // signature or member initializer may use it regardless of order.
    // It moves only when nothing its own declaration names is declared between
    // the two, nor, at file scope, by a Cpp1 line between them.
    struct named_object final {
        const ir_declaration* object = nullptr;
        // The namespaces between the member list and the object.
        std::vector<const ir_declaration*> path;
    };
    struct member_index final {
        std::multimap<std::string, named_object> objects;
        std::multimap<std::string, const ir_declaration*> declared;
    };
    auto moved_objects = std::set<const ir_declaration*>{};
    // The Cpp1 text of each declaration that may move, and the declarations of
    // its member list that the text names.
    struct rendered_object final {
        line_text text;
        std::vector<const named_object*> objects;
        // Each with whether it may move too.
        std::vector<std::pair<const ir_declaration*, bool>> declared;
    };
    auto rendered_objects =
        std::map<std::pair<const ir_declaration*, const member_index*>, rendered_object>{};
    auto member_indexes = std::map<const std::vector<ir_declaration>*, member_index>{};
    const auto position = [](source_position value) { return std::pair{value.line, value.column}; };
    const auto names_in = [](const line_text& rendered) {
        auto names = std::set<std::string>{};
        for (const auto& piece : rendered.pieces()) {
            const auto& text = piece.text;
            for (std::size_t at = 0; at < text.size();) {
                const auto first = static_cast<unsigned char>(text[at]);
                if (!std::isalnum(first) && first != '_') {
                    ++at;
                    continue;
                }
                auto end = at;
                while (end < text.size() &&
                       (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_')) {
                    ++end;
                }
                if (!std::isdigit(first)) names.emplace(text.substr(at, end - at));
                at = end;
            }
        }
        return names;
    };
    const auto index_of = [&](const std::vector<ir_declaration>& members) -> const member_index& {
        const auto [found, added] = member_indexes.try_emplace(&members);
        if (!added) return found->second;
        auto path = std::vector<const ir_declaration*>{};
        const auto add = [&](const auto& self, const std::vector<ir_declaration>& list) -> void {
            for (const auto& member : list) {
                found->second.declared.emplace(
                    member.kind == declaration_kind::using_declaration
                        ? using_declared_name(member) : member.name,
                    &member);
                if (is_namespace(member)) {
                    path.push_back(&member);
                    self(self, member.members);
                    path.pop_back();
                }
                else if ((member.kind == declaration_kind::object && member.cpp1_text.empty()) ||
                         (member.kind == declaration_kind::function &&
                          (is_defined_in_place(member) || !is_hoisted_function(member))) ||
                         member.kind == declaration_kind::type) {
                    found->second.objects.emplace(member.name, named_object{&member, path});
                }
            }
        };
        add(add, members);
        return found->second;
    };
    const auto has_cpp1_line = [&](std::uint32_t from, std::uint32_t to) {
        for (auto line = from; line < to && line <= source.lines().size(); ++line) {
            const auto category = source.lines()[line - 1].category;
            if (category != source_category::cpp2 && category != source_category::empty &&
                category != source_category::comment) {
                return true;
            }
        }
        return false;
    };
    // Calls `emit` with each object that `rendered`, the text of `members[index]`,
    // names and that moves before it, and returns whether any did.
    const auto move_named_objects = [&](const std::vector<ir_declaration>& members,
                                        std::size_t index, const line_text& rendered,
                                        bool file_scope, const auto& render,
                                        const auto& emit) -> bool {
        const auto& user = members[index];
        const auto& entries = index_of(members);
        auto visiting = std::set<const ir_declaration*>{};
        auto moved = false;
        const auto after_user = [&](const ir_declaration& value) {
            return position(value.span.begin) > position(user.span.end);
        };
        const auto try_move = [&](const auto& self, const named_object& entry) -> bool {
            const auto* object = entry.object;
            if (moved_objects.contains(object)) return true;
            if (!visiting.insert(object).second) return false;
            const auto between = [&](const ir_declaration& value) {
                const auto at = position(value.span.begin);
                return position(user.span.begin) <= at && at < position(object->span.begin);
            };
            auto cached = rendered_objects.find({object, &entries});
            if (cached == rendered_objects.end()) {
                auto value = rendered_object{render(*object), {}, {}};
                for (const auto& name : names_in(value.text)) {
                    for (auto [other, last] = entries.objects.equal_range(name); other != last;
                         ++other) {
                        if (other->second.object != object) value.objects.push_back(&other->second);
                    }
                    for (auto [other, last] = entries.declared.equal_range(name); other != last;
                         ++other) {
                        if (other->second == object) continue;
                        const auto [first, end] = entries.objects.equal_range(name);
                        value.declared.push_back({other->second, std::any_of(first, end,
                            [&](const auto& item) { return item.second.object == other->second; })});
                    }
                }
                cached = rendered_objects.emplace(std::pair{object, &entries}, std::move(value)).first;
            }
            const auto& rendering = cached->second;
            if (rendering.text.empty()) return false;
            // A declaration between the two that it names and that is not moved ahead.
            const auto blocks = [&](const auto& other) {
                return between(*other.first) && !moved_objects.contains(other.first) &&
                    std::find(entry.path.begin(), entry.path.end(), other.first) == entry.path.end();
            };
            // One tried already, and not moved, stays.
            const auto movable = [&](const auto& other) {
                return other.second && other.first != &user && after_user(*other.first) &&
                    !visiting.contains(other.first);
            };
            if (std::any_of(rendering.declared.begin(), rendering.declared.end(),
                    [&](const auto& other) { return blocks(other) && !movable(other); })) {
                return false;
            }
            for (const auto* other : rendering.objects) {
                if (after_user(*other->object)) self(self, *other);
            }
            if (std::any_of(rendering.declared.begin(), rendering.declared.end(), blocks)) return false;
            if (file_scope && has_cpp1_line(user.span.begin.line, object->span.begin.line)) {
                return false;
            }
            auto wrapped = line_text{};
            for (const auto* space : entry.path) {
                wrapped += "namespace " + render_name(space->name) + " { ";
            }
            wrapped.append(line_text{rendering.text});
            for (std::size_t level = 0; level < entry.path.size(); ++level) wrapped += " }";
            emit(*object, std::move(wrapped));
            moved_objects.insert(object);
            moved = true;
            return true;
        };
        auto named = std::vector<const named_object*>{};
        for (const auto& name : names_in(rendered)) {
            for (auto [entry, last] = entries.objects.equal_range(name); entry != last; ++entry) {
                if (after_user(*entry->second.object)) named.push_back(&entry->second);
            }
        }
        std::sort(named.begin(), named.end(), [&](const auto* left, const auto* right) {
            return position(left->object->span.begin) < position(right->object->span.begin);
        });
        for (const auto* entry : named) static_cast<void>(try_move(try_move, *entry));
        return moved;
    };
    auto render_type_definitions = [&](const auto& self, const ir_declaration& declaration)
        -> line_text {
        if (is_defined_in_place(declaration)) {
            return render_declaration(declaration, expression_options);
        }
        if (declaration.kind == declaration_kind::function) {
            return is_hoisted_function(declaration) ? std::string{} : function_declaration(declaration);
        }
        if (is_function_using(declaration) || is_concept(declaration) ||
            declaration.is_defined_last) {
            return {};
        }
        if (is_deferred_object(declaration)) {
            const auto name = render_name(declaration.name);
            return "extern " + (declaration.is_lazy
                ? render_lazy_declarator(declaration.type, name)
                : render_cpp1_type_declaration(declaration.type, name)) + ";";
        }
        if (is_namespace(declaration)) {
            auto result = line_text{"namespace " + render_name(declaration.name) + " {"};
            auto empty = true;
            // Each member begins on its source line, unless that is not after the
            // last line named so far (a generated member shares its source's line).
            auto last_line = declaration.span.begin.line;
            for (std::size_t index = 0; index < declaration.members.size(); ++index) {
                const auto& member = declaration.members[index];
                if (moved_objects.contains(&member)) continue;
                auto rendered = self(self, member);
                if (rendered.empty()) continue;
                const auto moved = move_named_objects(declaration.members, index, rendered, false,
                    [&](const ir_declaration& object) { return self(self, object); },
                    [&](const ir_declaration& object, line_text text) {
                        result += "\n";
                        result.begin_line(object.span.begin.line, object.span.end.line);
                        result.append(std::move(text));
                    });
                result += "\n";
                if (moved) result.begin_line(member.span.begin.line, member.span.end.line);
                else if (member.span.begin.line > last_line) result.begin_line(member.span.begin.line);
                const auto first = result.pieces().size() - 1;
                result.append(std::move(rendered));
                for (auto piece = first; piece < result.pieces().size(); ++piece) {
                    last_line = std::max(last_line, result.pieces()[piece].line);
                }
                empty = false;
            }
            if (empty) return {};
            result += "\n}";
            return result;
        }
        return render_declaration(declaration, expression_options, member_mode::declaration);
    };
    auto has_out_of_line_members = [&](const auto& self, const ir_declaration& declaration)
        -> bool {
        return is_type(declaration) &&
            std::any_of(declaration.members.begin(), declaration.members.end(),
                [&](const auto& member) {
                    return is_defined_out_of_line(member, expression_options) || member.is_lazy ||
                        is_alias_defined_out_of_line(declaration, member) || self(self, member);
                });
    };
    auto is_definition = [&](const ir_declaration& declaration) {
        return is_phase2(declaration) ||
            has_out_of_line_members(has_out_of_line_members, declaration);
    };
    auto render_function_definitions = [&](const auto& self, const ir_declaration& declaration,
                                           std::vector<line_chunk>& result) -> void {
        if (is_type(declaration)) {
            render_out_of_line_members(declaration, expression_options, result);
        }
        else if (is_phase2(declaration)) {
            result.push_back({declaration.span.begin.line,
                              render_declaration(declaration, expression_options),
                              declaration.span.end.line});
        }
        else if (is_namespace(declaration) && contains(contains, declaration, is_definition)) {
            result.push_back({declaration.span.begin.line,
                              "namespace " + render_name(declaration.name) + " {"});
            for (const auto& member : declaration.members) self(self, member, result);
            result.push_back({declaration.span.end.line, "}"});
        }
    };
    // What `select` renders for the declarations in `declaration`, its
    // namespaces and its types, in the namespaces that enclose them.
    auto render_selected = [&](const auto& self, const ir_declaration& declaration,
                               const auto& select, std::vector<line_chunk>& result) -> void {
        if (is_namespace(declaration)) {
            const auto size = result.size();
            result.push_back({declaration.span.begin.line,
                              "namespace " + render_name(declaration.name) + " {"});
            for (const auto& member : declaration.members) self(self, member, select, result);
            if (result.size() == size + 1) result.pop_back();
            else result.push_back({declaration.span.end.line, "}"});
            return;
        }
        if (auto text = select(declaration); !text.empty()) {
            result.push_back({declaration.span.begin.line, std::move(text),
                              declaration.span.end.line});
        }
        if (is_type(declaration)) {
            for (const auto& member : declaration.members) self(self, member, select, result);
        }
    };

    auto emit_line = [&](std::uint32_t line_number, line_text text, std::uint32_t end_line = 0) {
        output.add_line(line_number, std::move(text), end_line);
    };

    auto line_number_at = [](std::size_t index) -> std::uint32_t {
        return static_cast<std::uint32_t>(index + 1);
    };
    auto emit_preamble = [&]() {
        const auto begin = code_begin_index(source);
        for (std::size_t index = 0; index < begin; ++index) {
            const auto& line = source.lines()[index];
            if (line.category != source_category::preprocessor &&
                line.category != source_category::import) {
                continue;
            }
            if (continues_line(source, index)) {
                output.add_verbatim_line(line_number_at(index), line.text);
                continue;
            }
            const auto header = include_header(line.text);
            if (!header.empty() && !output.try_mark_include(header)) continue;
            emit_line(line_number_at(index), lowering_detail::cpp1_include_line(line.text));
        }
    };
    emit_preamble();
    if (prologue) {
        for (auto index = prologue->fragment_begin; index < prologue->end; ++index) {
            const auto& line = source.lines()[index];
            if (line.category == source_category::empty ||
                line.category == source_category::comment) {
                continue;
            }
            if (continues_line(source, index)) {
                output.add_verbatim_line(line_number_at(index), line.text);
                continue;
            }
            const auto header = include_header(line.text);
            if (!header.empty() && !output.try_mark_include(header)) continue;
            emit_line(line_number_at(index), line.text);
        }
    }
    // The written function or lambda most nearly enclosing each lambda; a
    // generated member spans its whole type.
    const auto key = [](source_position position) { return std::pair{position.line, position.column}; };
    const auto encloses = [&](const ir_declaration& outer, const ir_declaration& inner) {
        return &outer != &inner && key(outer.span.begin) <= key(inner.span.begin) &&
            key(inner.span.end) <= key(outer.span.end);
    };
    auto functions = std::vector<const ir_declaration*>{lambda_declarations};
    const auto collect_functions = [&](const auto& self, const ir_declaration& declaration) -> void {
        if (declaration.kind == declaration_kind::function && !declaration.is_generated) {
            functions.push_back(&declaration);
        }
        for (const auto& member : declaration.members) self(self, member);
    };
    for (const auto& declaration : program.declarations) collect_functions(collect_functions, declaration);
    const auto enclosing_function = [&](const ir_declaration& lambda) {
        const ir_declaration* innermost = nullptr;
        for (const auto* candidate : functions) {
            if (encloses(*candidate, lambda) &&
                (innermost == nullptr || key(innermost->span.begin) < key(candidate->span.begin))) {
                innermost = candidate;
            }
        }
        return innermost;
    };
    const auto in_default_argument = [&](const ir_declaration& lambda) {
        const auto* innermost = enclosing_function(lambda);
        return innermost != nullptr && std::ranges::any_of(
            innermost->parameters, [&](const parameter& value) {
                return std::ranges::any_of(value.default_value.tree().tokens,
                    [&](const auto& token) { return token.text == lambda.name; });
            });
    };
    const auto in_member_with_this = [&](const ir_declaration& lambda) {
        const auto* innermost = enclosing_function(lambda);
        // A default argument cannot capture `this`.
        return innermost != nullptr && !in_default_argument(lambda) &&
            innermost->scope == declaration_scope::type_scope && receiver(*innermost) != nullptr;
    };
    const auto unsafe_blocks = options.experiments.enabled("unsafe")
        ? ir_detail::unsafe_spans(program.declarations) : std::vector<source_span>{};
    const auto render_lambda = [&](const std::string& name) -> std::optional<line_text> {
        if (!is_function_expression_name(name)) return std::nullopt;
        const auto declaration = std::find_if(lambda_declarations.begin(),
            lambda_declarations.end(), [&](const auto* value) { return value->name == name; });
        const auto expression = std::find_if(input.function_expressions.begin(),
            input.function_expressions.end(),
            [&](const auto& candidate) { return candidate.name == name; });
        if (declaration == lambda_declarations.end() ||
            expression == input.function_expressions.end()) {
            return std::nullopt;
        }
        // Everything lexically inside an `unsafe` block is unchecked.
        auto lambda_options = expression_options;
        // Captures of `that` move at its last uses in the enclosing function.
        if (const auto* enclosing = enclosing_function(**declaration);
            enclosing != nullptr && lambda_options.plan != nullptr) {
            lambda_options.that_last_uses = lambda_options.plan->that_last_uses_in(enclosing->symbol);
        }
        if (ir_detail::inside_any(unsafe_blocks, (*declaration)->span)) {
            disable_runtime_checks(lambda_options);
        }
        // Captures outside any function body, or in a default argument, are nonlocal.
        lambda_options.nonlocal = enclosing_function(**declaration) == nullptr ||
            in_default_argument(**declaration);
        return render_function_expression(**declaration, *expression, lambda_options,
                                          in_member_with_this(**declaration));
    };
    // The concepts in source order, each after the later ones it names, in its
    // namespaces.
    auto concepts = std::vector<std::pair<const ir_declaration*,
                                          std::vector<const ir_declaration*>>>{};
    {
        auto path = std::vector<const ir_declaration*>{};
        const auto collect = [&](const auto& self, const ir_declaration& declaration) -> void {
            if (is_concept(declaration)) concepts.emplace_back(&declaration, path);
            if (!is_namespace(declaration)) return;
            path.push_back(&declaration);
            for (const auto& member : declaration.members) self(self, member);
            path.pop_back();
        };
        for (const auto& declaration : program.declarations) collect(collect, declaration);
    }
    auto concepts_named = std::multimap<std::string, std::size_t>{};
    for (std::size_t index = 0; index < concepts.size(); ++index) {
        concepts_named.emplace(concepts[index].first->name, index);
    }
    auto emitted_concepts = std::set<const ir_declaration*>{};
    const auto emit_concept = [&](const auto& self, const ir_declaration& concept_declaration,
                                  const std::vector<const ir_declaration*>& path) -> void {
        if (!emitted_concepts.insert(&concept_declaration).second) return;
        auto text = render_declaration(concept_declaration, expression_options);
        for (const auto& name : names_in(text)) {
            for (auto [entry, last] = concepts_named.equal_range(name); entry != last; ++entry) {
                const auto& [other, other_path] = concepts[entry->second];
                self(self, *other, other_path);
            }
        }
        auto result = line_text{};
        const auto line = concept_declaration.span.begin.line;
        result.begin_line(line, line);
        for (const auto* space : path) result += "namespace " + render_name(space->name) + " { ";
        result.append(std::move(text));
        for (std::size_t level = 0; level < path.size(); ++level) result += " }";
        result += "\n";
        output.add_raw(std::move(result));
    };
    for (const auto& declaration : program.declarations) {
        output.add_raw(render_type_declarations(render_type_declarations, declaration, false));
    }
    for (const auto& [concept_declaration, path] : concepts) {
        emit_concept(emit_concept, *concept_declaration, path);
    }
    for (const auto& declaration : program.declarations) {
        output.add_raw(render_type_declarations(render_type_declarations, declaration, true));
    }

    // Interleave declarations with the Cpp1 lines around them: each declaration is
    // emitted where it begins, and the lines it spans are not copied.
    auto next_declaration = program.declarations.begin();
    auto covered_through = std::uint32_t{0};
    for (auto index = prologue ? prologue->end : code_begin_index(source);
         index < source.lines().size(); ++index) {
        const auto& line = source.lines()[index];
        const auto line_number = line_number_at(index);
        for (; next_declaration != program.declarations.end() &&
               next_declaration->span.begin.line <= line_number;
             ++next_declaration) {
            auto rendered = moved_objects.contains(&*next_declaration) ? line_text{}
                : render_type_definitions(render_type_definitions, *next_declaration);
            if (!rendered.empty()) {
                static_cast<void>(move_named_objects(program.declarations,
                    static_cast<std::size_t>(next_declaration - program.declarations.begin()),
                    rendered, true,
                    [&](const ir_declaration& object) {
                        return render_type_definitions(render_type_definitions, object);
                    },
                    [&](const ir_declaration& object, line_text text) {
                        emit_line(object.span.begin.line, std::move(text), object.span.end.line);
                    }));
                emit_line(next_declaration->span.begin.line, std::move(rendered),
                          next_declaration->span.end.line);
            }
            covered_through = std::max(covered_through, next_declaration->span.end.line);
        }
        if (line_number <= covered_through || line.category == source_category::cpp2) continue;
        if (line.category == source_category::raw_string) {
            output.add_verbatim_line(line_number, line.text);
            continue;
        }
        if (continues_line(source, index)) {
            output.add_verbatim_line(line_number, line.text);
            continue;
        }
        if (line.category == source_category::preprocessor ||
            line.category == source_category::import) {
            const auto header = include_header(line.text);
            if (!header.empty() && !output.try_mark_include(header)) continue;
        }
        output.add_raw("\n");
        emit_line(line_number, line.category == source_category::preprocessor
            ? lowering_detail::cpp1_include_line(line.text) : line.text);
    }

    const auto emit_chunks = [&](std::vector<line_chunk> chunks) {
        for (auto& chunk : chunks) {
            if (chunk.text.empty()) continue;
            output.add_raw("\n");
            emit_line(chunk.line, std::move(chunk.text), chunk.end_line);
        }
    };
    const auto emit_selected = [&](const auto& select) {
        for (const auto& declaration : program.declarations) {
            auto chunks = std::vector<line_chunk>{};
            render_selected(render_selected, declaration, select, chunks);
            emit_chunks(std::move(chunks));
        }
    };
    emit_selected([&](const ir_declaration& declaration) {
        return declaration.is_defined_last
            ? render_declaration(declaration, expression_options, member_mode::declaration)
            : line_text{};
    });
    for (const auto& declaration : program.declarations) {
        auto definitions = std::vector<line_chunk>{};
        render_function_definitions(render_function_definitions, declaration, definitions);
        emit_chunks(std::move(definitions));
    }
    emit_selected([](const ir_declaration& declaration) {
        return line_text{declaration.trailing_cpp1};
    });

    if (!options.header_guard.empty() && (source.has_cpp2() || all_std)) {
        output.add_raw("\n#endif\n");
    }
    auto text = substitute_function_expressions(output.take(), render_lambda);
    if (options.experiments.enabled("checked-arithmetic") &&
        std::any_of(text.pieces().begin(), text.pieces().end(), [](const auto& piece) {
            return piece.text.find("cpp2::checked_") != std::string::npos ||
                piece.text.find("cpp2::wrapping_") != std::string::npos;
        })) {
        add_runtime_include(text.pieces().front().text, "cpp2checked.h");
    }
    if (options.experiments.enabled("comprehension") &&
        std::any_of(text.pieces().begin(), text.pieces().end(), [](const auto& piece) {
            return piece.text.find("cpp2::comprehend") != std::string::npos;
        })) {
        add_runtime_include(text.pieces().front().text, "cpp2comprehension.h");
    }
    if (try_hoists.count > 0) add_runtime_include(text.pieces().front().text, "cpp2try.h");
    auto emitted = output.finish(text, options.line_directives);
    return {std::move(emitted.text), std::move(emitted.source_map)};
}

}
