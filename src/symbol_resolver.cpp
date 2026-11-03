#include <cppfrontier/expression.hpp>
#include <cppfrontier/pipeline.hpp>

#include <set>
#include <utility>

#include "scope_model.hpp"

namespace cppfrontier {

namespace {

// The type a declared type names for member lookup: `T` in `const T` or `T<int>`.
auto named_type(const type_id& declared) -> std::string {
    const auto* type = &declared;
    while (type->kind == type_kind::constant) type = &type->children.front();
    if (type->kind != type_kind::name) return {};
    auto result = std::string{type->is_global ? "::" : ""};
    for (const auto& component : type->components) {
        if (&component != &type->components.front()) result += "::";
        result += component.identifier;
    }
    return result;
}

}

// Looks up each identifier token in the scope model at its source position.
// After `.`, a name is a member of the object's declared Cpp2 type.
auto resolve_symbols(const ir_program& program, const scope_model& scopes,
                     const token_list& tokens) -> std::vector<symbol_reference> {
    auto references = std::vector<symbol_reference>{};
    auto declarations = std::set<std::pair<std::uint32_t, std::uint32_t>>{};
    for (const auto& value : program.symbols) {
        if (value.id != invalid_symbol && !value.name.empty()) {
            declarations.emplace(value.span.begin.line, value.span.begin.column);
        }
    }
    const auto member_of = [&](std::size_t object, std::string_view name,
                               scope_point point) -> const scope_model::entry* {
        if (tokens[object].text == "this") {
            const auto* type = scopes.enclosing_type(point);
            return type == nullptr ? nullptr : scopes.member(*type, name);
        }
        if (tokens[object].kind != token_kind::identifier) return nullptr;
        const auto* found = scopes.lookup(tokens[object].text, point);
        if (found == nullptr) return nullptr;
        const auto type_name = named_type(found->type);
        if (type_name.empty()) return nullptr;
        const auto* type = scopes.lookup(type_name, {found->scope, found->ordinal});
        if (type == nullptr) return nullptr;
        type = scopes.resolve(*type);
        return type->kind == scope_model::entry_kind::type ? scopes.member(*type, name) : nullptr;
    };

    const auto key = [](source_position position) {
        return std::pair{position.line, position.column};
    };
    // A capture `x$` reads as its name `_N` in the innermost function expression
    // containing it, and names what `x` names where that expression appears. A
    // capture of a longer expression, such as `p*$` or `v.size()$`, names nothing.
    const auto captured = [&](std::string_view name,
                              const token& value) -> const scope_model::entry* {
        if (name.size() < 2 || name.front() != '_' ||
            name.find_first_not_of("0123456789", 1) != std::string_view::npos) {
            return nullptr;
        }
        const ir_declaration* innermost = nullptr;
        for (const auto& declaration : program.declarations) {
            if (is_function_expression_name(declaration.name) &&
                key(declaration.span.begin) <= key(value.span.begin) &&
                key(value.span.end) <= key(declaration.span.end) &&
                (innermost == nullptr ||
                 key(innermost->span.begin) < key(declaration.span.begin))) {
                innermost = &declaration;
            }
        }
        if (innermost == nullptr) return nullptr;
        for (const auto& expression : program.function_expressions) {
            if (expression.name != innermost->name) continue;
            for (const auto& capture : expression.captures) {
                if (capture.name != name) continue;
                const auto& parsed = capture.expression.tree();
                if (parsed.tokens.size() != 1 ||
                    parsed.tokens.front().kind != expression_token_kind::identifier) {
                    return nullptr;
                }
                return scopes.lookup(parsed.tokens.front().text, scopes.outside(innermost->symbol));
            }
        }
        return nullptr;
    };

    for (auto position = std::size_t{0}; position < tokens.size(); ++position) {
        const auto& value = tokens[position];
        const auto dereference = value.kind == token_kind::pointer_dereference;
        if ((value.kind != token_kind::identifier && !dereference) ||
            declarations.contains({value.span.begin.line, value.span.begin.column})) {
            continue;
        }
        const auto name = std::string_view{value.text}.substr(0, value.text.size() - dereference);
        if (const auto* found = captured(name, value)) {
            if (const auto* resolved = scopes.resolve(*found); resolved->symbol != invalid_symbol) {
                references.push_back({resolved->symbol, value.span});
            }
            continue;
        }
        const auto point = scopes.at(value.span.begin);
        const auto member = position >= 2 &&
            (tokens[position - 1].text == "." || tokens[position - 1].text == "..");
        const auto* found = member ? member_of(position - 2, name, point)
                                   : scopes.lookup(name, point);
        if (found != nullptr) found = scopes.resolve(*found);
        if (found != nullptr && found->symbol != invalid_symbol) {
            references.push_back({found->symbol, value.span});
        }
    }
    return references;
}

}
