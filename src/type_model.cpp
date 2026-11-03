#include <cppfrontier/type.hpp>

#include "brackets.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace cppfrontier {
namespace {

struct fundamental_alias final {
    std::string_view cpp2;
    std::string_view cpp1;
};

constexpr std::array<fundamental_alias, 16> fundamental_aliases{{
    {"i8", "signed char"}, {"i16", "short"}, {"i32", "int"},
    {"i64", "std::int64_t"}, {"u8", "unsigned char"}, {"u16", "unsigned short"},
    {"u32", "unsigned int"}, {"u64", "std::uint64_t"},
    {"uint", "unsigned int"}, {"longdouble", "long double"}, {"longlong", "long long"},
    {"ulonglong", "unsigned long long"}, {"ulong", "unsigned long"},
    {"ushort", "unsigned short"}, {"_schar", "signed char"},
    {"_uchar", "unsigned char"},
}};

// Words that combine into a multi-word Cpp1 fundamental type.
constexpr std::array<std::string_view, 7> multi_word_fundamentals{{
    "char", "short", "int", "long", "double", "signed", "unsigned",
}};

constexpr std::array<std::string_view, 9> single_word_fundamentals{{
    "void", "bool", "float", "wchar_t", "char8_t", "char16_t", "char32_t", "size_t", "nullptr_t",
}};

constexpr std::array<std::string_view, 7> passing_words{{
    "in", "in_ref", "copy", "inout", "out", "move", "forward",
}};

auto passing_of(std::string_view word) -> passing_style {
    if (word == "in_ref") return passing_style::in_ref;
    if (word == "copy") return passing_style::copy;
    if (word == "inout") return passing_style::inout;
    if (word == "out") return passing_style::out;
    if (word == "move") return passing_style::move;
    if (word == "forward") return passing_style::forward;
    return passing_style::in;
}

auto advance(source_position position, std::string_view text) -> source_position {
    for (const auto value : text) {
        if (value == '\n') {
            ++position.line;
            position.column = 1;
        }
        else {
            ++position.column;
        }
    }
    return position;
}

class type_parser final {
public:
    type_parser(const std::vector<expression_token>& tokens, std::optional<source_span> span,
                type_parse_options options = {})
        : tokens_{tokens}, span_{span}, options_{options} {
        offsets_.reserve(tokens.size());
        for (const auto& token : tokens) {
            joined_ += token.leading;
            offsets_.push_back(joined_.size());
            joined_ += token.text;
        }
        if (!span_) return;
        starts_.reserve(tokens.size());
        auto position = span_->begin;
        for (const auto& token : tokens) {
            position = advance(position, std::string_view{token.leading});
            starts_.push_back(position);
            position = advance(position, std::string_view{token.text});
        }
    }

    auto parse(std::size_t first, std::size_t last) -> type_id {
        auto result = type_id{};
        if (first >= last) return result;
        if (options_.type_operators) {
            // `type A | B` is `A | B`, as in a template argument marked a type.
            const auto begin = word(first, "type") && first + 1 < last ? first + 1 : first;
            if (auto combined = combination(begin, last)) return std::move(*combined);
            // A parenthesized operand groups.
            if (text(first) == "(" && matching(first, last) == last - 1 &&
                (operator_at(first + 1, last - 1, "|") != last - 1 ||
                 operator_at(first + 1, last - 1, "&") != last - 1)) {
                return parse(first + 1, last - 1);
            }
        }
        if (word(first, "type") && first + 1 < last &&
            (tokens_[first + 1].kind == expression_token_kind::identifier ||
             text(first + 1) == "::")) {
            result = parse(first + 1, last);
            result.core().has_type_marker = true;
        }
        else if (text(first) == "*" || word(first, "const")) {
            result.kind = text(first) == "*" ? type_kind::pointer : type_kind::constant;
            // A type that ends in a qualifier omits the `_` wildcard: `p: * = x&;`.
            result.children.push_back(first + 1 < last
                ? parse(first + 1, last)
                : type_id{.kind = type_kind::deduced, .text = "_"});
        }
        else {
            result = core(first, last);
        }
        locate(result, first, last);
        // `type decltype(x)::m` is rendered from its text, which omits the marker.
        if (result.kind == type_kind::cpp1 && result.has_type_marker) {
            result.text = spelling(first + 1, last);
        }
        return result;
    }

private:
    const std::vector<expression_token>& tokens_;
    std::optional<source_span> span_;
    type_parse_options options_;
    // The tokens with their leading text, the offset of each token's text in
    // it, and, with a span, each token's source position.
    std::string joined_;
    std::vector<std::size_t> offsets_;
    std::vector<source_position> starts_;

