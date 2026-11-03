#include <cppfrontier/pipeline.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "ir_walk.hpp"
#include "known_types.hpp"
#include "mentions.hpp"
#include "scope_model.hpp"

namespace cppfrontier {
namespace {

using scope_entry = scope_model::entry;

// What an argument accesses: a local or parameter (`object`), or the enclosing
// type through `this` (`object` is the type), with `parts` the members named
// by `x.a.b` or `this.a.b`, outermost first, or empty for all of it.
struct access_path final {
    const scope_entry* object = nullptr;
    std::vector<std::string> parts;

    // Whether one access is the other or contains it.
    [[nodiscard]] auto overlaps(const access_path& other) const -> bool {
        const auto common = std::min(parts.size(), other.parts.size());
        return object != nullptr && object == other.object &&
            std::equal(parts.begin(), parts.begin() + static_cast<std::ptrdiff_t>(common),
                       other.parts.begin());
    }
};

// A mention in an argument: the object it accesses, at a token of the
// outermost text.
using argument_mention = std::pair<access_path, std::size_t>;

// The members of each path `x.a.b` of a text, by the token of `x`, without a
// member function it calls.
using member_paths = std::unordered_map<std::size_t, std::vector<std::string>>;

auto paths_of(const expression_text& text) -> member_paths {
    auto result = member_paths{};
    const auto& tokens = text.tokens;
    const auto visit = [&](const auto& self, const expression& node, bool callee) -> void {
        if (node.kind == expression_kind::member) {
            auto names = std::vector<std::string>{};
            const auto* object = &node;
            while (object->kind == expression_kind::member && tokens[object->op].text == "." &&
                   object->children.size() == 2 && is_single_name(object->children[1])) {
                names.insert(names.begin(), tokens[object->children[1].first].text);
                object = &object->children[0];
            }
            if (object != &node && is_single_name(*object)) {
                if (callee) names.pop_back();
                result[object->first] = std::move(names);
                return;
            }
        }
        for (auto index = std::size_t{0}; index < node.children.size(); ++index) {
            self(self, node.children[index], node.kind == expression_kind::call && index == 0);
        }
    };
    visit(visit, text.root, false);
    return result;
}

// An argument of one call: the receiver of a member or UFCS call counts as the
// first argument.
struct call_argument final {
    const expression* node = nullptr;
    // The object the argument binds as a whole, when it is a name or a member
    // or subscript path rooted at one.
    access_path binds;
    // Every object a name mentioned anywhere in the argument accesses.
    std::vector<argument_mention> mentions;
};

// Rejects a call that passes an object `inout` or `out` to a function declared
// in this translation unit while another argument of the same call mentions
// the same object or a part of it. Cpp2 does not mark `inout` at the call
// site, so the parameter passing comes from the callee's declaration, and a
// call whose callee cannot be resolved is not checked.
class exclusivity_checker final {
public:
    exclusivity_checker(const ir_program& program, const scope_model& scopes,
                        const token_list& tokens)
        : program_{program}, tokens_{tokens}, scopes_{scopes}, types_{program, scopes} {}

    auto check() -> std::vector<diagnostic> {
        // A generated member may share its source member's body, which is checked once.
        auto checked = std::set<const function_body*>{};
        ir_detail::visit_declarations(program_.declarations,
            [&](const ir_declaration& declaration) {
                if (declaration.function_statements) {
                    if (checked.insert(declaration.function_statements.get()).second) {
                        ir_detail::visit_statements(declaration.function_statements->statements,
                            [&](const statement& value) { check(value); });
                    }
                }
                else if (!declaration.is_generated && !declaration.body.empty() &&
                         (declaration.kind == declaration_kind::object ||
                          declaration.kind == declaration_kind::function)) {
                    check(declaration.body, scopes_.inside(declaration.symbol),
                          declaration.span);
                }
            });
        return std::move(diagnostics_);
    }

private:
    const ir_program& program_;
    const token_list& tokens_;
    const scope_model& scopes_;
    known_types types_;
    std::vector<diagnostic> diagnostics_;

