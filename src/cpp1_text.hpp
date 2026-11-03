#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cppfrontier::detail {

inline auto is_identifier_continue(char value) -> bool {
    return std::isalnum(static_cast<unsigned char>(value)) || value == '_';
}

// Whether `text` contains `word` as a whole identifier.
inline auto names_word(std::string_view text, std::string_view word) -> bool {
    for (auto found = text.find(word); found != std::string_view::npos;
         found = text.find(word, found + 1)) {
        const auto end = found + word.size();
        if ((found == 0 || !is_identifier_continue(text[found - 1])) &&
            (end == text.size() || !is_identifier_continue(text[end]))) {
            return true;
        }
    }
    return false;
}

// End of the pp-number starting at `position`. `..` is excluded because it
// begins a Cpp2 range operator, and `.` before an identifier because `0.f()`
// is a UFCS call.
inline auto pp_number_end(std::string_view text, std::size_t position) -> std::size_t {
    ++position;
    while (position < text.size()) {
        const auto value = text[position];
        const auto next = position + 1 < text.size() ? text[position + 1] : '\0';
        if ((value == 'e' || value == 'E' || value == 'p' || value == 'P') &&
            (next == '+' || next == '-')) {
            position += 2;
        }
        else if (is_identifier_continue(value)) {
            ++position;
        }
        else if (value == '\'' && is_identifier_continue(next)) {
            position += 2;
        }
        else if (value == '.' && next != '.' && next != '_' &&
                 !std::isalpha(static_cast<unsigned char>(next))) {
            ++position;
        }
        else {
            break;
        }
    }
    return position;
}

// Length of a string or character encoding prefix (`u8`, `u`, `U`, `L`) at
// `position`, or 0 when no literal follows.
inline auto encoding_prefix_size(std::string_view text, std::size_t position) -> std::size_t {
    if (position >= text.size() || (position > 0 && is_identifier_continue(text[position - 1]))) {
        return 0;
    }
    auto size = std::size_t{0};
    if (text.substr(position, 2) == "u8") size = 2;
    else if (text[position] == 'u' || text[position] == 'U' || text[position] == 'L') size = 1;
    else return 0;
    const auto rest = text.substr(position + size);
    return rest.starts_with('"') || rest.starts_with('\'') || rest.starts_with("R\"") ? size : 0;
}

// The `)d"` that ends the raw string whose `R` is at `position`, or empty when
// no `(` follows its delimiter on the line.
inline auto raw_string_terminator(std::string_view text, std::size_t position) -> std::string {
    const auto open = text.find_first_of("(\n", position + 2);
    if (open == std::string_view::npos || text[open] != '(') return {};
    auto result = std::string{")"};
    result += text.substr(position + 2, open - position - 2);
    result += '"';
    return result;
}

enum class lexeme_kind : std::uint8_t {
    none, line_comment, block_comment, string, raw_string, character, number,
};

struct lexeme final {
    lexeme_kind kind = lexeme_kind::none;
    // One past the lexeme; an unterminated literal or comment ends at the end
    // of `text`, or for a string or character literal, of its line.
    std::size_t end = 0;
    bool terminated = true;
};

// The comment, literal or pp-number ([lex.ppnumber]) that begins at `position`
// of `text`. A literal may have an encoding prefix, a raw string also `$` for
// interpolation. A pp-number begins with a digit that does not continue an
// identifier.
inline auto lexeme_at(std::string_view text, std::size_t position) -> lexeme {
    const auto rest = text.substr(position);
    if (rest.starts_with("//")) {
        return {lexeme_kind::line_comment, std::min(text.find('\n', position), text.size())};
    }
    if (rest.starts_with("/*")) {
        const auto end = text.find("*/", position + 2);
        if (end == std::string_view::npos) return {lexeme_kind::block_comment, text.size(), false};
        return {lexeme_kind::block_comment, end + 2};
    }
    const auto follows_word = position > 0 && is_identifier_continue(text[position - 1]);
    if (rest.empty()) return {};
    if (std::isdigit(static_cast<unsigned char>(rest.front()))) {
        return follows_word ? lexeme{} : lexeme{lexeme_kind::number, pp_number_end(text, position)};
    }
    const auto dollar = rest.starts_with('$');
    const auto literal = position + dollar + encoding_prefix_size(text, position + dollar);
    if (text.substr(literal, 2) == "R\"" && (literal != position || !follows_word)) {
        const auto terminator = raw_string_terminator(text, literal);
        const auto end = terminator.empty() ? std::string_view::npos : text.find(terminator, literal);
        if (end == std::string_view::npos) return {lexeme_kind::raw_string, text.size(), false};
        return {lexeme_kind::raw_string, end + terminator.size()};
    }
    if (dollar || literal >= text.size()) return {};
    const auto quote = text[literal];
    if (quote != '"' && quote != '\'') return {};
    const auto kind = quote == '"' ? lexeme_kind::string : lexeme_kind::character;
    for (auto at = literal + 1; at < text.size() && text[at] != '\n'; ++at) {
        if (text[at] == '\\') ++at;
        else if (text[at] == quote) return {kind, at + 1};
    }
    return {kind, std::min(text.find('\n', literal), text.size()), false};
}