    [[nodiscard]] auto text(std::size_t index) const -> std::string_view {
        return index < tokens_.size() ? std::string_view{tokens_[index].text} : std::string_view{};
    }

    [[nodiscard]] auto word(std::size_t index, std::string_view value) const -> bool {
        return index < tokens_.size() && tokens_[index].kind == expression_token_kind::identifier &&
            tokens_[index].text == value;
    }

    [[nodiscard]] auto identifier(std::size_t index) const -> bool {
        return index < tokens_.size() && tokens_[index].kind == expression_token_kind::identifier;
    }

    // Tokens [first, last) with the whitespace and comments between them.
    [[nodiscard]] auto spelling(std::size_t first, std::size_t last) const -> std::string {
        if (first >= last) return {};
        const auto end = offsets_[last - 1] + tokens_[last - 1].text.size();
        return joined_.substr(offsets_[first], end - offsets_[first]);
    }

    auto locate(type_id& result, std::size_t first, std::size_t last) const -> void {
        result.text = spelling(first, last);
        if (!span_ || first >= last) return;
        result.span.begin = starts_[first];
        result.span.end = advance(starts_[last - 1], std::string_view{tokens_[last - 1].text});
    }

    // The index of the token closing the bracket opened at `open`, or nullopt.
    [[nodiscard]] auto matching(std::size_t open, std::size_t last) const
        -> std::optional<std::size_t> {
        last = std::min(last, tokens_.size());
        if (open >= last) return std::nullopt;
        return text(open) == "<" ? detail::angle_close(tokens_, open, last)
                                 : detail::bracket_close(tokens_, open, last);
    }

    // The first top-level `symbol` in [first, last), or `last`. A function
    // type `(...) -> R` and a constrained `_ is C` have no top-level operators:
    // the return type and the constraint extend to the end.
    [[nodiscard]] auto operator_at(std::size_t first, std::size_t last, std::string_view symbol)
        const -> std::size_t {
        last = std::min(last, tokens_.size());
        if (word(first, "_") && word(first + 1, "is")) return last;
        if (text(first) == "(") {
            const auto close = matching(first, last);
            if (close && *close + 1 < last && text(*close + 1) == "->") return last;
        }
        const auto found = detail::find_top_level(tokens_, first, last,
            [&](std::size_t open) { return matching(open, last); },
            [&](std::size_t index) { return text(index) == symbol; });
        return found < last && text(found) == symbol ? found : last;
    }

    // `A | B` or `A & B` in [first, last), or nullopt without a top-level
    // operator. Nested operands of the same kind are flattened; an empty
    // operand becomes a child of kind `none`, which the caller reports.
    auto combination(std::size_t first, std::size_t last) -> std::optional<type_id> {
        for (const auto [symbol, kind] : {std::pair{"|", type_kind::union_type},
                                          std::pair{"&", type_kind::intersection_type}}) {
            if (operator_at(first, last, symbol) == last) continue;
            auto result = type_id{};
            result.kind = kind;
            for (auto begin = first; begin <= last;) {
                const auto end = operator_at(begin, last, symbol);
                auto operand = parse(begin, end);
                if (operand.kind == kind) {
                    for (auto& child : operand.children) result.children.push_back(std::move(child));
                }
                else {
                    result.children.push_back(std::move(operand));
                }
                begin = end + 1;
            }
            locate(result, first, last);
            return result;
        }
        return std::nullopt;
    }