    auto check(const statement& value) -> void {
        const auto point = scopes_.at(value);
        const auto check_text = [&](const parsed_expression& text, scope_point at) {
            check(text, at, value.span);
        };
        std::visit([&](const auto& form) {
            using form_type = std::remove_cvref_t<decltype(form)>;
            if constexpr (std::is_same_v<form_type, statement::expression_form>) {
                check_text(form.text, point);
            }
            else if constexpr (std::is_same_v<form_type, statement::declaration_form>) {
                if (!form.declaration) {
                    check_text(form.text, point);
                }
                else if (form.declaration->kind == declaration_kind::object &&
                         !form.declaration->is_alias && !form.declaration->initializer.empty()) {
                    check_text(form.declaration->initializer, point);
                }
            }
            else if constexpr (std::is_same_v<form_type, statement::destructuring_form>) {
                check_text(form.initializer, point);
            }
            else if constexpr (std::is_same_v<form_type, statement::block_form>) {
                for (const auto& parameter : form.parameters) {
                    check_text(parameter.initializer, scopes_.at(parameter));
                }
            }
            else if constexpr (std::is_same_v<form_type, statement::if_form>) {
                check_text(form.condition, point);
            }
            else if constexpr (std::is_same_v<form_type, statement::loop_form>) {
                check_text(form.condition, point);
                check_text(form.next, point);
            }
            else if constexpr (std::is_same_v<form_type, statement::contract_form>) {
                check_text(form.condition, point);
                check_text(form.message, point);
            }
        }, value.form);
    }

    // `span` is the statement or declaration that holds `source`.
    auto check(const parsed_expression& source, scope_point point, source_span span) -> void {
        if (!point.valid()) return;
        const auto& text = source.tree();
        // The mentions that are uses, including those in captures and string
        // interpolations, in token order.
        auto paths = std::unordered_map<const expression_text*, member_paths>{};
        paths.emplace(&text, paths_of(text));
        auto mentions = std::vector<argument_mention>{};
        for_each_mention(text, [&](const mention& value) {
            if (value.compile_time || value.function_name || !is_identifier(*value.text, value.token)) {
                return;
            }
            auto found = paths.find(value.text);
            if (found == paths.end()) {
                found = paths.emplace(value.text, paths_of(*value.text)).first;
            }
            const auto access = access_at(*value.text, value.token, point, found->second);
            if (access.object != nullptr) mentions.emplace_back(access, value.path.front());
        }, &program_.function_expressions);
        const auto visit = [&](const auto& self, const expression& node) -> void {
            if (node.kind == expression_kind::call) {
                check_call(source, node, mentions, paths.at(&text), point, span);
            }
            for (const auto& child : node.children) self(self, child);
        };
        visit(visit, text.root);
    }

    [[nodiscard]] static auto token_text(const expression_text& text, std::size_t index)
        -> std::string_view {
        return index < text.tokens.size() ? std::string_view{text.tokens[index].text}
                                          : std::string_view{};
    }

    [[nodiscard]] static auto is_identifier(const expression_text& text, std::size_t index)
        -> bool {
        return index < text.tokens.size() &&
            text.tokens[index].kind == expression_token_kind::identifier &&
            text.tokens[index].text.find("::") == std::string::npos;
    }

    [[nodiscard]] static auto single_name(const expression_text& text, const expression& node)
        -> bool {
        return node.kind == expression_kind::name && node.last == node.first + 1 &&
            is_identifier(text, node.first);
    }

    // The object, parameter or function `name` names at `point`.
    [[nodiscard]] auto lookup(std::string_view name, scope_point point) const
        -> const scope_entry* {
        const auto* found = scopes_.lookup(name, point);
        return found != nullptr ? scopes_.resolve(*found) : nullptr;
    }

    [[nodiscard]] static auto is_object(const scope_entry& value) noexcept -> bool {
        using kind = scope_model::entry_kind;
        return value.kind == kind::object || value.kind == kind::parameter ||
            value.kind == kind::statement_parameter;
    }