// Scans source lines for code outside comments, carrying a block comment or
// raw string from one line to the next.
struct line_scanner final {
    bool in_block_comment = false;
    // The `)d"` that ends the raw string the scan is in, or empty.
    std::string raw_terminator;

    // Calls `code(begin, end, kind)` for each literal and pp-number of `line`,
    // and for each other character that is not whitespace, with kind none.
    template<class Code>
    auto scan(std::string_view line, Code code) -> void {
        auto position = std::size_t{0};
        const auto awaited = in_block_comment ? std::string{"*/"} : raw_terminator;
        if (!awaited.empty()) {
            const auto end = line.find(awaited);
            if (end == std::string_view::npos) return;
            position = end + awaited.size();
            in_block_comment = false;
            raw_terminator.clear();
        }
        while (position < line.size()) {
            if (std::isspace(static_cast<unsigned char>(line[position]))) {
                ++position;
                continue;
            }
            const auto found = lexeme_at(line, position);
            if (found.kind == lexeme_kind::line_comment) return;
            if (found.kind == lexeme_kind::block_comment) {
                in_block_comment = !found.terminated;
                position = found.end;
                continue;
            }
            if (found.kind == lexeme_kind::raw_string && !found.terminated) {
                raw_terminator = raw_string_terminator(line, line.find("R\"", position));
            }
            const auto end = found.kind == lexeme_kind::none ? position + 1 : found.end;
            code(position, end, found.kind);
            position = end;
        }
    }
};

// The interpolations `(expr)$` of the string literal token `literal`: an
// ordinary string or an interpolating raw string `$R"d(...)d"`.
struct string_interpolations final {
    // Offsets of the `(` and `)` of each interpolation, in order.
    std::vector<std::pair<std::size_t, std::size_t>> parts;
    // Offset of the first `)$` that closes no interpolation, or npos.
    std::size_t unmatched = std::string_view::npos;
};

// A `)` matching the `(` at `open` in [open, end) of `text`, skipping escaped
// characters (when not `raw`) and character and string literals, or npos.
inline auto interpolation_close(std::string_view text, std::size_t open, std::size_t end, bool raw)
    -> std::size_t {
    // The end of the literal whose opening quote ends at `begin`, or npos.
    const auto skip_literal = [&](std::size_t begin, char quote, bool escaped_quote) {
        for (auto position = begin; position < end; ++position) {
            if (escaped_quote && text[position] == '\\' && position + 1 < end &&
                text[position + 1] == quote) {
                return position + 2;
            }
            if (text[position] == '\\') ++position;
            else if (!escaped_quote && text[position] == quote) return position + 1;
        }
        return std::string_view::npos;
    };
    auto depth = std::size_t{0};
    for (auto position = open; position < end;) {
        const auto value = text[position];
        auto next = position + 1;
        if (!raw && value == '\\') {
            next = position + 2;
            if (position + 1 < end && text[position + 1] == '"') {
                next = skip_literal(position + 2, '"', true);
            }
        }
        else if (raw && value == '"') {
            next = skip_literal(position + 1, '"', false);
        }
        else if (value == '\'') {
            auto word = position;
            while (word > open && is_identifier_continue(text[word - 1])) --word;
            // Not a digit separator, as in `1'000`.
            if (word == position || encoding_prefix_size(text, word) == position - word) {
                const auto literal_end = skip_literal(position + 1, '\'', false);
                if (literal_end != std::string_view::npos) next = literal_end;
            }
        }
        else if (value == '(') {
            ++depth;
        }
        else if (value == ')' && --depth == 0) {
            return position;
        }
        if (next == std::string_view::npos) return next;
        position = next;
    }
    return std::string_view::npos;
}

inline auto scan_interpolations(std::string_view literal) -> string_interpolations {
    auto result = string_interpolations{};
    const auto raw = literal.starts_with("$R\"");
    auto begin = std::size_t{0};
    auto end = std::size_t{0};
    if (raw) {
        const auto open = literal.find('(');
        if (open == std::string_view::npos || literal.size() < 2 * open) return result;
        begin = open + 1;
        end = literal.size() - (open - 3) - 2;
    }
    else {
        const auto quote = literal.find('"');
        if (quote == std::string_view::npos || (quote > 0 && literal[quote - 1] == 'R') ||
            literal.size() < quote + 2) {
            return result;
        }
        begin = quote + 1;
        end = literal.size() - 1;
    }
    for (auto position = begin; position < end;) {
        const auto value = literal[position];
        if (!raw && value == '\\') {
            position += 2;
            continue;
        }
        if (value == '(') {
            const auto close = interpolation_close(literal, position, end, raw);
            if (close != std::string_view::npos && close + 1 < end && literal[close + 1] == '$') {
                result.parts.emplace_back(position, close);
                position = close + 2;
                continue;
            }
        }
        else if (value == ')' && position + 1 < end && literal[position + 1] == '$' &&
                 result.unmatched == std::string_view::npos) {
            result.unmatched = position;
        }
        ++position;
    }
    return result;
}

}
