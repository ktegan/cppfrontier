#include "cpp1_type_renderer.hpp"
#include "cpp1_expression_renderer.hpp"

#include <cppfrontier/token.hpp>

#include <algorithm>
#include <cctype>
#include <ranges>
#include <vector>

namespace cppfrontier {
namespace {

// Marks where a declared name goes in a declarator.
constexpr auto placeholder = '@';

struct spelled_type final {
    std::string base;
    std::string declarator;
};

auto join(std::string base, const std::string& declarator) -> std::string {
    const auto word_character = [](char value) {
        return std::isalnum(static_cast<unsigned char>(value)) || value == '_' || value == '>' ||
            value == ')';
    };
    if (!base.empty() && !declarator.empty() &&
        word_character(base.back()) && word_character(declarator.front())) {
        base += ' ';
    }
    return base + declarator;
}

auto spelling(const spelled_type& type) -> std::string {
    auto declarator = type.declarator;
    std::erase(declarator, placeholder);
    return join(type.base, declarator);
}

auto named_spelling(const spelled_type& type, std::string_view name) -> std::string {
    auto declarator = type.declarator;
    const auto at = declarator.find(placeholder);
    if (at == std::string::npos) return type.base + " " + std::string{name};
    const auto pointer_declarator = at != 0 && declarator[at - 1] == '*' &&
        declarator.find('(') == std::string::npos;
    declarator.replace(at, 1, (pointer_declarator ? " " : "") + std::string{name});
    return join(type.base, declarator);
}

// `aliases` spells a fundamental alias `cpp2::u8`, as in an expression,
// instead of its Cpp1 type `unsigned char`, as in a declaration.
auto spell(const type_id& type, bool aliases) -> spelled_type;

auto render(const type_id& type, bool aliases) -> std::string;

auto alias_spelling(std::string_view name, bool aliases) -> std::string {
    if (aliases && is_fundamental_type_keyword(name)) return "cpp2::" + std::string{name};
    return std::string{cpp1_fundamental_spelling(name)};
}

// Text kept as written: names are spelled for Cpp1 and fundamental type
// keywords are mapped.
auto render_words(std::string_view text, bool aliases) -> std::string {
    const auto parsed = parse_expression_text(text);
    const auto& tokens = parsed.tokens;
    auto result = std::string{};
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        const auto& token = tokens[index];
        const auto previous = index > 0 ? std::string_view{tokens[index - 1].text} : "";
        const auto next = index + 1 < tokens.size() ? std::string_view{tokens[index + 1].text} : "";
        const auto blank = std::all_of(token.leading.begin(), token.leading.end(),
            [](char c) { return std::isspace(static_cast<unsigned char>(c)); });
        if (index > 0 && !(blank && (token.text == "::" || previous == "::"))) {
            result += token.leading;
        }
        if (token.kind != expression_token_kind::identifier) {
            result += token.text;
            continue;
        }
        const auto member = previous == "." || previous == "->";
        if (const auto fundamental = alias_spelling(token.text, aliases);
            !fundamental.empty() && previous != "::") {
            result += fundamental;
        }
        else if (token.text == "type_of" && previous != "::") {
            result += "CPP2_TYPEOF";
        }
        else if (is_cpp2_support_name(token.text) && !member && previous != "::" && next != "::") {
            result += "cpp2::" + token.text;
        }
        else {
            result += cpp1_identifier(token.text);
        }
    }
    return result;
}

// A value template argument, and `decltype(e)`, render as Cpp1 expressions.
auto render_argument(const type_id& argument, bool aliases) -> std::string {
    if (argument.kind == type_kind::expression || argument.kind == type_kind::decltype_of) {
        auto options = cpp1_expression_options{};
        options.nonlocal = true;
        options.member_calls = argument.kind == type_kind::decltype_of;
        return render_cpp1_expression(argument.text, options);
    }
    return render(argument, aliases);
}

// `typename` for a name written `type T::x`, or a dependent qualified name.
auto needs_typename(const type_id& type) -> bool {
    return type.has_type_marker ||
        ((type.kind == type_kind::name || type.kind == type_kind::cpp1) && type.is_dependent);
}

auto render_name(const type_id& type, bool aliases) -> std::string {
    auto result = std::string{type.is_global ? "::" : ""};
    for (std::size_t index = 0; index < type.components.size(); ++index) {
        const auto& component = type.components[index];
        if (index != 0) {
            result += "::";
            if (component.has_template_keyword) {
                result += "template ";
            }
        }
        const auto head = index == 0 && !type.is_global;
        if (head && (type.components.size() == 1 || component.has_arguments) &&
            is_cpp2_support_name(component.identifier)) {
            result += "cpp2::" + component.identifier;
        }
        else if (const auto fundamental = alias_spelling(component.identifier, aliases);
                 head && !fundamental.empty()) {
            result += fundamental;
        }
        else {
            result += cpp1_identifier(component.identifier);
        }
        if (!component.has_arguments) continue;
        result += '<';
        for (std::size_t argument = 0; argument < component.arguments.size(); ++argument) {
            if (argument != 0) result += ", ";
            result += render_argument(component.arguments[argument], aliases);
        }
        result += '>';
    }
    return result;
}

// `std::variant<A, B>` for `A | B`, without alternatives spelled alike in
// Cpp1; one remaining alternative is that type.
auto render_union(const type_id& type, bool aliases) -> std::string {
    auto alternatives = std::vector<std::string>{};
    for (const auto& child : type.children) {
        auto spelled = render(child, aliases);
        if (std::find(alternatives.begin(), alternatives.end(), spelled) == alternatives.end()) {
            alternatives.push_back(std::move(spelled));
        }
    }
    if (alternatives.size() == 1) return alternatives.front();
    auto result = std::string{"std::variant<"};
    for (const auto& alternative : alternatives) {
        if (&alternative != &alternatives.front()) result += ", ";
        result += alternative;
    }
    return result + ">";
}

auto render_base(const type_id& type, bool aliases) -> std::string {
    switch (type.kind) {
    case type_kind::none: return {};
    case type_kind::union_type: return render_union(type, aliases);
    // `C auto` for `_ is C`; Cpp1 has no `A & B auto`, so an operator
    // constraint is checked where the type is declared.
    case type_kind::deduced: {
        const auto* constraint = type.constraint();
        if (constraint == nullptr || constraint->is_operator()) return "auto";
        return render(*constraint, aliases) + " auto";
    }
    case type_kind::name: return render_name(type, aliases);
    case type_kind::fundamental: {
        const auto spelling = alias_spelling(type.text, aliases);
        return spelling.empty() ? type.text : spelling;
    }
    case type_kind::reference:
        return render(type.children.front(), aliases) + (type.is_rvalue ? "&&" : "&");
    case type_kind::pack: return render(type.children.front(), aliases) + "...";
    case type_kind::expression:
    case type_kind::decltype_of: return render_argument(type, aliases);
    default: return render_words(type.text, aliases);
    }
}

// `in`, `copy`, ... parameters of a function type, as in a declaration.
auto render_function_parameter(const type_parameter& parameter, const type_id& type,
                               std::size_t ordinal, bool aliases) -> std::string {
    const auto unnamed = parameter.name.empty() || parameter.name == "_";
    const auto name = unnamed ? "unnamed_param_" + std::to_string(ordinal) : parameter.name;
    const auto parsed = spell(type, aliases);
    const auto spelled = spelling(parsed);
    auto result = std::string{unnamed ? "[[maybe_unused]] " : ""};
    // A `forward` parameter is diagnosed.
    switch (parameter.passing) {
    case passing_style::in: return result + "cpp2::impl::in<" + spelled + "> " + name;
    case passing_style::in_ref: return result + named_spelling(parsed, "const& " + name);
    case passing_style::copy: return result + named_spelling(parsed, name);
    case passing_style::inout: return result + named_spelling(parsed, "& " + name);
    case passing_style::out: return result + "cpp2::impl::out<" + spelled + "> " + name;
    case passing_style::move:
    case passing_style::forward: return result + named_spelling(parsed, "&& " + name);
    }
    return result;
}

// `R(parameters)`, or `R(*)(parameters)` below the pointer declarator `pointers`.
auto spell_function(const type_id& type, const std::string& pointers, bool aliases)
    -> spelled_type {
    auto parameters = std::string{};
    for (std::size_t index = 0; index + 1 < type.children.size(); ++index) {
        if (!parameters.empty()) parameters += ", ";
        parameters += render_function_parameter(type.parameters[index], type.children[index],
                                                index + 1, aliases);
    }
    // The function's declarator goes where the return type's declarator names
    // its declaration, so a returned function pointer nests around it. A
    // `forward T` return is a reference to `T`.
    auto result = spell(type.children.back(), aliases);
    const auto at = result.declarator.find(placeholder);
    result.declarator.replace(at, 1, (type.returns_forward ? "&" : "") +
        (pointers.empty() ? std::string(1, placeholder) + "(" + parameters + ")"
                          : "(" + pointers + placeholder + ")(" + parameters + ")"));
    return result;
}

auto spell(const type_id& type, bool aliases) -> spelled_type {
    const auto& core = type.core();
    // The qualifiers of `const Ts...` apply to each element, as `Ts const...`.
    if (core.kind == type_kind::pack) {
        auto element = type;
        element.core() = core.children.front();
        auto result = spell(element, aliases);
        result.declarator.insert(result.declarator.find(placeholder), "...");
        return result;
    }
    // Cpp2 qualifiers read left to right, so Cpp1 spells them in reverse.
    auto qualifiers = std::vector<type_kind>{};
    for (const auto* current = &type; current != &core; current = &current->children.front()) {
        qualifiers.push_back(current->kind);
    }
    // A function type itself cannot be `const`.
    while (core.is_function() && !qualifiers.empty() &&
           qualifiers.back() == type_kind::constant) {
        qualifiers.pop_back();
    }
    auto declarator = std::string{};
    for (const auto qualifier : std::views::reverse(qualifiers)) {
        if (qualifier == type_kind::constant) {
            if (!declarator.empty()) declarator += ' ';
            declarator += "const";
        }
        else {
            declarator += '*';
        }
    }
    if (!declarator.empty() && declarator.back() != '*') declarator += ' ';
    if (core.is_function()) return spell_function(core, declarator, aliases);
    auto result = spelled_type{render_base(core, aliases), declarator + placeholder};
    if (needs_typename(core)) result.base = "typename " + result.base;
    return result;
}

auto render(const type_id& type, bool aliases) -> std::string {
    return spelling(spell(type, aliases));
}

}