    // Ranges separated by top-level commas in [first, last).
    [[nodiscard]] auto split(std::size_t first, std::size_t last) const
        -> std::vector<std::pair<std::size_t, std::size_t>> {
        last = std::min(last, tokens_.size());
        auto result = std::vector<std::pair<std::size_t, std::size_t>>{};
        for (auto begin = first;;) {
            const auto comma = detail::find_top_level(tokens_, begin, last,
                [&](std::size_t open) { return detail::angle_close(tokens_, open, last); },
                [&](std::size_t index) { return text(index) == ","; });
            if (comma >= last || text(comma) != ",") {
                result.emplace_back(begin, last);
                return result;
            }
            result.emplace_back(begin, comma);
            begin = comma + 1;
        }
    }

    // Text kept as written; `parse` records it.
    static auto passthrough() -> type_id {
        auto result = type_id{};
        result.kind = type_kind::cpp1;
        return result;
    }

    auto core(std::size_t first, std::size_t last) -> type_id {
        auto result = type_id{};
        if (text(first) == "(") {
            const auto close = matching(first, last);
            if (close && *close + 2 < last && text(*close + 1) == "->") {
                return function(first, *close, last);
            }
            return passthrough();
        }
        if (word(first, "_") || word(first, "auto")) {
            result.kind = type_kind::deduced;
            if (first + 1 == last) return result;
            if (word(first, "_") && word(first + 1, "is") && first + 2 < last) {
                result.children.push_back(parse(first + 2, last));
                return result;
            }
            return passthrough();
        }
        auto position = first;
        if ((word(first, "decltype") || word(first, "type_of")) && text(first + 1) == "(") {
            const auto close = matching(first + 1, last);
            if (!close) return passthrough();
            result.kind = type_kind::decltype_of;
            position = *close + 1;
        }
        else if (is_cpp1_fundamental_word(text(first)) && identifier(first)) {
            result.kind = type_kind::fundamental;
            while (position < last && identifier(position) &&
                   is_cpp1_fundamental_word(text(position))) {
                ++position;
            }
        }
        else if (identifier(first) && text(first + 1) != "::" &&
                 (!cpp1_fundamental_spelling(text(first)).empty() ||
                  std::find(single_word_fundamentals.begin(), single_word_fundamentals.end(),
                            text(first)) != single_word_fundamentals.end())) {
            result.kind = type_kind::fundamental;
            ++position;
        }
        else if (const auto end = name(first, last, result)) {
            position = *end;
        }
        else {
            return passthrough();
        }
        locate(result, first, position);
        // `T...` and the Cpp1 references `T&` and `T&&`.
        while (position < last) {
            auto wrapper = type_id{};
            if (text(position) == "...") wrapper.kind = type_kind::pack;
            else if (text(position) == "&" || text(position) == "&&") {
                wrapper.kind = type_kind::reference;
                wrapper.is_rvalue = text(position) == "&&";
            }
            else break;
            wrapper.children.push_back(std::move(result));
            result = std::move(wrapper);
            ++position;
            locate(result, first, position);
        }
        if (position != last) return passthrough();
        return result;
    }

    // A possibly qualified name starting at `first`; returns the index after it.
    auto name(std::size_t first, std::size_t last, type_id& result) -> std::optional<std::size_t> {
        result.kind = type_kind::name;
        auto position = first;
        if (text(position) == "::") {
            result.is_global = true;
            ++position;
        }
        for (;;) {
            auto component = type_name_component{};
            if (!result.components.empty() && word(position, "template")) {
                component.has_template_keyword = true;
                ++position;
            }
            if (position >= last || !identifier(position)) return std::nullopt;
            component.identifier = tokens_[position].text;
            ++position;
            if (position < last && text(position) == "<") {
                const auto close = matching(position, last);
                if (!close) return std::nullopt;
                component.has_arguments = true;
                if (*close > position + 1) {
                    for (const auto& [begin, end] : split(position + 1, *close)) {
                        component.arguments.push_back(argument(begin, end));
                    }
                }
                position = *close + 1;
            }
            result.components.push_back(std::move(component));
            if (position + 1 < last && text(position) == "::") {
                ++position;
                continue;
            }
            return position;
        }
    }