    // Whether `function` names a member function of the enclosing type `type`.
    [[nodiscard]] auto is_member_function(const scope_entry& function,
                                          const scope_entry* type) const -> bool {
        if (type == nullptr || !scopes_.declared_in(function, *type)) return false;
        const auto candidates = types_.overloads(&function);
        return std::any_of(candidates.begin(), candidates.end(),
            [](const auto* value) { return cppfrontier::receiver(*value) != nullptr; });
    }

    // The access of the name at token `index`, with its members in `paths`:
    // `this` or a local, parameter or member, or its part `this.a`, `x.a.b`.
    [[nodiscard]] auto access_at(const expression_text& text, std::size_t index,
                                 scope_point point, const member_paths& paths) const
        -> access_path {
        const auto name = token_text(text, index);
        const auto* type = scopes_.enclosing_type(point);
        const auto path = paths.find(index);
        auto parts = path != paths.end() ? path->second : std::vector<std::string>{};
        if (name == "this") {
            if (type == nullptr) return {};
            const auto* member = parts.empty() ? nullptr : scopes_.member(*type, parts.front());
            if (member == nullptr || !is_object(*member)) parts.clear();
            return {type, std::move(parts)};
        }
        const auto* found = lookup(name, point);
        if (found == nullptr) return {};
        // A member function called on the implicit `this` reads or writes `this`.
        if (found->kind == scope_model::entry_kind::function) {
            return is_member_function(*found, type) ? access_path{type, {}} : access_path{};
        }
        if (!is_object(*found)) return {};
        if (type != nullptr && scopes_.declared_in(*found, *type)) {
            parts.insert(parts.begin(), found->name);
            return {type, std::move(parts)};
        }
        return {found, std::move(parts)};
    }

    // The object a name, member path or subscript path binds as a whole.
    [[nodiscard]] auto bound_object(const expression_text& text, const expression& node,
                                    scope_point point, const member_paths& paths) const
        -> access_path {
        const auto* root = &node;
        while (root->kind == expression_kind::member || root->kind == expression_kind::subscript ||
               root->kind == expression_kind::group) {
            if (root->children.empty()) return {};
            root = &root->children.front();
        }
        return single_name(text, *root) ? access_at(text, root->first, point, paths)
                                        : access_path{};
    }

    // The mentions of `mentions` in `node`.
    [[nodiscard]] static auto mentions_in(const std::vector<argument_mention>& mentions,
                                          const expression& node)
        -> std::vector<argument_mention> {
        auto result = std::vector<argument_mention>{};
        std::copy_if(mentions.begin(), mentions.end(), std::back_inserter(result),
            [&](const auto& value) { return value.second >= node.first && value.second < node.last; });
        return result;
    }

