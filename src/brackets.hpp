#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Bracket matching over a token sequence whose elements have a `text`.
namespace cppfrontier::detail {

inline auto is_open_bracket(std::string_view text) noexcept -> bool {
    return text == "(" || text == "[" || text == "{";
}

inline auto is_close_bracket(std::string_view text) noexcept -> bool {
    return text == ")" || text == "]" || text == "}";
}

// The number of template lists a `>` or `>>` token closes, else 0.
inline auto angle_closes(std::string_view text) noexcept -> std::size_t {
    return text.find_first_not_of('>') == std::string_view::npos ? text.size() : 0;
}

// Whether `text` is a word that cannot begin an expression, so is not an
// operand after a template argument list.
inline auto is_statement_word(std::string_view text) noexcept -> bool {
    static constexpr auto words = std::array<std::string_view, 26>{
        "return", "co_return", "co_yield", "co_await", "throw", "case", "else", "do", "in",
        "if", "while", "for", "is", "as", "inspect", "break", "continue", "goto",
        "using", "namespace", "type", "const", "static", "pre", "post", "assert"};
    return std::find(words.begin(), words.end(), text) != words.end();
}

// Where the scan of the bracket at `open` ends: at its matching `)`, `]` or
// `}`, at a closer whose bracket is not open, or at `end`. Braces delimit
// blocks: a `}` closes the brackets left open inside its block, and a stray
// `)` or `]` inside a block is skipped. A `)` or `]` likewise closes the
// brackets left open inside its own.
template<class Tokens>
auto bracket_end(const Tokens& tokens, std::size_t open, std::size_t end) -> std::size_t {
    // The closers expected, innermost last.
    auto expected = std::string{};
    for (auto position = open; position < end; ++position) {
        const std::string_view text = tokens[position].text;
        if (text == "(") expected += ')';
        else if (text == "[") expected += ']';
        else if (text == "{") expected += '}';
        else if (is_close_bracket(text)) {
            const auto block = expected.rfind('}');
            const auto match = expected.rfind(text.front());
            if (match != std::string::npos && (text == "}" || block == std::string::npos ||
                                               match > block)) {
                expected.resize(match);
                if (expected.empty()) return position;
            }
            else if (text == "}" || block == std::string::npos) {
                return position;
            }
        }
    }
    return end;
}

// Whether `close` is the closer of the bracket `open`.
inline auto closes(std::string_view open, std::string_view close) noexcept -> bool {
    return (open == "(" && close == ")") || (open == "[" && close == "]") ||
        (open == "{" && close == "}");
}

// The closer in [open, end) matching the bracket at `open`, or nullopt.
template<class Tokens>
auto bracket_close(const Tokens& tokens, std::size_t open, std::size_t end)
    -> std::optional<std::size_t> {
    const auto close = bracket_end(tokens, open, end);
    if (close == end || !closes(tokens[open].text, tokens[close].text)) return std::nullopt;
    return close;
}

// The `(`, `[` or `{` in [begin, close] matching the bracket at `close`, or nullopt.
template<class Tokens>
auto bracket_open(const Tokens& tokens, std::size_t begin, std::size_t close)
    -> std::optional<std::size_t> {
    auto depth = std::size_t{0};
    for (auto position = close + 1; position-- > begin;) {
        const std::string_view text = tokens[position].text;
        if (is_close_bracket(text)) ++depth;
        else if (is_open_bracket(text) && --depth == 0) return position;
    }
    return std::nullopt;
}

// The `>` or `>>` closing the template list whose `<` is at `open`, where each
// `<` outside brackets opens another list; nullopt when a closer or the end
// comes first.
template<class Tokens>
auto angle_close(const Tokens& tokens, std::size_t open, std::size_t end)
    -> std::optional<std::size_t> {
    auto depth = std::size_t{0};
    for (auto position = open; position < end; ++position) {
        const std::string_view text = tokens[position].text;
        if (is_open_bracket(text)) {
            const auto close = bracket_close(tokens, position, end);
            if (!close) return std::nullopt;
            position = *close;
        }
        else if (is_close_bracket(text)) {
            return std::nullopt;
        }
        else if (text == "<") {
            ++depth;
        }
        else if (const auto count = angle_closes(text); count != 0) {
            if (count >= depth) return position;
            depth -= count;
        }
    }
    return std::nullopt;
}

// The `>` closing the template argument list that the `<` at `open` begins,
// when the tokens read as one: none is `;`, `{` or `}`; outside brackets none
// is an operator that a template argument cannot contain, `&&` only as a Cpp1
// reference declarator before `,` or `>`, and `=` only in a typed value
// `:T = v`; and no operand follows the `>`, as in the comparisons `a < b > c`.
// `text(i)` spells the token at `i`, empty past the end, and `operand(i)` says
// whether it is an operand.
template<class Text, class Operand>
auto template_argument_close(Text text, Operand operand, std::size_t open, std::size_t end)
    -> std::optional<std::size_t> {
    static constexpr auto rejected = std::array<std::string_view, 17>{
        "||", "?", "<=", "<=>", ">=", "<<", "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=",
        "^=", "<<=", ">>="};
    auto angles = std::size_t{0};
    auto groups = std::size_t{0};
    auto typed_value = false;
    for (auto index = open; index < end; ++index) {
        const std::string_view value = text(index);
        if (value == ";" || value == "{" || value == "}") return std::nullopt;
        if (value == "(" || value == "[") ++groups;
        else if (value == ")" || value == "]") {
            if (groups-- == 0) return std::nullopt;
        }
        else if (groups != 0) continue;
        else if (value == ":") typed_value = true;
        else if (value == ",") typed_value = false;
        else if (value == "<") ++angles;
        else if (const auto count = angle_closes(value); count != 0) {
            if (count < angles) angles -= count;
            else return operand(index + 1) ? std::nullopt : std::optional{index};
        }
        else if (value == "&&") {
            const std::string_view next = text(index + 1);
            if (next != "," && angle_closes(next) == 0) return std::nullopt;
        }
        else if ((value != "=" || !typed_value) &&
                 std::find(rejected.begin(), rejected.end(), value) != rejected.end()) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

// `template_argument_close` for every `<` in [0, end) in one pass, indexed by
// position. The scans of the `<`s in one bracket group read the same tokens,
// so each group keeps its live scans on a stack, newest last: a newer scan has
// read fewer `<`, so a `>` ends the newest scans first, and an `=` outside a
// typed value `:T = v` ends those begun after the group's last `:`.
template<class Text, class Operand>
auto template_argument_closes(Text text, Operand operand, std::size_t end)
    -> std::vector<std::optional<std::size_t>> {
    static constexpr auto rejected = std::array<std::string_view, 16>{
        "||", "?", "<=", "<=>", ">=", "<<", "+=", "-=", "*=", "/=", "%=", "&=", "|=",
        "^=", "<<=", ">>="};
    struct scan final {
        std::size_t open;
        // The group's `angles` before this scan's `<`.
        std::size_t base;
    };
    struct group final {
        std::vector<scan> scans;
        // The `<` read less the lists closed; a scan's depth is `angles - base`.
        std::size_t angles = 0;
        std::optional<std::size_t> colon;
        std::optional<std::size_t> comma;
    };
    auto result = std::vector<std::optional<std::size_t>>(end);
    auto groups = std::vector<group>(1);
    for (auto index = std::size_t{0}; index < end; ++index) {
        const std::string_view value = text(index);
        auto& current = groups.back();
        if (value == ";" || value == "{" || value == "}") {
            for (auto& each : groups) each.scans.clear();
        }
        else if (value == "(" || value == "[") groups.emplace_back();
        else if (value == ")" || value == "]") {
            if (groups.size() > 1) groups.pop_back();
            else current.scans.clear();
        }
        else if (value == ":") current.colon = index;
        else if (value == ",") current.comma = index;
        else if (value == "<") {
            current.scans.push_back({index, current.angles});
            ++current.angles;
        }
        else if (const auto count = angle_closes(value); count != 0) {
            while (!current.scans.empty() && current.angles - current.scans.back().base <= count) {
                if (!operand(index + 1)) result[current.scans.back().open] = index;
                current.scans.pop_back();
            }
            current.angles = current.scans.empty() ? 0 : current.angles - count;
        }
        else if (value == "&&") {
            const std::string_view next = text(index + 1);
            if (next != "," && angle_closes(next) == 0) current.scans.clear();
        }
        else if (value == "=") {
            // A scan is in a typed value when it began before a `:` that no `,` follows.
            const auto typed = current.colon && (!current.comma || *current.comma < *current.colon);
            while (!current.scans.empty() &&
                   (!typed || current.scans.back().open > *current.colon)) {
                current.scans.pop_back();
            }
        }
        else if (std::find(rejected.begin(), rejected.end(), value) != rejected.end()) {
            current.scans.clear();
        }
    }
    return result;
}

// Ignores a bracket left open.
struct ignore_unclosed final {
    auto operator()(std::size_t) const noexcept -> void {}
};

// The first position in [begin, end) outside brackets and template lists at
// which `stop(position)` holds or a closer leaves the range, else `end`.
// `list_close(position)` is the close of the template list a `<` begins, if
// any, which may be at `end`. A bracket whose closer is missing is passed to
// `unclosed`, and the scan goes on at the closer of another kind that ended it.
template<class Tokens, class ListClose, class Stop, class Unclosed = ignore_unclosed>
auto find_top_level(const Tokens& tokens, std::size_t begin, std::size_t end,
                    ListClose list_close, Stop stop, Unclosed unclosed = {}) -> std::size_t {
    for (auto position = begin; position < end; ++position) {
        if (stop(position)) return position;
        const std::string_view text = tokens[position].text;
        if (is_close_bracket(text)) return position;
        if (is_open_bracket(text)) {
            const auto close = bracket_end(tokens, position, end);
            if (close == end || !closes(text, tokens[close].text)) {
                unclosed(position);
                position = close - 1;
                continue;
            }
            position = close;
        }
        else if (text == "<") {
            // A `>>` at `end` may close this list and an enclosing one.
            if (const auto close = list_close(position); close && *close <= end) position = *close;
        }
    }
    return end;
}

// The `)` of the name list `(a, b)` at `begin` when `:` or `:=` follows it:
// the start of a destructuring declaration (experiment `destructuring`).
// `is_name(token)` holds for each name, a declaration name or `_`.
template<class Tokens, class IsName>
auto destructuring_names_end(const Tokens& tokens, std::size_t begin, IsName is_name)
    -> std::optional<std::size_t> {
    if (begin >= tokens.size() || std::string_view{tokens[begin].text} != "(") return std::nullopt;
    for (auto position = begin + 1; position + 1 < tokens.size() && is_name(tokens[position]);
         position += 2) {
        const std::string_view next = tokens[position + 1].text;
        if (next == ")") {
            const auto follows = position + 2 < tokens.size()
                ? std::string_view{tokens[position + 2].text} : std::string_view{};
            return follows == ":" || follows == ":=" ? std::optional{position + 1} : std::nullopt;
        }
        if (next != ",") break;
    }
    return std::nullopt;
}

}
