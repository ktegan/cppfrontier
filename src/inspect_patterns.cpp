#include <cppfrontier/pipeline.hpp>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "brackets.hpp"
#include "ir_walk.hpp"
#include "parser_context.hpp"
#include "scope_model.hpp"

namespace cppfrontier {

namespace {

// A type with `sum_cases`, with the name and symbol of each case.
struct sum_type final {
    std::string name;
    std::vector<std::pair<std::string, symbol_id>> cases;
    // The cases are members, named `Type::case` in messages.
    bool nested = true;
};

auto sum_types_of(const ir_program& program) -> std::unordered_map<symbol_id, sum_type> {
    auto result = std::unordered_map<symbol_id, sum_type>{};
    ir_detail::visit_declarations(program.declarations, [&](const ir_declaration& declaration) {
        if (declaration.sum_cases.empty()) return;
        auto& type = result[declaration.symbol];
        type.name = declaration.name;
        for (const auto id : declaration.sum_cases) {
            const auto* found = find_symbol(program, id);
            if (found == nullptr) continue;
            type.cases.emplace_back(found->name, id);
            type.nested = found->parent == declaration.symbol;
        }
    });
    return result;
}

// The tokens of `node` without the whitespace between them.
auto spelling(const expression_text& text, const expression& node) -> std::string {
    auto result = std::string{};
    for (auto index = node.first; index < node.last; ++index) result += text.tokens[index].text;
    return result;
}

class checker final {
public:
    checker(const scope_model& scopes, const std::unordered_map<symbol_id, sum_type>& sum_types,
            bool sealed)
        : scopes_{scopes}, sum_types_{sum_types}, sealed_{sealed} {}

    struct coverage final {
        // Null when the subject's type is not a sum type.
        const sum_type* type = nullptr;
        // The cases not covered.
        std::string missing;
        // An alternative names a case as a type-id, `is E::a`, which tests a type.
        std::string as_type;
    };

    // The cases of its subject's sum type that `inspect` at `point` does not cover.
    auto missing(const expression_text& text, const expression& inspect, scope_point point) const
        -> coverage {
        const auto* type = subject_type(text, inspect.children[0], point);
        if (type == nullptr) return {};
        auto covered = std::vector<symbol_id>{};
        auto as_type = std::string{};
        for (auto index = std::size_t{2}; index < inspect.children.size(); ++index) {
            const auto parts = inspect_alternative_of(text.tokens, inspect.children[index]);
            if (parts.guard != nullptr) continue;
            for (const auto* pattern : parts.patterns) {
                // A type case as a type, or an enumerator as a value `(E::a)`.
                const auto is_type = pattern->kind == expression_kind::sequence;
                const auto* named = is_type ? pattern
                    : pattern->kind == expression_kind::group && pattern->children.size() == 1 &&
                            pattern->children.front().kind == expression_kind::name
                        ? &pattern->children.front() : nullptr;
                if (named != nullptr) covered.push_back(symbol_of(text, *named, point, is_type));
                if (is_type && as_type.empty() && is_case(*type,
                        symbol_of(text, *pattern, point, false))) {
                    as_type = spelling(text, *pattern);
                }
            }
        }
        auto result = std::string{};
        for (const auto& [name, symbol] : type->cases) {
            if (std::find(covered.begin(), covered.end(), symbol) == covered.end()) {
                result += (result.empty() ? "" : ", ") +
                    (type->nested ? type->name + "::" : "") + name;
            }
        }
        return {type, result, as_type};
    }

private:
    const scope_model& scopes_;
    const std::unordered_map<symbol_id, sum_type>& sum_types_;
    // Experiment `sealed`: the subject may also be the dereference of a pointer.
    bool sealed_;

    [[nodiscard]] static auto is_case(const sum_type& type, symbol_id symbol) -> bool {
        return symbol != invalid_symbol && std::any_of(type.cases.begin(), type.cases.end(),
            [&](const auto& value) { return value.second == symbol; });
    }

    // The type (`is_type`) or value a type-id or a name in `node` names at
    // `point`, or `invalid_symbol`.
    [[nodiscard]] auto symbol_of(const expression_text& text, const expression& node,
                                 scope_point point, bool is_type) const -> symbol_id {
        const auto* found = scopes_.lookup(spelling(text, node), point);
        if (found == nullptr) return invalid_symbol;
        const auto* resolved = scopes_.resolve(*found);
        return (resolved->kind == scope_model::entry_kind::type) == is_type ? resolved->symbol
                                                                            : invalid_symbol;
    }

