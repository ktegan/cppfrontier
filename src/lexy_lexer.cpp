#include <cppfrontier/pipeline.hpp>

#include "cpp1_text.hpp"

#include <lexy/action/scan.hpp>
#include <lexy/dsl.hpp>
#include <lexy/input/string_input.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace cppfrontier {
namespace {

namespace dsl = lexy::dsl;

using lexy_input = lexy::string_input<>;
using lexy_lexeme = lexy::lexeme_for<lexy_input>;

constexpr auto identifier_token = dsl::token(
    dsl::identifier(dsl::ascii::alpha_underscore, dsl::ascii::alpha_digit_underscore));
constexpr auto single_punctuation_token = dsl::token(
    LEXY_LIT("-") | LEXY_LIT("?") | LEXY_LIT("+") | LEXY_LIT("*") | LEXY_LIT("/") |
    LEXY_LIT("%") | LEXY_LIT("!") | LEXY_LIT("&") | LEXY_LIT("|") | LEXY_LIT("^") |
    LEXY_LIT("~") | LEXY_LIT(".") | LEXY_LIT(",") | LEXY_LIT("$"));
constexpr auto punctuation_token = dsl::token(
    LEXY_LIT("<<=") | LEXY_LIT(">>=") | LEXY_LIT("<=>") | LEXY_LIT("...") | LEXY_LIT(":=") | LEXY_LIT("->") | LEXY_LIT("++") |
    LEXY_LIT("--") | LEXY_LIT("<<") | LEXY_LIT(">>") | LEXY_LIT("<=") | LEXY_LIT(">=") |
    LEXY_LIT("==") | LEXY_LIT("!=") | LEXY_LIT("&&") | LEXY_LIT("||") | LEXY_LIT("+=") |
    LEXY_LIT("-=") | LEXY_LIT("*=") | LEXY_LIT("/=") | LEXY_LIT("%=") | LEXY_LIT("&=") |
    LEXY_LIT("|=") | LEXY_LIT("^=") | LEXY_LIT("::") | LEXY_LIT(":") | LEXY_LIT("@") |
    LEXY_LIT("(") | LEXY_LIT(")") | LEXY_LIT("[") | LEXY_LIT("]") | LEXY_LIT("=") |
    LEXY_LIT(";") | LEXY_LIT("{") | LEXY_LIT("}") | LEXY_LIT("<") | LEXY_LIT(">"));
constexpr auto identifier_suffix_token = dsl::token(
    LEXY_LIT("++") | LEXY_LIT("--") | LEXY_LIT("*=") | LEXY_LIT("*"));
constexpr auto pointer_dereference_token = dsl::token(
    dsl::identifier(dsl::ascii::alpha_underscore, dsl::ascii::alpha_digit_underscore) >>
    LEXY_LIT("*"));
constexpr std::array keyword_text{
    "type", "using", "namespace", "public", "protected", "private", "move", "forward", "if", "else", "return",
    "while", "for", "in", "do", "next", "this", "that", "true", "false", "nullptr", "requires",
    "break", "continue", "try", "catch", "throw", "static", "concept",
};

auto classify(std::string_view text) -> token_kind {
    if (text.empty()) {
        return token_kind::end_of_file;
    }
    if (std::find(keyword_text.begin(), keyword_text.end(), text) != keyword_text.end()) {
        return token_kind::keyword;
    }
    auto unprefixed = text;
    if (unprefixed.starts_with('$')) unprefixed.remove_prefix(1);
    if (unprefixed.starts_with("u8")) unprefixed.remove_prefix(2);
    else if (unprefixed.starts_with('u') || unprefixed.starts_with('U') ||
             unprefixed.starts_with('L')) unprefixed.remove_prefix(1);
    if (unprefixed.starts_with('"') || unprefixed.starts_with("R\"")) {
        return token_kind::string_literal;
    }
    if (unprefixed.starts_with('\'')) {
        return token_kind::character_literal;
    }
    if (text.front() >= '0' && text.front() <= '9') {
        return token_kind::integer_literal;
    }
    if (text.size() >= 2 && text.back() == '*' && text.front() != '*' &&
        std::isalpha(static_cast<unsigned char>(text.front()))) {
        return token_kind::pointer_dereference;
    }
    if (text.front() == '*') {
        return token_kind::pointer_suffix;
    }
    switch (text.front()) {
    case ':': case '@': case '(': case ')': case '[': case ']': case '=': case ';': case '{': case '}':
    case '-': case '?': case '+': case '*': case '/': case '%': case '<': case '>':
    case '!': case '&': case '|': case '^': case '~': case '.': case ',': case '$':
        return token_kind::punctuation;
    default:
        return token_kind::identifier;
    }
}

constexpr std::array escaped_cpp1_names{
    "and", "and_eq", "bitand", "bitor", "compl", "not", "not_eq", "or", "or_eq",
    "xor", "xor_eq", "new", "class", "struct", "enum", "union", "default",
};

constexpr std::array cpp2_contextual_names{
    "type", "namespace", "using", "public", "protected", "private", "move", "forward",
    "copy", "in", "inout", "out", "in_ref", "forward_ref", "if", "else", "return",
    "while", "for", "do", "next", "requires", "true", "false", "nullptr", "this",
    "that", "break", "continue", "static",
};

constexpr std::array cpp1_reserved_names{
    "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool",
    "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t", "class",
    "compl", "concept", "const", "consteval", "constexpr", "constinit", "const_cast",
    "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete",
    "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern",
    "false", "float", "for", "friend", "goto", "if", "inline", "int", "long",
    "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr", "operator",
    "or", "or_eq", "private", "protected", "public", "register", "reinterpret_cast",
    "requires", "return", "short", "signed", "sizeof", "static", "static_assert",
    "static_cast", "struct", "switch", "template", "this", "thread_local", "throw",
    "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using",
    "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq",
};

auto position_after(const std::vector<std::size_t>& line_starts, std::size_t offset)
    -> source_position {
    const auto next = std::upper_bound(line_starts.begin(), line_starts.end(), offset);
    const auto line = static_cast<std::uint32_t>(next - line_starts.begin());
    return {line, static_cast<std::uint32_t>(offset - *(next - 1) + 1)};
}

auto make_token(std::string_view text, std::size_t offset,
                const std::vector<std::size_t>& line_starts) -> token {
    token result;
    result.kind = classify(text);
    result.text.assign(text.begin(), text.end());
    if (text.starts_with("__identifier__") && text.size() > 14) {
        result.text.erase(0, 14);
        result.escaped_identifier = true;
    }
    result.span.begin = position_after(line_starts, offset);
    result.span.end = position_after(line_starts, offset + text.size());
    return result;
}

auto is_adjacent(const token& left, const token& right) noexcept -> bool {
    return left.span.end.line == right.span.begin.line &&
        left.span.end.column == right.span.begin.column;
}

// Appends the operator after `operator` to `name`: a symbol (`operator<=>`),
// `()`, `[]`, or a literal suffix (`operator""sv`). Returns the tokens consumed.
auto merge_operator_name(const token_list& tokens, std::size_t index, token& name)
    -> std::size_t {
    static constexpr auto symbols = std::array<std::string_view, 36>{
        "=", "==", "!=", "<", ">", "<=", ">=", "<=>", "+", "-", "*", "/", "%", "^", "&",
        "|", "~", "!", "<<", ">>", "&&", "||", "++", "--", "+=", "-=", "*=", "/=", "%=",
        "^=", "&=", "|=", "<<=", ">>=", "->", "->*"};
    const auto at = [&](std::size_t offset) -> const token* {
        return index + offset < tokens.size() ? &tokens[index + offset] : nullptr;
    };
    const auto* first = at(1);
    if (first == nullptr) return 0;
    auto consumed = std::size_t{0};
    const auto* second = at(2);
    if ((first->text == "(" || first->text == "[") && second != nullptr &&
        second->text == (first->text == "(" ? ")" : "]") && is_adjacent(*first, *second)) {
        consumed = 2;
    }
    else if (first->text == "->" && second != nullptr && second->text == "*" &&
             is_adjacent(*first, *second)) {
        consumed = 2;
    }
    else if (first->text == "\"\"" && second != nullptr &&
             second->kind == token_kind::identifier && is_adjacent(*first, *second)) {
        consumed = 2;
    }
    else if (std::find(symbols.begin(), symbols.end(), first->text) != symbols.end()) {
        consumed = 1;
    }
    for (auto offset = std::size_t{1}; offset <= consumed; ++offset) {
        name.text += at(offset)->text;
        name.span.end = at(offset)->span.end;
    }
    return consumed;
}

auto merge_symbol_tokens(token_list& tokens) {
    auto output = token_list{};
    output.reserve(tokens.size());
    for (auto index = std::size_t{0}; index < tokens.size(); ++index) {
        auto merged = tokens[index];
        // `is` and `as` are operators, as Cpp2 keywords, so `is ::N::C` is not one name.
        const auto is_operator_word = merged.text == "is" || merged.text == "as";
        while (index + 2 < tokens.size() && merged.kind == token_kind::identifier &&
               !is_operator_word &&
               tokens[index + 1].text == "::" &&
               tokens[index + 2].kind == token_kind::identifier) {
            merged.text += "::" + tokens[index + 2].text;
            merged.span.end = tokens[index + 2].span.end;
            index += 2;
        }
        if (merged.text == "operator" || merged.text.ends_with("::operator")) {
            index += merge_operator_name(tokens, index, merged);
        }
        while (index + 1 < tokens.size() && merged.kind == token_kind::punctuation &&
               (merged.text == "<" || merged.text == ">") &&
               tokens[index + 1].kind == token_kind::punctuation &&
               tokens[index + 1].text == merged.text) {
            merged.text += tokens[index + 1].text;
            merged.span.end = tokens[index + 1].span.end;
            ++index;
        }
        output.push_back(std::move(merged));
    }
    tokens = std::move(output);
}

// The position of the token closing the outermost template list of the type
// that begins after the `:` or `->` at `introducer`, when the type has one:
// past `*`, `const` and words such as `type` or `forward`, a name and `<`,
// and in it every `<` opens a list. A `::name<` after a close continues the type.
auto type_list_close(const token_list& tokens, std::size_t introducer)
    -> std::optional<std::size_t> {
    auto at = introducer + 1;
    while (at < tokens.size() && (tokens[at].text == "*" || tokens[at].text == "const" ||
                                  tokens[at].text == "::" ||
                                  tokens[at].kind == token_kind::identifier ||
                                  tokens[at].text == "type" || tokens[at].text == "forward" ||
                                  tokens[at].text == "move")) {
        ++at;
    }
    if (at == introducer + 1 || at >= tokens.size() || tokens[at].text != "<") return std::nullopt;
    auto angles = std::size_t{0};
    auto groups = std::size_t{0};
    for (; at < tokens.size(); ++at) {
        const std::string_view text = tokens[at].text;
        if (text == ";" || text == "{" || text == "}") return std::nullopt;
        if (text == "(" || text == "[") ++groups;
        else if (text == ")" || text == "]") {
            if (groups-- == 0) return std::nullopt;
        }
        else if (groups != 0) continue;
        else if (text == "<") ++angles;
        else if (text.starts_with('>') && text.find_first_not_of('>') >= text.size() - 1) {
            const auto closes = text.find_first_not_of('>') == std::string_view::npos
                ? text.size() : text.size() - 1;
            if (closes < angles) {
                angles -= closes;
                continue;
            }
            if (closes > angles) return std::nullopt;
            if (at + 3 < tokens.size() && closes == text.size() && tokens[at + 1].text == "::" &&
                tokens[at + 2].kind == token_kind::identifier && tokens[at + 3].text == "<") {
                angles = 0;
                at += 2;
                continue;
            }
            return at;
        }
    }
    return std::nullopt;
}

// A type begins after `:` or `->`, and in it every `<` opens a template list,
// so a `>=`, `>>=` or `>==` closing the type's outermost list is that close
// followed by `=` or `==`.
auto split_type_closes(token_list& tokens) -> void {
    auto split = std::vector<bool>(tokens.size());
    auto any = false;
    for (auto at = std::size_t{0}; at < tokens.size(); ++at) {
        if (tokens[at].text != ":" && tokens[at].text != "->") continue;
        if (const auto close = type_list_close(tokens, at); close && tokens[*close].text.ends_with('=')) {
            split[*close] = true;
            any = true;
        }
    }
    if (!any) return;
    auto output = token_list{};
    output.reserve(tokens.size() + tokens.size() / 8);
    for (auto at = std::size_t{0}; at < tokens.size(); ++at) {
        if (!split[at]) {
            output.push_back(std::move(tokens[at]));
            continue;
        }
        auto assign = tokens[at];
        assign.text = "=";
        assign.span.begin = {assign.span.end.line, assign.span.end.column - 1};
        tokens[at].text.pop_back();
        tokens[at].span.end = assign.span.begin;
        output.push_back(std::move(tokens[at]));
        // `>==` lexes as `>=` `=`.
        if (at + 1 < tokens.size() && tokens[at + 1].text == "=" &&
            is_adjacent(assign, tokens[at + 1])) {
            assign.text = "==";
            assign.span.end = tokens[at + 1].span.end;
            ++at;
        }
        output.push_back(std::move(assign));
    }
    tokens = std::move(output);
}

// Whether the character literal `literal` holds one character or escape sequence.
auto holds_one_character(std::string_view literal) -> bool {
    literal.remove_prefix(detail::encoding_prefix_size(literal, 0) + 1);
    literal.remove_suffix(1);
    if (literal.empty()) return false;
    const auto is_hex = [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; };
    auto length = std::size_t{1};
    if (literal.front() != '\\') {
        while (length < literal.size() &&
               (static_cast<unsigned char>(literal[length]) & 0xC0) == 0x80) {
            ++length;
        }
        return length == literal.size();
    }
    if (literal.size() < 2) return false;
    const auto kind = literal[1];
    length = 2;
    if (literal.size() > 2 && literal[2] == '{' &&
        (kind == 'x' || kind == 'o' || kind == 'u' || kind == 'N')) {
        return literal.back() == '}' && literal.find('}') == literal.size() - 1;
    }
    if (kind >= '0' && kind <= '7') {
        while (length < literal.size() && length < 4 && literal[length] >= '0' &&
               literal[length] <= '7') {
            ++length;
        }
    }
    else if (kind == 'x') {
        while (length < literal.size() && is_hex(literal[length])) ++length;
    }
    else if (kind == 'u' || kind == 'U') {
        length += kind == 'u' ? 4 : 8;
    }
    return length == literal.size();
}

// What is wrong with the character literal `literal`, or empty. An unprefixed
// or `u8` literal holds one UTF-8 code unit, and a `u` literal one UTF-16
// code unit.
auto character_literal_problem(std::string_view literal) -> std::string {
    if (!holds_one_character(literal)) {
        return "character literal " + std::string{literal} + " must hold exactly one "
            "character or escape sequence - use a string literal for text";
    }
    const auto prefix = detail::encoding_prefix_size(literal, 0);
    if ((prefix == 0 || literal.starts_with("u8")) &&
        static_cast<unsigned char>(literal[prefix + 1]) >= 0x80) {
        return "character literal " + std::string{literal} + " holds more than one UTF-8 "
            "code unit - write U" + std::string{literal.substr(prefix)} + " for one code "
            "point, or a string literal for text";
    }
    // A UTF-8 lead byte from 0xf0 begins a code point beyond the 16-bit range.
    if (literal.starts_with("u'") && static_cast<unsigned char>(literal[2]) >= 0xf0) {
        return "character literal " + std::string{literal} + " holds more than one UTF-16 "
            "code unit - write U" + std::string{literal.substr(1)} + " for one code point, "
            "or a string literal for text";
    }
    return {};
}

auto unlexable_message(std::string_view text) -> std::string {
    auto literal = text;
    if (literal.starts_with('$')) literal.remove_prefix(1);
    if (!literal.empty()) literal.remove_prefix(detail::encoding_prefix_size(literal, 0));
    if (text.starts_with("/*")) return "block comment is missing its closing */";
    if (literal.starts_with("R\"")) return "raw string literal is missing its closing )\"";
    if (literal.starts_with('"')) return "string literal is missing its closing \"";
    if (literal.starts_with('\'')) return "character literal is missing its closing '";
    const auto byte = static_cast<unsigned char>(text.front());
    if (byte < 0x20 || byte == 0x7f) {
        constexpr auto digits = std::string_view{"0123456789abcdef"};
        return std::string{"unexpected control character 0x"} + digits[byte >> 4] + digits[byte & 15];
    }
    return "unexpected text '" + std::string{text} + "'";
}

}