    // A template argument: `:T = v`, member accesses such as `o.f()` and
    // anything else that is not a type-id, such as `(1 > 0) as int`, are
    // values; Cpp1 declarators such as `int*` and `int[2]` stay types. `|`
    // and `&` are Cpp1 operators unless `type` marks the argument a type:
    // `std::bitset<A | B>`, `std::vector<type A | B>`.
    auto argument(std::size_t first, std::size_t last) -> type_id {
        auto is_expression = first < last && text(first) == ":";
        const auto marked = word(first, "type");
        if (marked && options_.type_operators) {
            auto combined = parse(first + 1, last);
            if (combined.is_operator()) return combined;
        }
        if (!word(first + (marked ? 1 : 0), "decltype")) {
            for (auto index = first; index < last && !is_expression; ++index) {
                is_expression = text(index) == "." &&
                    (index + 1 >= last || tokens_[index + 1].kind != expression_token_kind::number);
            }
        }
        if (!is_expression) {
            auto result = parse(first, last);
            const auto end = text(last - 1);
            if (!result.is_operator() &&
                (result.kind != type_kind::cpp1 || result.core().has_type_marker || end == "*" ||
                 end == "&" || end == "&&" ||
                 end == "]" || word(last - 1, "const"))) {
                return result;
            }
        }
        auto result = type_id{};
        result.kind = type_kind::expression;
        locate(result, first, last);
        return result;
    }

    auto function(std::size_t open, std::size_t close, std::size_t last) -> type_id {
        auto result = type_id{};
        result.kind = type_kind::function;
        if (close > open + 1) {
            for (const auto& [begin, end] : split(open + 1, close)) {
                if (begin == end) continue;
                auto position = begin;
                auto parameter = type_parameter{};
                if (std::find(passing_words.begin(), passing_words.end(), text(position)) !=
                        passing_words.end() &&
                    position + 1 < end && text(position + 1) != ":") {
                    parameter.passing = passing_of(text(position));
                    ++position;
                }
                // A parameter is `name: type` with a passing style other than
                // `forward`; any other form, such as `this`, a pack or a
                // default, has no type, which is diagnosed.
                auto type = type_id{};
                const auto defaulted = detail::find_top_level(tokens_, position, end,
                    [&](std::size_t open) { return detail::angle_close(tokens_, open, end); },
                    [&](std::size_t index) { return text(index) == "="; }) < end;
                if (identifier(position) && position + 2 < end && text(position + 1) == ":" &&
                    text(position) != "this" && parameter.passing != passing_style::forward &&
                    !defaulted) {
                    parameter.name = tokens_[position].text;
                    type = parse(position + 2, end);
                }
                result.parameters.push_back(std::move(parameter));
                result.children.push_back(std::move(type));
            }
        }
        auto returns = close + 2;
        if (word(returns, "forward") && returns + 1 < last) {
            result.returns_forward = true;
            ++returns;
        }
        // `-> forward_ref _` returns `auto&&`.
        if (word(returns, "forward_ref") && returns + 1 < last) {
            auto reference = type_id{};
            reference.kind = type_kind::reference;
            reference.is_rvalue = true;
            reference.children.push_back(parse(returns + 1, last));
            locate(reference, returns, last);
            result.children.push_back(std::move(reference));
            return result;
        }
        result.children.push_back(parse(returns, last));
        return result;
    }
};

// Whether `text` names a member of `decltype(e)` or `type_of(e)`, such as
// `decltype(x)::value_type`, whose scope may depend on a template parameter,
// including the implicit one of a generic function.
auto is_decltype_scope(std::string_view text) -> bool {
    const auto parsed = parse_expression_text(text);
    const auto& tokens = parsed.tokens;
    if (tokens.size() < 4 || (tokens[0].text != "decltype" && tokens[0].text != "type_of") ||
        tokens[1].text != "(") {
        return false;
    }
    const auto close = detail::bracket_close(tokens, 1, tokens.size());
    return close && *close + 2 < tokens.size() && tokens[*close + 1].text == "::";
}

auto token_mentions(std::string_view text, std::string_view name) -> bool {
    const auto parsed = parse_expression_text(text);
    for (auto index = std::size_t{0}; index < parsed.tokens.size(); ++index) {
        const auto& token = parsed.tokens[index];
        if (token.kind != expression_token_kind::identifier || token.text != name) continue;
        const auto previous = index > 0 ? std::string_view{parsed.tokens[index - 1].text}
                                        : std::string_view{};
        if (previous != "::" && previous != "." && previous != "..") return true;
    }
    return false;
}

}