auto render_cpp1_type(const type_id& type) -> std::string {
    return render(type, false);
}

auto render_cpp1_expression_type(const type_id& type) -> std::string {
    return render(type, true);
}

auto render_cpp1_type_declaration(const type_id& type, std::string_view name) -> std::string {
    return named_spelling(spell(type, false), name);
}

auto render_cpp1_type(std::string_view type) -> std::string {
    return render_cpp1_type(parse_type(type));
}

auto render_cpp1_type_declaration(std::string_view type, std::string_view name) -> std::string {
    return render_cpp1_type_declaration(parse_type(type), name);
}

auto render_cpp1_constraint(const type_id& constraint, std::string_view subject) -> std::string {
    if (constraint.is_operator()) {
        const auto joiner = constraint.kind == type_kind::union_type ? " || " : " && ";
        auto result = std::string{};
        for (const auto& child : constraint.children) {
            if (!result.empty()) result += joiner;
            const auto operand = render_cpp1_constraint(child, subject);
            result += child.is_operator() ? "(" + operand + ")" : operand;
        }
        return result;
    }
    auto result = render_cpp1_type(constraint);
    const auto open = result.find('<');
    if (open == std::string::npos) return result + "<" + std::string{subject} + ">";
    const auto empty = constraint.kind == type_kind::name &&
        constraint.components.back().arguments.empty();
    result.insert(open + 1, std::string{subject} + (empty ? "" : ", "));
    return result;
}

auto is_simple_type_specifier(const type_id& type, bool aliases) -> bool {
    if (needs_typename(type)) return false;
    if (type.kind == type_kind::name) return true;
    return type.kind == type_kind::fundamental &&
        render_base(type, aliases).find(' ') == std::string::npos;
}

}