auto is_contextual_name(const token& value) noexcept -> bool {
    if (value.kind == token_kind::identifier) return true;
    if (value.kind != token_kind::keyword) return false;
    return std::find(cpp2_contextual_names.begin(), cpp2_contextual_names.end(), value.text) !=
        cpp2_contextual_names.end();
}

auto is_cpp2_name(const token& value) noexcept -> bool {
    return value.kind == token_kind::identifier || value.escaped_identifier;
}

auto is_declaration_name(const token& value) noexcept -> bool {
    if (!value.escaped_identifier && value.text.starts_with("operator") &&
        value.text.size() > 8 && value.text[8] != '_' &&
        std::isalnum(static_cast<unsigned char>(value.text[8])) == 0) {
        return true;
    }
    if (!is_contextual_name(value) && !value.escaped_identifier) return false;
    if (!value.escaped_identifier && (value.text == "this" || value.text == "that")) {
        return false;
    }
    return !is_cpp1_reserved_name(value.text) || is_escapable_cpp1_name(value.text);
}

auto is_type_name(const token& value) noexcept -> bool {
    return is_cpp2_name(value) || is_declaration_name(value);
}

auto is_cpp1_reserved_name(std::string_view value) noexcept -> bool {
    return std::find(cpp1_reserved_names.begin(), cpp1_reserved_names.end(), value) !=
        cpp1_reserved_names.end();
}

