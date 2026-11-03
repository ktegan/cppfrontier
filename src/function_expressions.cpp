#include <cppfrontier/pipeline.hpp>

#include "cpp1_text.hpp"
#include "parser_context.hpp"

#include <algorithm>
#include <cctype>
#include <optional>

namespace cppfrontier {

namespace {

using detail::is_close_bracket;

// Whether the `}` at `close` ends a braced initializer such as `T{...}` rather
// than a block. A `{` in a statement begun by a control keyword, as in
// `if c {` or `do {`, opens a block.
auto is_braced_initializer_close(const token_list& tokens, std::size_t close) -> bool {
    const auto open = detail::bracket_open(tokens, 0, close);
    if (!open || *open == 0) return false;
    const auto& previous = tokens[*open - 1];
    if (previous.kind != token_kind::identifier && previous.text != ">" &&
        previous.text != "(" && previous.text != "," && previous.text != "[") {
        return false;
    }
    for (auto at = *open; at-- > 0;) {
        const auto& text = tokens[at].text;
        if (is_close_bracket(text)) {
            const auto inner = detail::bracket_open(tokens, 0, at);
            if (!inner) return true;
            at = *inner;
        }
        else if (text == ";" || text == "{" || text == "}" || text == "(" || text == "[") {
            return true;
        }
        else if (text == "if" || text == "while" || text == "for" || text == "do" ||
                 text == "else" || text == "try" || text == "inspect") {
            return false;
        }
    }
    return true;
}

// Whether the `(` at `open` begins an argument list: its contents read as an
// expression list, not as statement parameters such as `(copy i := 0)`.
auto is_argument_list(const token_list& tokens, std::size_t open, std::size_t end) -> bool {
    const auto close = parser_detail::find_top_level(tokens, open + 1, end,
        parser_detail::angle_rule::expression, [&](std::size_t at) {
            return tokens[at].text == ":=" ||
                (tokens[at].text == ":" && tokens[at - 1].kind == token_kind::identifier);
        });
    return close < end && tokens[close].text == ")";
}

// A `:` begins an unnamed function where an expression can begin: after an
// operator, a bracket other than `)` or `]`, or a keyword that an expression
// follows: `return`, `for`, `if`, `while` or `throw`. After a name it declares
// that name.
auto begins_function_expression(const token_list& tokens, std::size_t position) -> bool {
    if (position == 0 || position + 1 >= tokens.size() || tokens[position].text != ":") {
        return false;
    }
    const auto& next = tokens[position + 1].text;
    if (next != "(" && next != "<") return false;
    const auto& previous = tokens[position - 1];
    if (previous.text == "return" || previous.text == "for" || previous.text == "if" ||
        previous.text == "while" || previous.text == "throw") {
        return true;
    }
    // The lexer reads `*` and `*=` as `pointer_suffix`, and `p*` as one token. A `*`
    // multiplies after an operand; elsewhere it begins a pointer type, as in `p: * :(...)`.
    if (previous.kind == token_kind::pointer_dereference || previous.text == "*=") return true;
    if (previous.text == "*") {
        if (position < 2) return false;
        const auto& left = tokens[position - 2];
        return left.kind == token_kind::identifier || left.kind == token_kind::integer_literal ||
            left.kind == token_kind::string_literal ||
            left.kind == token_kind::character_literal ||
            left.kind == token_kind::pointer_dereference || left.text == ")" || left.text == "]";
    }
    return previous.kind == token_kind::punctuation && previous.text != ")" &&
        previous.text != "]" &&
        previous.text != ">" && previous.text != ">>" && !previous.text.starts_with("operator");
}

struct extent final {
    std::size_t end;
    bool expression_body;
};

// The end of the function expression beginning at `begin`, or nullopt when the
// signature is not followed by an `=` body, as in the type `std::function<:(int) -> int>`.
auto function_expression_extent(const token_list& tokens, std::size_t begin)
    -> std::optional<extent> {
    const auto end = tokens.size();
    auto position = begin + 1;
    if (tokens[position].text == "<") {
        const auto close = detail::angle_close(tokens, position, end);
        if (!close) return std::nullopt;
        position = *close + 1;
    }
    if (position >= end || tokens[position].text != "(") return std::nullopt;
    const auto parameters_close = detail::bracket_close(tokens, position, end);
    if (!parameters_close) return std::nullopt;
    position = *parameters_close + 1;
    if (position >= end) return std::nullopt;
    // `:(x) x + 1` has no `=`, which the declaration parser reports.
    if (const auto& next = tokens[position].text;
        next != "->" && next != "=" && next != "==" && next != "requires" && next != "throws") {
        if (is_close_bracket(next) || next == ";" || next == "," || next == ">" || next == ">>") {
            return std::nullopt;
        }
    }
    else {
        // The return type and `requires` clause end at the `=` or `==` before the body.
        position = parser_detail::find_top_level(tokens, position, end,
            parser_detail::angle_rule::type, [&](std::size_t at) {
                const auto& text = tokens[at].text;
                return text == "=" || text == "==" || text == ";" || text == ",";
            });
        if (position >= end || (tokens[position].text != "=" && tokens[position].text != "==")) {
            return std::nullopt;
        }
        ++position;
    }
    if (position >= end) return std::nullopt;
    if (tokens[position].text == "{") {
        const auto close = detail::bracket_close(tokens, position, end);
        if (!close) return std::nullopt;
        return extent{*close + 1, false};
    }
    position = parser_detail::find_top_level(tokens, position, end,
        parser_detail::angle_rule::expression,
        [&](std::size_t at) { return tokens[at].text == "," || tokens[at].text == ";"; });
    if (position < end && tokens[position].text == ";") {
        // The `;` ends the body inside a larger expression, as in
        // `f(:() = 0;)`, `T{:() = 0;}` and the invocation `:() = x;(a)`;
        // otherwise it ends the enclosing statement.
        const auto following = position + 1 < end ? tokens[position + 1].text : "";
        if ((following == "(" && is_argument_list(tokens, position + 1, end)) ||
            following == ")" || following == "]" ||
            following == "," || following == "." || following == "is" ||
            following == "as" ||
            (following == "}" && is_braced_initializer_close(tokens, position + 1))) {
            ++position;
        }
    }
    return extent{position, true};
}

auto is_word(char character) -> bool {
    return std::isalnum(static_cast<unsigned char>(character)) || character == '_';
}

auto joined_text(const token_list& tokens, std::size_t begin, std::size_t end) -> std::string {
    auto result = std::string{};
    for (auto position = begin; position < end; ++position) {
        const auto& text = tokens[position].text;
        if (!result.empty() && !text.empty() && is_word(result.back()) && is_word(text.front())) {
            result += ' ';
        }
        result += text;
    }
    return result;
}

// The first token of the postfix expression ending just before the `$` at `position`.
auto capture_begin(const token_list& tokens, std::size_t begin, std::size_t position)
    -> std::optional<std::size_t> {
    // The `<` of the template argument list closed by the `>` at `close`, when
    // a name precedes it.
    const auto template_open = [&](std::size_t close) -> std::optional<std::size_t> {
        auto depth = 0;
        for (auto at = close + 1; at-- > begin;) {
            const auto& text = tokens[at].text;
            if (is_close_bracket(text)) {
                const auto open = detail::bracket_open(tokens, 0, at);
                if (!open || *open < begin) return std::nullopt;
                at = *open;
            }
            else if (text == ">") ++depth;
            else if (text == "<" && --depth == 0) {
                if (at == begin || tokens[at - 1].kind != token_kind::identifier) return std::nullopt;
                return at;
            }
            else if (text == ";" || text == "{" || text == "}") return std::nullopt;
        }
        return std::nullopt;
    };
    auto current = position;
    while (current > begin) {
        const auto& text = tokens[current - 1].text;
        if (text == "&" || (!text.empty() && text.find_first_not_of('*') == std::string::npos)) {
            --current;
            continue;
        }
        if (text == ">") {
            const auto open = template_open(current - 1);
            if (!open) return std::nullopt;
            current = *open;
            continue;
        }
        if (is_close_bracket(text)) {
            const auto open = detail::bracket_open(tokens, 0, current - 1);
            if (!open || *open <= begin) return std::nullopt;
            current = *open;
            const auto& callee = tokens[current - 1];
            if (callee.text == ">" && template_open(current - 1)) continue;
            if (callee.text == "..." && current >= begin + 2 && tokens[current - 2].text == "sizeof") {
                return current - 2;
            }
            if (callee.kind != token_kind::identifier && !is_close_bracket(callee.text)) return current;
            continue;
        }
        const auto& value = tokens[current - 1];
        // The lexer reads `p*` as one token.
        if (value.kind == token_kind::identifier || value.kind == token_kind::pointer_dereference ||
            value.text == "this" || value.text == "that") {
            --current;
            if (current > begin + 1 &&
                (tokens[current - 1].text == "." || tokens[current - 1].text == "::")) {
                --current;
                continue;
            }
            return current;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

// Whether the postfix expression ending just before the `$` at `position`
// already holds a capture.
auto follows_capture(const token_list& tokens, std::size_t position) -> bool {
    for (auto current = position; current-- > 0;) {
        const auto& value = tokens[current];
        if (value.text == "$") return true;
        if (is_close_bracket(value.text)) {
            const auto open = detail::bracket_open(tokens, 0, current);
            if (!open) return false;
            current = *open;
        }
        else if (value.kind != token_kind::identifier &&
                 value.kind != token_kind::pointer_dereference && value.text != "." &&
                 value.text != ".." && value.text != "::" && value.text != "&" &&
                 value.text != "this" && value.text != "that") {
            return false;
        }
    }
    return false;
}

}

auto parser_detail::capture_begin(const token_list& tokens, std::size_t begin,
                                  std::size_t position) -> std::optional<std::size_t> {
    return cppfrontier::capture_begin(tokens, begin, position);
}

// A capture is the postfix expression before its `$`, as outside a string.
auto parser_detail::replace_interpolated_captures(
    std::string text, const std::function<std::string(const std::string&)>& capture)
    -> std::string {
    const auto parts = detail::scan_interpolations(text).parts;
    // From the back, so that a replacement keeps the earlier offsets.
    for (auto part = parts.rbegin(); part != parts.rend(); ++part) {
        const auto [open, close] = *part;
        for (auto dollar = close; dollar-- > open + 1;) {
            if (text[dollar] != '$') continue;
            const auto before = source_text{text.substr(open + 1, dollar - open - 1)};
            const auto tokens = tokenize(before, 1,
                static_cast<std::uint32_t>(before.line_starts().size() + 1));
            const auto captured = capture_begin(tokens, 0, tokens.size());
            if (!captured) continue;
            const auto position = tokens[*captured].span.begin;
            const auto begin = open + 1 + before.line_starts()[position.line - 1] +
                position.column - 1;
            text.replace(begin, dollar + 1 - begin, capture(text.substr(begin, dollar - begin)));
            dollar = begin;
        }
    }
    return text;
}

namespace {

auto extract(token_list& tokens, token_list& declarations, function_expression_result& result,
             std::size_t first_index) -> void;

// The source position of `offset` in the text of `value`.
auto position_at(const token& value, std::size_t offset) -> source_position {
    auto result = value.span.begin;
    for (auto index = std::size_t{0}; index < offset; ++index) {
        if (value.text[index] == '\n') {
            ++result.line;
            result.column = 1;
        }
        else {
            ++result.column;
        }
    }
    return result;
}

auto offset_at(const token& value, source_position position) -> std::size_t {
    auto offset = std::size_t{0};
    for (auto line = value.span.begin.line; line < position.line; ++line) {
        offset = value.text.find('\n', offset) + 1;
    }
    const auto line_begin = position.line == value.span.begin.line ? value.span.begin.column : 1;
    return offset + position.column - line_begin;
}

// Extracts the function expressions in the interpolations of the string
// literal `value`, which then names them.
auto extract_interpolated(token& value, token_list& declarations,
                          function_expression_result& result, std::size_t first_index) -> void {
    auto text = value.text;
    const auto chunks = detail::scan_interpolations(value.text).parts;
    for (auto chunk = chunks.rbegin(); chunk != chunks.rend(); ++chunk) {
        const auto [open, close] = *chunk;
        // An ordinary string's interpolation is tokenized without its escapes, as
        // `unescape_interpolation` reads it; `origin` maps each character to its offset.
        auto expression = std::string{};
        auto origin = std::vector<std::size_t>{};
        const auto raw = value.text.starts_with("$R");
        for (auto index = open; index <= close; ++index) {
            if (!raw && value.text[index] == '\\' && value.text[index - 1] != '\'' && index < close) {
                ++index;
            }
            else if (!raw && value.text[index] == '\\' && index < close) {
                origin.push_back(index);
                expression += value.text[index++];
            }
            origin.push_back(index);
            expression += value.text[index];
        }
        origin.push_back(close + 1);
        if (expression.find(':') == std::string::npos) continue;
        // Tokenize the interpolation at its own column, then move it to its line.
        const auto begin = position_at(value, open);
        const auto source = source_text{std::string(begin.column - 1, ' ') + expression};
        const auto lines = static_cast<std::uint32_t>(
            std::count(expression.begin(), expression.end(), '\n'));
        auto tokens = tokenize(source, 1, lines + 2);
        // A token spans its escaped source text, which the parser reads as the token's.
        auto unescaped = std::vector<source_replacement>{};
        for (auto& moved : tokens) {
            moved.span.begin.line += begin.line - 1;
            moved.span.end.line += begin.line - 1;
            if (raw) continue;
            // A token begins after the character before it, so it includes its escapes.
            const auto after = [&](std::size_t index) {
                return begin.column + static_cast<std::uint32_t>(
                    (index == 0 ? origin[0] : origin[index - 1] + 1) - open);
            };
            moved.span.begin.column = after(moved.span.begin.column - begin.column);
            moved.span.end.column = after(moved.span.end.column - begin.column);
            const auto from = offset_at(value, moved.span.begin);
            if (value.text.compare(from, offset_at(value, moved.span.end) - from, moved.text) != 0) {
                unescaped.push_back({moved.span, moved.text});
            }
        }
        const auto extracted = result.expressions.size();
        extract(tokens, declarations, result, first_index);
        if (result.expressions.size() == extracted) continue;
        result.replacements.insert(result.replacements.end(), unescaped.begin(), unescaped.end());
        for (auto name = tokens.rbegin(); name != tokens.rend(); ++name) {
            if (!is_function_expression_name(name->text)) continue;
            const auto from = offset_at(value, name->span.begin);
            text.replace(from, offset_at(value, name->span.end) - from, name->text);
        }
    }
    if (text == value.text) return;
    result.replacements.push_back({value.span, text});
    value.text = std::move(text);
}

auto extract(token_list& tokens, token_list& declarations, function_expression_result& result,
             std::size_t first_index) -> void {
    // The last function expression contains no other, so extracting from the
    // back lowers nested expressions innermost first.
    for (auto position = tokens.size(); position-- > 0;) {
        if (tokens[position].kind == token_kind::string_literal) {
            extract_interpolated(tokens[position], declarations, result, first_index);
            continue;
        }
        if (!begins_function_expression(tokens, position)) continue;
        const auto found = function_expression_extent(tokens, position);
        if (!found) continue;
        auto expression = function_expression{};
        expression.name = "__cpp2_function_expression_" +
            std::to_string(first_index + result.expressions.size());
        auto body = token_list{tokens.begin() + static_cast<std::ptrdiff_t>(position),
                               tokens.begin() + static_cast<std::ptrdiff_t>(found->end)};
        // Captures are read from the back, so a repeated capture is placed at
        // its first occurrence; one in a string interpolation has no place.
        const auto capture_name = [&](const std::string& text,
                                      std::optional<source_position> begin = std::nullopt) {
            const auto existing = std::find_if(expression.captures.begin(),
                expression.captures.end(), [&](const auto& capture) {
                    return capture.expression.source() == text;
                });
            if (existing != expression.captures.end()) {
                if (begin) existing->expression = parsed_expression{text, false, false, false, begin};
                return existing->name;
            }
            auto name = "_" + std::to_string(expression.captures.size());
            expression.captures.push_back({name, parsed_expression{text, false, false, false, begin}});
            return name;
        };
        for (auto dollar = body.size(); dollar-- > 0;) {
            if (body[dollar].kind == token_kind::string_literal) {
                auto text =
                    parser_detail::replace_interpolated_captures(body[dollar].text, capture_name);
                if (text != body[dollar].text) {
                    // The literal may already read as the names of its function expressions.
                    const auto span = body[dollar].span;
                    const auto existing = std::find_if(result.replacements.begin(),
                        result.replacements.end(), [&](const auto& replacement) {
                            return std::pair{replacement.span.begin.line,
                                             replacement.span.begin.column} ==
                                std::pair{span.begin.line, span.begin.column};
                        });
                    if (existing != result.replacements.end()) existing->text = text;
                    else result.replacements.push_back({span, text});
                    body[dollar].text = std::move(text);
                }
                continue;
            }
            if (body[dollar].text != "$") continue;
            const auto captured = capture_begin(body, 0, dollar);
            if (!captured) {
                if (follows_capture(body, dollar)) {
                    result.diagnostics.push_back({"repeated-capture", diagnostic_severity::error,
                        body[dollar].span,
                        "$ (capture) can appear at most once in a single postfix-expression"});
                    body.erase(body.begin() + static_cast<std::ptrdiff_t>(dollar));
                }
                continue;
            }
            const auto name = capture_name(joined_text(body, *captured, dollar),
                                           body[*captured].span.begin);
            auto replacement = body[*captured];
            replacement.kind = token_kind::identifier;
            replacement.text = name;
            replacement.span.end = body[dollar].span.end;
            result.replacements.push_back({replacement.span, name});
            body.erase(body.begin() + static_cast<std::ptrdiff_t>(*captured),
                       body.begin() + static_cast<std::ptrdiff_t>(dollar) + 1);
            body.insert(body.begin() + static_cast<std::ptrdiff_t>(*captured), replacement);
            dollar = *captured;
        }
        auto placeholder = tokens[position];
        placeholder.kind = token_kind::identifier;
        placeholder.text = expression.name;
        placeholder.span.end = tokens[found->end - 1].span.end;
        result.replacements.push_back({placeholder.span, expression.name});
        declarations.push_back(placeholder);
        declarations.insert(declarations.end(), body.begin(), body.end());
        if (declarations.back().text != ";" && found->expression_body) {
            auto semicolon = declarations.back();
            semicolon.kind = token_kind::punctuation;
            semicolon.text = ";";
            semicolon.span.begin = semicolon.span.end;
            declarations.push_back(semicolon);
        }
        tokens.erase(tokens.begin() + static_cast<std::ptrdiff_t>(position),
                     tokens.begin() + static_cast<std::ptrdiff_t>(found->end));
        tokens.insert(tokens.begin() + static_cast<std::ptrdiff_t>(position), placeholder);
        result.expressions.push_back(std::move(expression));
    }
}

}

auto extract_function_expressions(token_list tokens, std::size_t first_index)
    -> function_expression_result {
    auto result = function_expression_result{};
    auto declarations = token_list{};
    extract(tokens, declarations, result, first_index);
    tokens.insert(tokens.end(), declarations.begin(), declarations.end());
    std::sort(result.replacements.begin(), result.replacements.end(),
              [](const auto& left, const auto& right) {
                  return std::pair{left.span.begin.line, left.span.begin.column} <
                      std::pair{right.span.begin.line, right.span.begin.column};
              });
    result.tokens = std::move(tokens);
    return result;
}

}