auto type_id::core() const noexcept -> const type_id& {
    const auto* result = this;
    while ((result->kind == type_kind::pointer || result->kind == type_kind::constant) &&
           !result->children.empty()) {
        result = &result->children.front();
    }
    return *result;
}

auto type_id::core() noexcept -> type_id& {
    return const_cast<type_id&>(static_cast<const type_id&>(*this).core());
}

auto parse_type(std::string_view text, source_span span, type_parse_options options) -> type_id {
    const auto parsed = parse_expression_text(text);
    const auto has_span = span.begin.line != 0 &&
        (span.begin.line != span.end.line || span.begin.column != span.end.column);
    auto parser = type_parser{parsed.tokens, has_span ? std::optional{span} : std::nullopt,
                              options};
    return parser.parse(0, parsed.tokens.size());
}

auto parse_type(const std::vector<expression_token>& tokens, std::size_t first, std::size_t last,
                type_parse_options options) -> type_id {
    auto parser = type_parser{tokens, std::nullopt, options};
    return parser.parse(first, std::min(last, tokens.size()));
}

auto pointer_depth(const type_id& type) noexcept -> int {
    auto result = 0;
    for (const auto* current = &type;
         (current->kind == type_kind::pointer || current->kind == type_kind::constant) &&
         !current->children.empty();
         current = &current->children.front()) {
        if (current->kind == type_kind::pointer) ++result;
    }
    return result;
}

auto mentions_name(const type_id& type, std::string_view name) -> bool {
    switch (type.kind) {
    case type_kind::name:
        for (std::size_t index = 0; index < type.components.size(); ++index) {
            const auto& component = type.components[index];
            if (index == 0 && !type.is_global && component.identifier == name) return true;
            for (const auto& argument : component.arguments) {
                if (mentions_name(argument, name)) return true;
            }
        }
        return false;
    case type_kind::decltype_of:
    case type_kind::expression:
    case type_kind::cpp1:
        return token_mentions(type.text, name);
    default:
        return std::any_of(type.children.begin(), type.children.end(),
            [&](const type_id& child) { return mentions_name(child, name); });
    }
}

auto qualify_name(type_id& type, std::string_view name, const type_id& scope, bool dependent)
    -> void {
    if (type.kind == type_kind::name) {
        for (auto& component : type.components) {
            for (auto& argument : component.arguments) {
                qualify_name(argument, name, scope, dependent);
            }
        }
        if (!type.is_global && type.components.front().identifier == name) {
            if (dependent) {
                for (auto& component : type.components) {
                    component.has_template_keyword = component.has_template_keyword ||
                        component.has_arguments;
                }
            }
            type.components.insert(type.components.begin(), scope.components.begin(),
                                   scope.components.end());
            type.is_global = scope.is_global;
            type.is_dependent = type.is_dependent || dependent;
        }
    }
    for (auto& child : type.children) qualify_name(child, name, scope, dependent);
}

auto mentions_decltype(const type_id& type) -> bool {
    if (type.kind == type_kind::decltype_of) return true;
    if (type.kind == type_kind::cpp1) return is_decltype_scope(type.text);
    for (const auto& component : type.components) {
        if (std::any_of(component.arguments.begin(), component.arguments.end(),
                        [](const type_id& argument) { return mentions_decltype(argument); })) {
            return true;
        }
    }
    return std::any_of(type.children.begin(), type.children.end(),
                       [](const type_id& child) { return mentions_decltype(child); });
}

