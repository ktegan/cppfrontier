#include "known_types.hpp"

#include <algorithm>
#include <array>
#include <string_view>
#include <variant>

#include "ir_walk.hpp"
#include "mentions.hpp"

namespace cppfrontier {
namespace {

using entry_kind = scope_model::entry_kind;

constexpr auto max_depth = 16;

// The standard containers whose first template argument is their element type.
constexpr auto containers = std::array<std::string_view, 11>{
    "vector", "array", "deque", "list", "forward_list", "span", "set", "multiset",
    "unordered_set", "unordered_multiset", "initializer_list"};

// The template argument `index` of `type` when it names `std::name<...>` for
// one of `names`.
auto standard_argument(const known_type& type, auto names, std::size_t index)
    -> std::optional<known_type> {
    const auto& components = type.type.core().components;
    if (type.type.core().kind != type_kind::name || pointer_depth(type.type) > 0 ||
        components.size() != 2 ||
        components.front().identifier != "std" ||
        std::find(names.begin(), names.end(), components.back().identifier) == names.end() ||
        components.back().arguments.size() <= index) {
        return std::nullopt;
    }
    return known_type{components.back().arguments[index], type.point};
}

auto is_object(const scope_model::entry& value) -> bool {
    return value.kind == entry_kind::object || value.kind == entry_kind::parameter ||
        value.kind == entry_kind::statement_parameter;
}

}

auto call_candidates::parameter_for(const ir_declaration& function, std::size_t argument) const
    -> const parameter* {
    const auto offset = receiver || cppfrontier::receiver(function) != nullptr ? 0 : 1;
    if (argument < static_cast<std::size_t>(offset)) return nullptr;
    const auto& parameters = function.parameters;
    const auto index = argument - static_cast<std::size_t>(offset);
    if (index < parameters.size()) return &parameters[index];
    return !parameters.empty() && parameters.back().is_variadic ? &parameters.back() : nullptr;
}

known_types::known_types(const ir_program& program, const scope_model& scopes) : scopes_{scopes} {
    ir_detail::visit_declarations(program.declarations, [&](const ir_declaration& value) {
        if (value.kind != declaration_kind::function || value.symbol == invalid_symbol) return;
        const auto scope = scopes.outside(value.symbol).scope;
        if (scope != scope_point::none) functions_[{scope, value.name}].push_back(&value);
    }, [&](const statement& value) {
        if (const auto* block = std::get_if<statement::block_form>(&value.form)) {
            for (const auto& parameter : block->parameters) {
                initializers_[&parameter.value] = {&parameter.initializer, scopes.at(parameter)};
            }
        }
        const auto* loop = std::get_if<statement::loop_form>(&value.form);
        if (loop != nullptr && loop->range_parameter &&
            value.kind == statement_kind::for_statement) {
            ranges_[&*loop->range_parameter] = {&loop->condition, scopes.at(value)};
        }
    });
}

auto known_types::type_of(const scope_model::entry& value) const -> std::optional<known_type> {
    return type_of(value, 0);
}

auto known_types::type_of(const expression_text& text, const expression& node,
                          scope_point point) const -> std::optional<known_type> {
    return type_of(text, node, point, 0);
}

auto known_types::type_of(const scope_model::entry& value, int depth) const
    -> std::optional<known_type> {
    if (depth > max_depth) return std::nullopt;
    if (!value.type.is_wildcard()) {
        return known_type{value.type, {value.scope, value.ordinal}};
    }
    const auto deduced = [&](const initializer& from) -> std::optional<known_type> {
        const auto* whole = from.text != nullptr ? whole_expression(from.text->tree()) : nullptr;
        return whole != nullptr ? type_of(from.text->tree(), *whole, from.point, depth + 1)
                                : std::nullopt;
    };
    if (value.declared_parameter != nullptr) {
        if (const auto found = initializers_.find(value.declared_parameter);
            found != initializers_.end()) {
            return deduced(found->second);
        }
        const auto range = ranges_.find(value.declared_parameter);
        if (range == ranges_.end()) return std::nullopt;
        const auto type = deduced(range->second);
        return type ? standard_argument(*type, containers, 0) : std::nullopt;
    }
    const auto* declaring = value.declaring_statement;
    if (declaring == nullptr) return std::nullopt;
    if (const auto* form = std::get_if<statement::declaration_form>(&declaring->form)) {
        return form->declaration ? deduced({&form->declaration->initializer, scopes_.at(*declaring)})
                                 : std::nullopt;
    }
    const auto* form = std::get_if<statement::destructuring_form>(&declaring->form);
    if (form == nullptr) return std::nullopt;
    const auto binding = std::find_if(form->bindings.begin(), form->bindings.end(),
        [&](const auto& item) { return item.name == value.name; });
    const auto index = static_cast<std::size_t>(binding - form->bindings.begin());
    const auto& tree = form->initializer.tree();
    const auto* whole = whole_expression(tree);
    if (whole == nullptr || index == form->bindings.size()) return std::nullopt;
    const auto type = type_of(tree, *whole, scopes_.at(*declaring), depth + 1);
    constexpr auto tuples = std::array<std::string_view, 2>{"pair", "tuple"};
    return type ? standard_argument(*type, tuples, index) : std::nullopt;
}

auto known_types::type_of(const expression_text& text, const expression& node, scope_point point,
                          int depth) const -> std::optional<known_type> {
    if (depth > max_depth || !point.valid()) return std::nullopt;
    const auto& tokens = text.tokens;
    switch (node.kind) {
    case expression_kind::group:
    case expression_kind::argument:
        return node.children.size() == 1
            ? type_of(text, node.children.front(), point, depth + 1) : std::nullopt;
    case expression_kind::literal: {
        auto scalar = known_type{};
        scalar.type.kind = type_kind::fundamental;
        return scalar;
    }
    case expression_kind::typed_value:
        return known_type{parse_type(tokens, node.children[0].first, node.children[0].last), point};
    case expression_kind::is_as:
        if (tokens[node.op].text != "as" || node.children.size() != 2) return std::nullopt;
        return known_type{parse_type(tokens, node.children[1].first, node.children[1].last), point};
    case expression_kind::name: {
        if (!is_single_name(node)) return std::nullopt;
        if (tokens[node.first].text == "this") {
            const auto* type = scopes_.enclosing_type(point);
            if (type == nullptr) return std::nullopt;
            return known_type{parse_type(type->name), {type->scope, type->ordinal}};
        }
        const auto* found = scopes_.lookup(tokens[node.first].text, point);
        if (found != nullptr) found = scopes_.resolve(*found);
        return found != nullptr && is_object(*found) ? type_of(*found, depth + 1) : std::nullopt;
    }
    case expression_kind::member: {
        if (tokens[node.op].text != "." || node.children.size() != 2 ||
            !is_single_name(node.children[1])) {
            return std::nullopt;
        }
        const auto object = type_of(text, node.children[0], point, depth + 1);
        const auto* type = object ? declared_type(*object) : nullptr;
        const auto* member = type != nullptr
            ? scopes_.member(*type, tokens[node.children[1].first].text) : nullptr;
        return member != nullptr && is_object(*member) ? type_of(*member, depth + 1)
                                                        : std::nullopt;
    }
    case expression_kind::call: {
        if (tokens[node.op].text != "(") return std::nullopt;
        const auto& callee = node.children.front();
        if (callee.kind == expression_kind::name) {
            const auto* found = scopes_.lookup(name_spelling(text, callee), point);
            if (found != nullptr) found = scopes_.resolve(*found);
            if (found != nullptr && found->kind == entry_kind::type) {
                return known_type{parse_type(tokens, callee.first, callee.last), point};
            }
        }
        const auto functions = callee_functions(text, callee, point, depth + 1).first;
        if (functions.empty() || functions.front()->type.is_wildcard()) return std::nullopt;
        return known_type{functions.front()->type, scopes_.outside(functions.front()->symbol)};
    }
    default:
        return std::nullopt;
    }
}

auto known_types::declared_type(const known_type& type) const -> const scope_model::entry* {
    const auto& core = type.type.core();
    if (core.kind != type_kind::name || !type.point.valid()) return nullptr;
    auto name = std::string{};
    for (const auto& component : core.components) {
        name += (name.empty() ? "" : "::") + component.identifier;
    }
    const auto* found = scopes_.lookup(name, type.point);
    if (found != nullptr) found = scopes_.resolve(*found);
    return found != nullptr && found->kind == entry_kind::type ? found : nullptr;
}

auto known_types::is_scalar(const known_type& type) -> bool {
    const auto kind = type.type.core().kind;
    return kind == type_kind::fundamental || kind == type_kind::reference ||
        pointer_depth(type.type) > 0;
}

auto known_types::overloads(const scope_model::entry* found) const
    -> std::vector<const ir_declaration*> {
    if (found != nullptr) found = scopes_.resolve(*found);
    if (found == nullptr || found->kind != entry_kind::function) return {};
    const auto functions = functions_.find({found->scope, found->name});
    return functions != functions_.end() ? functions->second : std::vector<const ir_declaration*>{};
}

auto known_types::callee_functions(const expression_text& text, const expression& callee,
                                   scope_point point, int depth) const
    -> std::pair<std::vector<const ir_declaration*>, bool> {
    const auto& tokens = text.tokens;
    if (callee.kind == expression_kind::name) {
        return {overloads(scopes_.lookup(name_spelling(text, callee), point)), false};
    }
    if (callee.kind != expression_kind::member || tokens[callee.op].text != "." ||
        callee.children.size() != 2 || !is_single_name(callee.children[1])) {
        return {};
    }
    const auto& name = tokens[callee.children[1].first].text;
    const auto object = type_of(text, callee.children[0], point, depth);
    const auto* type = object ? declared_type(*object) : nullptr;
    auto members = overloads(type != nullptr ? scopes_.member(*type, name) : nullptr);
    if (!members.empty()) return {std::move(members), true};
    return {overloads(scopes_.lookup(name, point)), true};
}

auto known_types::callees(const expression_text& text, const expression& call,
                          scope_point point) const -> call_candidates {
    auto result = call_candidates{};
    if (call.kind != expression_kind::call || call.children.empty() ||
        text.tokens[call.op].text != "(") {
        return result;
    }
    auto [functions, receiver] = callee_functions(text, call.children.front(), point, 0);
    result.receiver = receiver;
    const auto arguments = call.children.size() - 1;
    for (const auto* function : functions) {
        const auto& parameters = function->parameters;
        const auto count = arguments +
            (receiver || cppfrontier::receiver(*function) != nullptr ? 1 : 0);
        const auto variadic = std::any_of(parameters.begin(), parameters.end(),
            [](const parameter& value) { return value.is_variadic; });
        const auto required = static_cast<std::size_t>(std::count_if(
            parameters.begin(), parameters.end(), [](const parameter& value) {
                return value.default_value.empty() && !value.is_variadic;
            }));
        if (count >= required && (variadic || count <= parameters.size())) {
            result.functions.push_back(function);
        }
    }
    return result;
}

}