    // The sum type the declared type of the subject `node` names: a name, or
    // with `sealed_` the dereference `p*` of a name declared as a pointer to one.
    // `this` has the type of the enclosing type.
    [[nodiscard]] auto subject_type(const expression_text& text, const expression& node,
                                    scope_point point) const -> const sum_type* {
        const auto dereferenced = sealed_ && node.kind == expression_kind::postfix &&
            node.children.size() == 1 && text.tokens[node.op].text == "*";
        const auto& name = dereferenced ? node.children.front() : node;
        if (name.kind != expression_kind::name || name.last != name.first + 1) return nullptr;
        const auto* subject = scopes_.lookup(spelling(text, name), point);
        if (subject != nullptr && !dereferenced && subject->name == "this" &&
            subject->kind == scope_model::entry_kind::parameter) {
            subject = scopes_.enclosing_type(point);
            const auto found = subject == nullptr ? sum_types_.end()
                                                  : sum_types_.find(subject->symbol);
            return found == sum_types_.end() ? nullptr : &found->second;
        }
        if (subject == nullptr || subject->type.is_wildcard() ||
            (sealed_ && dereferenced != (subject->type.kind == type_kind::pointer))) {
            return nullptr;
        }
        const auto& type_text = sealed_ ? subject->type.core().text : subject->type.text;
        const auto* type = scopes_.lookup(type_text, point);
        if (type == nullptr) return nullptr;
        const auto found = sum_types_.find(scopes_.resolve(*type)->symbol);
        return found == sum_types_.end() ? nullptr : &found->second;
    }
};

// The inspect expression whose `inspect` token is `tokens[start]`, parsed.
auto parse_inspect(const token_list& tokens, std::size_t start, const experiment_set& experiments)
    -> std::optional<expression_text> {
    const auto brace = parser_detail::find_top_level(tokens, start + 1, tokens.size(),
        parser_detail::angle_rule::expression,
        [&](std::size_t at) { return tokens[at].text == "{" || tokens[at].text == ";"; });
    if (brace == tokens.size() || tokens[brace].text != "{") return std::nullopt;
    const auto close = detail::bracket_close(tokens, brace, tokens.size());
    if (!close) return std::nullopt;
    auto source = std::string{};
    for (auto index = start; index <= *close; ++index) {
        source += (source.empty() ? "" : " ") + tokens[index].text;
    }
    auto parsed = parse_expression_text(source, experiments.enabled("try-operator"),
                                        experiments.enabled("comprehension"));
    const auto& root = parsed.root;
    if (root.children.size() != 1 || root.children.front().kind != expression_kind::inspect ||
        root.children.front().last != parsed.tokens.size()) {
        return std::nullopt;
    }
    return parsed;
}

}

auto check_inspect_patterns(const ir_program& program, const scope_model& scopes,
                            const token_list& tokens, const experiment_set& experiments,
                            std::vector<diagnostic>& diagnostics) -> void {
    const auto sum_types = sum_types_of(program);
    const auto check = checker{scopes, sum_types, experiments.enabled("sealed")};
    auto inspects = std::unordered_map<std::uint64_t, std::size_t>{};
    const auto key = [](source_position at) {
        return std::uint64_t{at.line} << 32 | at.column;
    };
    for (auto index = std::size_t{0}; index < tokens.size(); ++index) {
        if (tokens[index].text == "inspect") inspects.emplace(key(tokens[index].span.begin), index);
    }
    std::erase_if(diagnostics, [&](diagnostic& value) {
        if (value.code != "invalid-inspect" || value.message != inspect_wildcard_message) {
            return false;
        }
        const auto found = inspects.find(key(value.span.begin));
        if (found == inspects.end()) return false;
        const auto parsed = parse_inspect(tokens, found->second, experiments);
        if (!parsed) return false;
        const auto [type, missing, as_type] = check.missing(
            *parsed, parsed->root.children.front(), scopes.at(value.span.begin));
        if (type == nullptr) return false;
        if (missing.empty()) return true;
        value.code = "inspect-not-exhaustive";
        value.message = "inspect of '" + type->name + "' does not cover " + missing +
            " - add alternatives or 'is _'" + (as_type.empty() ? "" :
                "; 'is " + as_type + "' tests a type, so write 'is (" + as_type +
                ")' to match the value");
        return false;
    });
}

}