    auto check_call(const parsed_expression& source, const expression& call,
                    const std::vector<argument_mention>& mentions, const member_paths& paths,
                    scope_point point, source_span span) -> void {
        const auto& text = source.tree();
        const auto candidates = types_.callees(text, call, point);
        if (candidates.functions.empty()) return;
        // Argument 0 is the receiver: the object of `x.f()`, or the implicit
        // `this` of an unqualified member function call, which no token spells.
        auto arguments = std::vector<call_argument>(call.children.size());
        const auto& callee = call.children.front();
        if (candidates.receiver) {
            const auto* object = &callee.children.front();
            while (object->kind == expression_kind::group && object->children.size() == 1) {
                object = &object->children.front();
            }
            arguments[0] = {object, bound_object(text, *object, point, paths),
                            mentions_in(mentions, *object)};
        }
        else if (const auto* type = scopes_.enclosing_type(point);
                 callee.kind == expression_kind::name && type != nullptr &&
                 std::any_of(candidates.functions.begin(), candidates.functions.end(),
                     [](const auto* value) { return cppfrontier::receiver(*value) != nullptr; })) {
            arguments[0] = {nullptr, {type, {}}, {{{type, {}}, callee.first}}};
        }
        for (auto index = std::size_t{1}; index < call.children.size(); ++index) {
            const auto* node = &call.children[index];
            if (node->kind == expression_kind::argument && !node->children.empty()) {
                node = &node->children.front();
            }
            arguments[index] = {node, bound_object(text, *node, point, paths),
                                mentions_in(mentions, *node)};
        }
        const auto passed = [&](std::size_t index, passing_style passing) {
            return candidates.all_bind(index, [&](const parameter& found) {
                return found.passing == passing ||
                    (passing == passing_style::inout && found.passing == passing_style::out);
            });
        };
        for (auto later = std::size_t{1}; later < arguments.size(); ++later) {
            for (auto earlier = std::size_t{0}; earlier < later; ++earlier) {
                // The token of `other` that overlaps the object `index` passes exclusively.
                const auto conflict = [&](std::size_t index, std::size_t other)
                    -> std::optional<std::size_t> {
                    const auto& bound = arguments[index].binds;
                    if (bound.object == nullptr || !passed(index, passing_style::inout)) {
                        return std::nullopt;
                    }
                    for (const auto& [access, token] : arguments[other].mentions) {
                        if (bound.overlaps(access)) return token;
                    }
                    return std::nullopt;
                };
                const auto word = [&](std::size_t index) {
                    return passed(index, passing_style::out) ? "out" : "inout";
                };
                if (const auto token = conflict(earlier, later)) {
                    report(source, arguments[earlier].binds, word(earlier), *token, span);
                    break;
                }
                if (conflict(later, earlier)) {
                    report(source, arguments[later].binds, word(later),
                           arguments[later].mentions.front().second, span);
                    break;
                }
            }
        }
    }

    [[nodiscard]] static auto spell(const access_path& access) -> std::string {
        auto result = access.object->kind == scope_model::entry_kind::type ? std::string{}
                                                                           : access.object->name;
        for (const auto& part : access.parts) result += (result.empty() ? "" : ".") + part;
        return result.empty() ? "this" : result;
    }

    auto report(const parsed_expression& source, const access_path& object,
                std::string_view passing, std::size_t token, source_span span) -> void {
        const auto begin = source.position_of(token);
        const auto at = begin
            ? source_span{*begin, {begin->line, begin->column + static_cast<std::uint32_t>(
                                                     source.tree().tokens[token].text.size())}}
            : locate(source.tree(), token, span);
        diagnostics_.push_back({"exclusivity-violation", diagnostic_severity::error, at,
            "'" + spell(object) + "' is passed " + std::string{passing} +
                " and also used by another argument of the same call"});
    }

    // The span of the expression token `index` of a text that does not know
    // where it begins, in the statement at `span`: the token with that text
    // and ordinal there.
    [[nodiscard]] auto locate(const expression_text& text, std::size_t index,
                              source_span span) const -> source_span {
        const auto word = token_text(text, index);
        auto ordinal = std::count_if(text.tokens.begin(), text.tokens.begin() +
            static_cast<std::ptrdiff_t>(index),
            [&](const auto& token) { return token.text == word; });
        const auto matches = [&](std::string_view candidate) {
            return candidate == word ||
                (candidate.starts_with(word) && (candidate.substr(word.size()).starts_with("::") ||
                                                 candidate.substr(word.size()) == "*"));
        };
        const auto key = [](source_position position) {
            return std::pair{position.line, position.column};
        };
        auto first = std::lower_bound(tokens_.begin(), tokens_.end(), span.begin,
            [&](const token& value, source_position position) {
                return key(value.span.begin) < key(position);
            });
        const auto limit = std::min<std::size_t>(tokens_.size(),
            static_cast<std::size_t>(first - tokens_.begin()) + text.tokens.size() + 8);
        for (auto position = static_cast<std::size_t>(first - tokens_.begin()); position < limit;
             ++position) {
            if (matches(tokens_[position].text) && ordinal-- == 0) return tokens_[position].span;
        }
        return span;
    }
};

}

auto check_exclusivity(const ir_program& program, const scope_model& scopes,
                       const token_list& tokens) -> std::vector<diagnostic> {
    return exclusivity_checker{program, scopes, tokens}.check();
}

}