auto mark_dependent_names(type_id& type, const std::vector<std::string>& names) -> void {
    switch (type.kind) {
    case type_kind::name: {
        const auto is_name = [&](const std::string& identifier) {
            return std::find(names.begin(), names.end(), identifier) != names.end();
        };
        const auto mentions = [&](const type_name_component& component) {
            return std::any_of(component.arguments.begin(), component.arguments.end(),
                [&](const type_id& argument) {
                    return mentions_decltype(argument) ||
                        std::any_of(names.begin(), names.end(),
                            [&](const std::string& name) { return mentions_name(argument, name); });
                });
        };
        // The first component that is a dependent scope, such as `T` in
        // `T::x` or `vector<T>` in `std::vector<T>::size_type`.
        auto scope = type.components.size();
        if (!type.is_global && type.components.size() > 1 &&
            !type.components.front().has_arguments && is_name(type.components.front().identifier)) {
            scope = 0;
        }
        for (std::size_t index = 0; scope == type.components.size() &&
                                    index + 1 < type.components.size(); ++index) {
            if (mentions(type.components[index])) scope = index;
        }
        type.is_dependent = scope != type.components.size();
        for (auto index = scope + 1; index < type.components.size(); ++index) {
            auto& component = type.components[index];
            component.has_template_keyword = component.has_template_keyword ||
                component.has_arguments;
        }
        // An unmarked `T::x` argument stays a value, but a `type`-marked one
        // within it is a type: `std::map<int, type T::y<int>>`.
        for (auto& component : type.components) {
            for (auto& argument : component.arguments) {
                mark_dependent_names(argument, names);
                if (argument.kind == type_kind::name && !argument.has_type_marker) {
                    argument.is_dependent = false;
                }
            }
        }
        break;
    }
    case type_kind::cpp1:
        type.is_dependent = is_decltype_scope(type.text);
        break;
    case type_kind::pointer:
    case type_kind::constant:
    case type_kind::reference:
    case type_kind::pack:
    case type_kind::function:
    case type_kind::union_type:
    case type_kind::intersection_type:
        for (auto& child : type.children) mark_dependent_names(child, names);
        break;
    default:
        break;
    }
}

auto cpp1_fundamental_spelling(std::string_view name) noexcept -> std::string_view {
    const auto found = std::find_if(fundamental_aliases.begin(), fundamental_aliases.end(),
        [name](const auto& alias) { return alias.cpp2 == name; });
    return found == fundamental_aliases.end() ? std::string_view{} : found->cpp1;
}

auto is_fundamental_type_keyword(std::string_view name) noexcept -> bool {
    return std::any_of(fundamental_aliases.begin(), fundamental_aliases.end(),
        [name](const auto& alias) { return alias.cpp2 == name; });
}

auto is_fundamental_type_name(std::string_view name) noexcept -> bool {
    return is_fundamental_type_keyword(name) || is_cpp1_fundamental_word(name) ||
        std::find(single_word_fundamentals.begin(), single_word_fundamentals.end(), name) !=
            single_word_fundamentals.end();
}

auto is_cpp1_fundamental_word(std::string_view word) noexcept -> bool {
    return std::find(multi_word_fundamentals.begin(), multi_word_fundamentals.end(), word) !=
        multi_word_fundamentals.end();
}

auto cpp2_fundamental_suggestion(const std::vector<std::string_view>& words) -> std::string_view {
    const auto count = [&](std::string_view word) {
        return std::count(words.begin(), words.end(), word);
    };
    const auto is_unsigned = count("unsigned") != 0;
    const auto longs = count("long");
    if (count("char") && count("signed")) return "'i8' (usually best) or 'cpp2::_schar'";
    if (count("char") && is_unsigned) return "'u8' (usually best) or 'cpp2::_uchar'";
    if (count("short")) return is_unsigned ? "'ushort'" : "'short'";
    if (count("double") && longs) return "'longdouble'";
    if (longs == 1) return is_unsigned ? "'ulong'" : "'long'";
    if (longs > 1) return is_unsigned ? "'ulonglong'" : "'longlong'";
    if (count("int")) return is_unsigned ? "'uint'" : "'int'";
    return {};
}

}
