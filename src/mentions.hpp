#pragma once

#include <cppfrontier/expression.hpp>
#include <cppfrontier/ir.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "cpp1_expression_renderer.hpp"

namespace cppfrontier {

// A single-token name.
inline auto is_single_name(const expression& node) -> bool {
    return node.kind == expression_kind::name && node.last == node.first + 1;
}

// The spelling of a name up to its first template argument list, as `N::f`
// for `N::f<T>`; empty for another expression.
inline auto name_spelling(const expression_text& text, const expression& node) -> std::string {
    auto result = std::string{};
    if (node.kind != expression_kind::name) return result;
    for (auto index = node.first; index < node.last && text.tokens[index].text != "<"; ++index) {
        result += text.tokens[index].text;
    }
    return result;
}

// The expression a text consists of, ignoring a trailing `;`, or null.
inline auto whole_expression(const expression_text& text) -> const expression* {
    const auto& tokens = text.tokens;
    const auto end = !tokens.empty() && tokens.back().text == ";" ? tokens.size() - 1
                                                                   : tokens.size();
    const auto& children = text.root.children;
    if (children.size() != 1 || children.front().first != 0 || children.front().last != end) {
        return nullptr;
    }
    return &children.front();
}

// Whether child `index` of `node` is in a compile-time context, where a
// mention is not a use: a template argument, a part of a requires-expression,
// or an operand of `decltype`, `sizeof`, `sizeof...`, `alignof` or `noexcept`.
// `typeid` evaluates a polymorphic glvalue operand, so its operand is a use.
inline auto compile_time_child(const expression_text& text, const expression& node,
                               std::size_t index) -> bool {
    if (node.kind == expression_kind::name || node.kind == expression_kind::requires_expression) {
        return true;
    }
    if (node.kind != expression_kind::call || index == 0) return false;
    const auto& callee = node.children.front();
    const auto pack = callee.kind == expression_kind::postfix &&
        text.tokens[callee.op].text == "...";
    const auto& operator_name = pack ? callee.children.front() : callee;
    if (!is_single_name(operator_name)) return false;
    const auto& name = text.tokens[operator_name.first].text;
    return name == "sizeof" || (!pack && (name == "decltype" || name == "alignof" ||
                                          name == "noexcept"));
}

enum class mention_role : std::uint8_t {
    read,
    moved,     // `move name` as a whole argument
    assigned,  // `name = value`
    out,       // `out name` as a whole argument
};

// A name that an expression text mentions: the leading identifier of an
// unqualified name, or the function name of a UFCS call `x.f()`; other
// member names are not mentions. String interpolations are texts of their
// own, and with `expressions` the captures of a function expression are
// mentioned where it appears.
struct mention final {
    // The text holding the mention, and its token there.
    const expression_text* text = nullptr;
    std::size_t token = 0;
    // Token indices from the outermost text, as `last_use::path`.
    std::vector<std::size_t> path;
    mention_role role = mention_role::read;
    // In a compile-time context, where a mention is not a use.
    bool compile_time = false;
    // Inside a fold or the operand of a pack expansion `x...`, where it may be
    // evaluated more than once.
    bool expanded = false;
    // The function name of a UFCS call `x.f()`.
    bool function_name = false;
    // In the capture of a function expression, `x$`, which it initializes:
    // the capture's text.
    const parsed_expression* capture = nullptr;
    // A whole argument of `call`, as its child `argument`; the object of a
    // UFCS call is argument 0.
    const expression* call = nullptr;
    std::size_t argument = 0;
    // In the subject of a pattern test (experiment `condition-bindings`),
    // which its bindings refer to.
    bool subject = false;
    // The inspect expressions whose alternative's value holds the mention,
    // each with that alternative, outermost first.
    std::vector<std::pair<const expression*, const expression*>> alternatives;