auto is_escapable_cpp1_name(std::string_view value) noexcept -> bool {
    return std::find(escaped_cpp1_names.begin(), escaped_cpp1_names.end(), value) !=
        escaped_cpp1_names.end();
}

auto cpp1_identifier(std::string_view value) -> std::string {
    if (std::find(escaped_cpp1_names.begin(), escaped_cpp1_names.end(), value) ==
        escaped_cpp1_names.end()) {
        return std::string{value};
    }
    return "cpp2_" + std::string{value};
}

auto anonymous_object_name(source_position position) -> std::string {
    return "auto_" + std::to_string(position.line) + "_" + std::to_string(position.column);
}

auto tokenize(const source_text& source, std::uint32_t begin_line,
              std::uint32_t end_line_exclusive, std::vector<diagnostic>* diagnostics)
    -> token_list {
    token_list tokens;
    if (begin_line == 0 || end_line_exclusive < begin_line) {
        return tokens;
    }

    const auto& data = source.data();
    const auto& line_starts = source.line_starts();
    // The offset of the start of `line`, or the end of the text after the last line.
    const auto start_of = [&](std::uint32_t line) {
        return line <= line_starts.size() ? line_starts[line - 1] : data.size();
    };
    const auto offset = start_of(begin_line);
    // The section ends before the newline that ends its last line.
    const auto end_offset = end_line_exclusive > begin_line && end_line_exclusive <= line_starts.size()
        ? line_starts[end_line_exclusive - 1] - 1
        : end_line_exclusive > begin_line ? data.size() : offset;

    const auto length = end_offset > offset ? end_offset - offset : std::size_t{0};
    const auto text = std::string_view{data}.substr(offset, length);
    auto position = std::size_t{0};
    // Reports the text [position, end) as no token.
    const auto report = [&](std::size_t end) {
        if (diagnostics == nullptr) return;
        diagnostics->push_back({"lexer", diagnostic_severity::error,
            {position_after(line_starts, offset + position), position_after(line_starts, offset + end)},
            unlexable_message(text.substr(position, end - position))});
    };
    const auto push = [&](std::size_t end) {
        tokens.push_back(make_token(text.substr(position, end - position), offset + position,
                                    line_starts));
        if (diagnostics == nullptr) return;
        if (tokens.back().kind == token_kind::character_literal) {
            if (auto problem = character_literal_problem(tokens.back().text); !problem.empty()) {
                diagnostics->push_back({"lexer", diagnostic_severity::error, tokens.back().span,
                                        std::move(problem)});
            }
            return;
        }
        if (tokens.back().kind != token_kind::string_literal) return;
        // An interpolation is a std::string, which a prefixed literal cannot join.
        const auto literal = std::string_view{tokens.back().text};
        const auto dollar = literal.starts_with('$');
        const auto prefix = detail::encoding_prefix_size(literal.substr(dollar), 0);
        if (prefix != 0 && (dollar || !detail::scan_interpolations(literal).parts.empty())) {
            diagnostics->push_back({"lexer", diagnostic_severity::error, tokens.back().span,
                "a string interpolation '(...)$' is not allowed in a '" +
                std::string{literal.substr(dollar, prefix)} + "' string literal - an "
                "interpolation is a std::string; use an unprefixed literal"});
            return;
        }
        const auto unmatched = detail::scan_interpolations(literal).unmatched;
        if (unmatched != std::string_view::npos) {
            const auto at = position_after(line_starts, offset + position + unmatched);
            diagnostics->push_back({"lexer", diagnostic_severity::error, {at, at},
                "no matching ( for string interpolation ending in )$"});
        }
    };
    // The end of the token `rule` matches at `position`, or `position`.
    const auto match = [&](auto rule) {
        const auto rest = text.substr(position);
        auto input = lexy_input{rest.data(), rest.size()};
        auto scanner = lexy::scan(input, lexy::noop);
        auto lexeme = lexy::scan_result<lexy_lexeme>{};
        if (!scanner.branch(lexeme, dsl::capture(rule)) || !lexeme) return position;
        return position + static_cast<std::size_t>(lexeme.value().size());
    };

    while (position < text.size()) {
        if (std::isspace(static_cast<unsigned char>(text[position])) != 0) {
            ++position;
            continue;
        }
        const auto found = detail::lexeme_at(text, position);
        auto end = found.end;
        if (found.kind == detail::lexeme_kind::none) {
            const auto rest = text.substr(position);
            // `operator*` is not a dereference, as in `operator*=`.
            const auto operator_word = rest.starts_with("operator") &&
                (rest.size() == 8 || !detail::is_identifier_continue(rest[8]));
            end = operator_word ? position : match(pointer_dereference_token);
            if (end == position) end = match(identifier_token);
            if (end == position) end = match(identifier_suffix_token);
            if (end == position) end = match(punctuation_token);
            if (end == position) end = match(single_punctuation_token);
            if (end == position) {
                // A byte and its UTF-8 continuation bytes.
                ++end;
                while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
                    ++end;
                }
                report(end);
            }
            else {
                push(end);
                // `not` followed by whitespace is the operator `!`.
                auto& last = tokens.back();
                if (last.text == "not" && !last.escaped_identifier && end < text.size() &&
                    std::isspace(static_cast<unsigned char>(text[end])) != 0) {
                    last.text = "!";
                    last.kind = token_kind::punctuation;
                }
            }
        }
        else if (!found.terminated) {
            report(end);
        }
        else if (found.kind != detail::lexeme_kind::line_comment &&
                 found.kind != detail::lexeme_kind::block_comment) {
            push(end);
        }
        position = end;
    }

    if (!tokens.empty()) {
        auto end_span_position = position_after(line_starts, std::min(end_offset, data.size()));
        if (end_offset < data.size() && data[end_offset - 1] == '\n') {
            --end_span_position.line;
            end_span_position.column = 1;
        }
        tokens.back().span.end = end_span_position;
    }
    merge_symbol_tokens(tokens);
    split_type_closes(tokens);
    return tokens;
}

}