    [[nodiscard]] auto name() const -> const std::string& { return text->tokens[token].text; }
};

// Calls `visit` with each mention in `text`, in token order.
template<class Visit>
auto for_each_mention(const expression_text& text, const Visit& visit,
                      const std::vector<function_expression>* expressions = nullptr,
                      const std::vector<std::size_t>& prefix = {}, bool compile_time = false,
                      bool expanded = false, const parsed_expression* capture = nullptr)
    -> void {
    const auto& tokens = text.tokens;
    struct context final {
        bool compile_time = false;
        bool expanded = false;
        mention_role role = mention_role::read;
        // The call this node is a whole argument of.
        const expression* call = nullptr;
        std::size_t argument = 0;
        // The call this node is the callee of.
        const expression* callee_of = nullptr;
    };
    // The pattern test subjects being walked.
    auto subjects = 0;
    // The inspect expressions being walked, and the alternative values.
    auto inspects = std::vector<const expression*>{};
    auto alternatives = std::vector<std::pair<const expression*, const expression*>>{};
    const auto report = [&](std::size_t index, const context& where, bool function_name) {
        auto path = prefix;
        path.push_back(index);
        const auto& name = tokens[index].text;
        if (expressions != nullptr && is_function_expression_name(name)) {
            const auto found = std::find_if(expressions->begin(), expressions->end(),
                [&](const auto& value) { return value.name == name; });
            if (found == expressions->end()) return;
            for (const auto& capture : found->captures) {
                for_each_mention(capture.expression.tree(), visit, expressions, path,
                                 where.compile_time, where.expanded, &capture.expression);
            }
            return;
        }
        visit(mention{&text, index, std::move(path), where.role, where.compile_time,
                      where.expanded, function_name, capture, where.call, where.argument,
                      subjects > 0, alternatives});
    };
    // The names bound by the enclosing for-comprehensions (experiment
    // `comprehension`), inspect alternatives (experiment `patterns`) and
    // earlier pattern tests of a condition (experiment `condition-bindings`).
    auto bound = std::vector<std::string_view>{};
    const auto walk = [&](const auto& self, const expression& node, const context& where) -> void {
        // A context that is not a whole argument, callee or assignment target.
        const auto plain = context{where.compile_time, where.expanded};
        const auto child = [&](std::size_t index) {
            return context{where.compile_time || compile_time_child(text, node, index),
                           where.expanded || node.kind == expression_kind::fold ||
                               (node.kind == expression_kind::postfix &&
                                tokens[node.op].text == "...")};
        };
        switch (node.kind) {
        case expression_kind::name:
            if (tokens[node.first].kind == expression_token_kind::identifier &&
                (node.last == node.first + 1 || tokens[node.first + 1].text == "<") &&
                std::find(bound.begin(), bound.end(), tokens[node.first].text) == bound.end()) {
                report(node.first, is_single_name(node) ? where : plain, false);
            }
            break;
        case expression_kind::literal:
            for (auto index = node.first; index < node.last; ++index) {
                if (tokens[index].kind != expression_token_kind::string) continue;
                const auto nested = interpolated_expressions(tokens[index].text);
                auto path = prefix;
                path.push_back(index);
                for (auto ordinal = std::size_t{0}; ordinal < nested.size(); ++ordinal) {
                    path.push_back(ordinal);
                    for_each_mention(parse_expression_text(nested[ordinal]), visit, expressions,
                                     path, where.compile_time, where.expanded);
                    path.pop_back();
                }
            }
            return;
        case expression_kind::member: {
            const auto& member = node.children[1];
            const auto ufcs = where.callee_of != nullptr && tokens[node.op].text == ".";
            auto object = child(0);
            if (ufcs) object.call = where.callee_of;
            self(self, node.children[0], object);
            if (ufcs && tokens[where.callee_of->op].text == "(" && is_single_name(member)) {
                report(member.first, child(1), true);
            }
            for (const auto& argument : member.children) {
                self(self, argument, context{true, where.expanded});
            }
            return;
        }
        case expression_kind::call: {
            auto callee = child(0);
            callee.callee_of = &node;
            self(self, node.children[0], callee);
            for (auto index = std::size_t{1}; index < node.children.size(); ++index) {
                auto argument = child(index);
                argument.call = &node;
                argument.argument = index;
                self(self, node.children[index], argument);
            }
            return;
        }
        case expression_kind::argument: {
            const auto& op = tokens[node.op].text;
            self(self, node.children.front(), context{where.compile_time, where.expanded,
                op == "move" ? mention_role::moved : op == "out" ? mention_role::out
                                                                 : mention_role::read});
            return;
        }
        case expression_kind::assignment:
            self(self, node.children[0], context{where.compile_time, where.expanded,
                tokens[node.op].text == "=" ? mention_role::assigned : mention_role::read});
            self(self, node.children[1], child(1));
            return;
        case expression_kind::comprehension:
            // The bound name is not a mention, nor are its uses in the guard and value.
            self(self, node.children[0], child(0));
            bound.push_back(tokens[node.children[1].first].text);
            for (auto index = std::size_t{2}; index < node.children.size(); ++index) {
                self(self, node.children[index], child(index));
            }
            bound.pop_back();
            return;
        case expression_kind::alternative: {
            // The names an inspect alternative binds (experiment `patterns`)
            // are not mentions, nor are their uses in its guard and value.
            const auto parts = inspect_alternative_of(tokens, node);
            for (const auto* pattern : parts.patterns) self(self, *pattern, plain);
            const auto depth = bound.size();
            if (parts.destructuring != nullptr) {
                for (const auto& item : parts.destructuring->children) {
                    bound.push_back(tokens[item.first].text);
                }
            }
            if (parts.binding != nullptr) bound.push_back(tokens[parts.binding->first].text);
            if (parts.guard != nullptr) self(self, *parts.guard, plain);
            if (!inspects.empty()) alternatives.emplace_back(inspects.back(), &node);
            self(self, *parts.value, plain);
            if (!inspects.empty()) alternatives.pop_back();
            bound.resize(depth);
            return;
        }
        case expression_kind::pattern_test: {
            // The names a pattern test binds are not mentions, nor are their
            // uses in the later operands of its `&&` chain.
            ++subjects;
            self(self, node.children[0], plain);
            --subjects;
            const auto parts = inspect_alternative_of(tokens, node.children[1]);
            for (const auto* pattern : parts.patterns) self(self, *pattern, plain);
            if (parts.destructuring != nullptr) {
                for (const auto& item : parts.destructuring->children) {
                    bound.push_back(tokens[item.first].text);
                }
            }
            if (parts.binding != nullptr) bound.push_back(tokens[parts.binding->first].text);
            return;
        }
        default:
            break;
        }
        const auto depth = bound.size();
        if (node.kind == expression_kind::inspect) inspects.push_back(&node);
        for (auto index = std::size_t{0}; index < node.children.size(); ++index) {
            self(self, node.children[index], child(index));
        }
        if (node.kind == expression_kind::inspect) inspects.pop_back();
        bound.resize(depth);
    };
    walk(walk, text.root, context{compile_time, expanded});
}

}
