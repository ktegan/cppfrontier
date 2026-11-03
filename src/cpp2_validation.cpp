#include <cppfrontier/pipeline.hpp>
#include <cppfrontier/type.hpp>

#include "cpp1_text.hpp"
#include "parser_context.hpp"

#include <algorithm>
#include <cctype>
#include <array>
#include <initializer_list>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cppfrontier {
namespace {

auto is_word(const token& value, std::string_view text) -> bool {
    return value.text == text &&
        (value.kind == token_kind::identifier || value.kind == token_kind::keyword);
}

auto is_legacy_type_word(const token& value) -> bool {
    return (value.kind == token_kind::identifier || value.kind == token_kind::keyword) &&
        is_cpp1_fundamental_word(value.text);
}

auto validate_legacy_types(const token_list& tokens,
                           std::vector<diagnostic>& diagnostics) -> void {
    auto position = std::size_t{0};
    while (position < tokens.size()) {
        if (!is_legacy_type_word(tokens[position])) {
            ++position;
            continue;
        }
        auto end = position;
        auto words = std::vector<std::string_view>{};
        auto spelling = std::string{};
        while (end < tokens.size() && is_legacy_type_word(tokens[end])) {
            if (!spelling.empty()) spelling += ' ';
            spelling += tokens[end].text;
            words.push_back(tokens[end].text);
            ++end;
        }
        if (words.size() > 1) {
            const auto replacement = cpp2_fundamental_suggestion(words);
            auto message = "'" + spelling + "' is an old-style C/C++ multi-word type";
            if (!replacement.empty()) {
                message += " - did you mean " + std::string{replacement} + "?";
            }
            diagnostics.push_back({
                "cpp1-multiword-type", diagnostic_severity::error,
                tokens[position].span, std::move(message)});
        }
        position = end;
    }
}

// Cpp2 has no conditional operator `c ? a : b`.
auto validate_conditional_operator(const token_list& tokens,
                                   std::vector<diagnostic>& diagnostics) -> void {
    for (const auto& value : tokens) {
        if (value.text != "?") continue;
        diagnostics.push_back({
            "cpp1-conditional-operator", diagnostic_severity::error, value.span,
            "the conditional operator 'c ? a : b' is not valid Cpp2; use an 'if' statement "
            "or an 'inspect' expression"});
    }
}

// `.*` and `->*` have no Cpp2 spelling; cppfront reads them as `.` or `->`
// without a member name. (`-> *T` is a pointer return type.)
auto validate_pointer_to_member(const token_list& tokens,
                                std::vector<diagnostic>& diagnostics) -> void {
    for (auto position = std::size_t{0}; position < tokens.size(); ++position) {
        const auto& text = tokens[position].text;
        auto op = std::string{};
        if (text == ".*" || text == "->*") op = text.substr(0, text.size() - 1);
        else if (text == "." && position + 1 < tokens.size() &&
                 tokens[position + 1].text == "*") {
            op = text;
        }
        else continue;
        diagnostics.push_back({
            "pointer-to-member-operator", diagnostic_severity::error, tokens[position].span,
            "'" + op + "' must be followed by a valid member name"});
    }
}

// A statement cannot start with `.` or be an empty `()`.
auto validate_incomplete_statement(const token_list& tokens,
                                   std::vector<diagnostic>& diagnostics) -> void {
    for (auto position = std::size_t{1}; position < tokens.size(); ++position) {
        const auto& text = tokens[position].text;
        const auto& previous = tokens[position - 1].text;
        const auto empty = text == "(" && position + 2 < tokens.size() &&
            tokens[position + 1].text == ")" && tokens[position + 2].text == ";" &&
            (previous == ";" || previous == "{" || previous == "}");
        if (!empty && (previous != ";" || (text != "." && text != ".."))) continue;
        diagnostics.push_back({
            "invalid-statement", diagnostic_severity::error, tokens[position].span,
            "invalid statement encountered inside a compound-statement"});
    }
}

auto validate_captures(const token_list& tokens,
                       std::vector<diagnostic>& diagnostics) -> void {
    // A postcondition `post(...)` captures with `$` as well.
    auto post_end = std::size_t{0};
    for (auto position = std::size_t{0}; position < tokens.size(); ++position) {
        const auto& value = tokens[position];
        if (value.kind == token_kind::identifier && value.text == "post" && position >= post_end) {
            // Skip a `<group, predicates...>` list, which may name `this`.
            auto end = position + 1;
            if (end < tokens.size() && tokens[end].text == "<") {
                end = parser_detail::angle_close(tokens, end, tokens.size()).value_or(end) + 1;
            }
            if (end < tokens.size() && tokens[end].text == "(") {
                if (const auto close = parser_detail::bracket_close(tokens, end, tokens.size())) {
                    post_end = *close + 1;
                }
            }
        }
        // A function expression's interpolated captures are already replaced.
        const auto interpolated = value.kind == token_kind::string_literal &&
            parser_detail::replace_interpolated_captures(value.text, [](const std::string&) {
                return std::string{};
            }) != value.text;
        if ((value.text != "$" && !interpolated) || position < post_end) continue;
        diagnostics.push_back({
            "unsupported-capture", diagnostic_severity::error, value.span,
            "$ captures are supported only in unnamed function expressions, postconditions "
            "and string interpolations"});
    }
}

auto validate_cpp1_words(const token_list& tokens, std::vector<diagnostic>& diagnostics) -> void {
    const auto add = [&](const token& at, std::string code, std::string message) {
        diagnostics.push_back({std::move(code), diagnostic_severity::error, at.span,
                               std::move(message)});
    };
    const auto text = [&](std::size_t position) -> std::string_view {
        return position < tokens.size() ? std::string_view{tokens[position].text} : "";
    };
    for (auto position = std::size_t{0}; position < tokens.size(); ++position) {
        const auto& value = tokens[position];
        if ((value.kind != token_kind::identifier && value.kind != token_kind::keyword) ||
            value.escaped_identifier) {
            continue;
        }
        if (value.text == "delete" && text(position - 1) != "operator") {
            add(value, "cpp1-delete", "'delete' and owning raw pointers are not supported in "
                "Cpp2 - use unique.new<T> or shared.new<T> instead in that order (or, in the "
                "future, gc.new<T>, but that is not yet implemented)");
        }
        else if (value.text == "NULL") {
            add(value, "cpp1-null", "'NULL' is not supported in Cpp2 - for a local pointer "
                "variable, leave it uninitialized instead, and set it to a non-null value when "
                "you have one");
        }
        else if (value.text == "decltype" && text(position + 1) == "(" &&
                 text(position + 2) == "auto" && text(position + 3) == ")") {
            add(value, "cpp1-decltype-auto", "decltype(auto) is not needed in Cpp2 - for "
                "return types, use '-> forward _' instead");
        }
        else if (value.text == "static_cast") {
            add(value, "cpp1-cast", "'static_cast<T>(expr)' is not supported in Cpp2 - use "
                "'expr as T' instead, which allows only type-safe conversions; to opt into "
                "possibly-lossy narrowing conversions, use 'unchecked_narrow<T>(expr)'");
        }
        else if (value.text == "dynamic_cast") {
            add(value, "cpp1-cast", "'dynamic_cast<Derived*>(pBase)' is not supported in Cpp2 - "
                "use 'pBase as *Derived' instead for safe dynamic pointer conversions");
        }
        else if (value.text == "reinterpret_cast") {
            add(value, "cpp1-cast", "'reinterpret_cast' is not supported in Cpp2 - use "
                "'unchecked_cast<T>(expr)', which reinterprets when no static conversion "
                "exists");
        }
        else if (value.text == "const_cast") {
            add(value, "cpp1-cast", "'const_cast' is not supported in Cpp2 - use "
                "'unchecked_cast<T>(expr)' if an unchecked conversion is necessary");
        }
        else if (value.text == "decltype" && text(position + 1) != "(") {
            add(value, "invalid-decltype",
                "'decltype' must be followed by a single parenthesized expression");
        }
    }
}

// Whether the tokens from `begin` reach an `=` before a `,`, `;` or closer at
// their own nesting level; `>=` and `>>=` close template arguments before `=`.
auto has_top_level_initializer(const token_list& tokens, std::size_t begin) -> bool {
    const auto end = parser_detail::find_top_level(tokens, begin, tokens.size(),
        parser_detail::angle_rule::type, [&](std::size_t position) {
            const auto& text = tokens[position].text;
            return text == "=" || text == "," || text == ";" ||
                (text.ends_with('=') && text.find_first_not_of('>') == text.size() - 1);
        });
    return end < tokens.size() && tokens[end].text.ends_with('=');
}

// `std::function<:(x: int) -> void>`: a `:(` signature without an `=` body is
// a function type, which is written without the `:`.
auto validate_function_type_colon(const token_list& tokens,
                                  std::vector<diagnostic>& diagnostics) -> void {
    for (auto position = std::size_t{1}; position + 1 < tokens.size(); ++position) {
        if (tokens[position].text != ":" || tokens[position + 1].text != "(") continue;
        const auto& previous = tokens[position - 1].text;
        if (previous != "<" && previous != ",") continue;
        if (has_top_level_initializer(tokens, position + 1)) continue;
        diagnostics.push_back({
            "function-type-colon", diagnostic_severity::error, tokens[position].span,
            "a function type is written without ':'; drop the ':' or add an '=' initializer"});
    }
}

// `(:int)`: a typed value after `(` or `,` needs an `= value` initializer.
auto validate_typed_value_initializer(const token_list& tokens,
                                      std::vector<diagnostic>& diagnostics) -> void {
    for (auto position = std::size_t{1}; position + 1 < tokens.size(); ++position) {
        if (tokens[position].text != ":") continue;
        const auto& previous = tokens[position - 1].text;
        const auto& next = tokens[position + 1].text;
        if ((previous != "(" && previous != ",") || next == "(" || next == "<") continue;
        if (has_top_level_initializer(tokens, position + 1)) continue;
        diagnostics.push_back({
            "typed-value-initializer", diagnostic_severity::error, tokens[position].span,
            "an anonymous object declaration must have '=' and an initializer"});
    }
}

// Cpp1 keywords that name types, so cannot be operands of a Cpp2 `*` or `&`.
auto is_cpp1_type_keyword(std::string_view text) -> bool {
    static constexpr auto words = std::array<std::string_view, 15>{
        "char", "short", "int", "long", "double", "signed", "unsigned", "void", "bool",
        "float", "wchar_t", "char8_t", "char16_t", "char32_t", "auto"};
    return std::find(words.begin(), words.end(), text) != words.end();
}

auto is_cv(std::string_view text) -> bool { return text == "const" || text == "volatile"; }

// Appends `text`, separated by a space where two words would join.
auto append_spelling(std::string& output, std::string_view text) -> void {
    const auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    if (!output.empty() && !text.empty() && word(output.back()) && word(text.front())) {
        output += ' ';
    }
    output += text;
}

// A Cpp1 declarator in the template argument [begin, end): trailing `const`,
// `&&`, a `*` or `&` after a Cpp1 type keyword, or a `*` or `&` after a leading
// `const`. `T*` and `T&` are Cpp2 dereference and address-of, so stay.
auto cpp1_declarator_suggestion(const token_list& tokens, std::size_t begin, std::size_t end)
    -> std::optional<std::string> {
    // The argument's top-level words, with `int*` split into `int` and `*`.
    auto words = std::vector<std::string>{};
    for (auto index = begin; index < end; ++index) {
        const auto& value = tokens[index];
        // A bracket or template list joins the word before it, through its
        // close or, when it has none, the end of the argument.
        const auto opens = detail::is_open_bracket(value.text) || value.text == "<";
        if (opens || detail::is_close_bracket(value.text) || detail::angle_closes(value.text) != 0) {
            const auto close = !opens ? std::optional{index}
                : value.text == "<" ? parser_detail::angle_close(tokens, index, end)
                                    : parser_detail::bracket_close(tokens, index, end);
            if (words.empty()) words.emplace_back();
            for (const auto last = close.value_or(end - 1); index <= last; ++index) {
                append_spelling(words.back(), tokens[index].text);
            }
            --index;
            continue;
        }
        if (value.text == ":") return std::nullopt;
        if (value.kind == token_kind::pointer_dereference) {
            words.push_back(value.text.substr(0, value.text.size() - 1));
            words.emplace_back("*");
        }
        else {
            words.push_back(value.text);
        }
    }
    const auto is_suffix = [](const std::string& word) {
        return is_cv(word) || word == "*" || word == "&" || word == "&&";
    };
    auto base_begin = std::size_t{0};
    while (base_begin < words.size() && is_cv(words[base_begin])) ++base_begin;
    auto base_end = words.size();
    while (base_end > base_begin && is_suffix(words[base_end - 1])) --base_end;
    if (base_end == words.size() || base_end == base_begin) return std::nullopt;
    auto has_cv = false;
    for (auto index = base_end; index < words.size(); ++index) has_cv = has_cv || is_cv(words[index]);
    if (!has_cv && words.back() != "&&" && base_begin == 0 &&
        !(base_end == 1 && is_cpp1_type_keyword(words.front()))) {
        return std::nullopt;
    }
    // Cpp1 declarators read right to left; Cpp2 qualifiers read left to right.
    auto result = std::string{};
    auto has_reference = false;
    for (auto index = words.size(); index > base_end; --index) {
        const auto& word = words[index - 1];
        if (word == "&" || word == "&&") has_reference = true;
        else result += word == "*" ? "*" : word + " ";
    }
    for (auto index = std::size_t{0}; index < base_begin; ++index) result += words[index] + " ";
    for (auto index = base_begin; index < base_end; ++index) append_spelling(result, words[index]);
    return "write the Cpp2 type '" + result + "'" +
        (has_reference ? std::string{"; Cpp2 types have no references"} : std::string{});
}

// Template arguments, the defaults of type template parameters, and the
// type-id of `sizeof`, `alignof` or `typeid` that begins with `*` or `const`
// use Cpp2 type syntax, with `const` and `*` before the type. An argument
// ending in postfix `*`, a Cpp2 dereference that Cpp1 would read as a pointer
// type, is rejected, as is a `sizeof` type-id that names no type.
auto validate_template_arguments(const token_list& tokens,
                                 const std::vector<std::optional<std::size_t>>& closes,
                                 std::vector<diagnostic>& diagnostics) -> void {
    const auto check = [&](std::size_t begin, std::size_t index) {
        const auto spelling = [&] {
            auto result = std::string{};
            for (auto at = begin; at < index; ++at) {
                const auto& previous = tokens[at - 1].span.end;
                const auto& current = tokens[at].span.begin;
                if (at != begin && (current.line != previous.line ||
                                    current.column > previous.column)) {
                    result += ' ';
                }
                result += tokens[at].text;
            }
            return result;
        };
        if (const auto suggestion = cpp1_declarator_suggestion(tokens, begin, index)) {
            diagnostics.push_back({
                "cpp1-declarator-in-template-argument", diagnostic_severity::error,
                tokens[begin].span,
                "'" + spelling() + "' is a Cpp1 declarator; " + *suggestion});
        }
        else if (index > begin && tokens[index - 1].text.ends_with('*')) {
            const auto text = spelling();
            const auto base = text.find_last_not_of('*') + 1;
            diagnostics.push_back({
                "postfix-star-in-template-argument", diagnostic_severity::error,
                tokens[begin].span,
                "'" + text + "' is a dereference in Cpp2; write '" + text.substr(base) +
                text.substr(0, base) + "' for a pointer type, or '(" + text +
                ")' for a dereference"});
        }
    };
    for (auto open = std::size_t{1}; open + 1 < tokens.size(); ++open) {
        const auto& word = tokens[open - 1].text;
        if (tokens[open].text != "(" || (word != "sizeof" && word != "alignof" &&
                                         word != "typeid") ||
            (tokens[open + 1].text != "*" && tokens[open + 1].text != "const")) {
            continue;
        }
        const auto close = parser_detail::bracket_close(tokens, open, tokens.size());
        if (!close) continue;
        check(open + 1, *close);
        // Only `*`, `const` and `_` leave the type to be deduced, as `const` does in
        // a declaration, which has no meaning here.
        if (std::all_of(tokens.begin() + static_cast<std::ptrdiff_t>(open + 1),
                        tokens.begin() + static_cast<std::ptrdiff_t>(*close), [](const auto& token) {
                            return token.text == "*" || token.text == "const" || token.text == "_";
                        })) {
            diagnostics.push_back({"deduced-type-operand", diagnostic_severity::error,
                tokens[open + 1].span,
                "'" + word + "' needs a type, but this one is deduced; name the type, as in '" +
                word + "(const int)'"});
        }
    }
    for (auto open = std::size_t{1}; open < tokens.size(); ++open) {
        if (tokens[open].text != "<") continue;
        if (tokens[open - 1].text == ":") {
            // A template parameter list: `T = X` and `T: type = X` default to a type.
            const auto close = parser_detail::angle_close(tokens, open, tokens.size());
            if (!close) continue;
            for (const auto& [begin, index] : parser_detail::top_level_items(
                     tokens, open + 1, *close, parser_detail::angle_rule::type)) {
                if (begin + 2 < index && tokens[begin + 1].text == "=") check(begin + 2, index);
                else if (begin + 4 < index && tokens[begin + 1].text == ":" &&
                         tokens[begin + 2].text == "type" && tokens[begin + 3].text == "=") {
                    check(begin + 4, index);
                }
            }
            continue;
        }
        // A contract's group and flags are values, as in `assert<group*>(c)`.
        const auto& name = tokens[open - 1].text;
        if (tokens[open - 1].kind != token_kind::identifier || name.starts_with("operator") ||
            name == "assert" || name == "pre" || name == "post") {
            continue;
        }
        const auto close = closes[open];
        if (!close) continue;
        for (const auto& [begin, index] : parser_detail::top_level_items(
                 tokens, open + 1, *close, parser_detail::angle_rule::type)) {
            // In `std::vector<std::vector<int*>>` the inner list ends at the same
            // `>>`, and the inner argument `int*` is checked on its own.
            const auto nested = std::any_of(tokens.begin() + static_cast<std::ptrdiff_t>(begin),
                tokens.begin() + static_cast<std::ptrdiff_t>(index), [&](const token& value) {
                    const auto at = static_cast<std::size_t>(&value - tokens.data());
                    return value.text == "<" && closes[at] == *close;
                });
            if (!nested) check(begin, index);
        }
    }
}

// The parameters of a requires-expression `requires (parameters) {` are Cpp2
// parameters.
auto validate_requires_parameters(const token_list& tokens,
                                  std::vector<diagnostic>& diagnostics) -> void {
    const auto passing = std::array<std::string_view, 7>{
        "in", "in_ref", "copy", "inout", "out", "move", "forward"};
    for (auto open = std::size_t{1}; open < tokens.size(); ++open) {
        if (tokens[open].text != "(" || tokens[open - 1].text != "requires") continue;
        const auto close = parser_detail::bracket_close(tokens, open, tokens.size());
        if (!close || *close + 1 >= tokens.size() || tokens[*close + 1].text != "{") continue;
        for (const auto& [begin, end] : parser_detail::top_level_items(
                 tokens, open + 1, *close, parser_detail::angle_rule::type)) {
            const auto name = std::find(passing.begin(), passing.end(), tokens[begin].text) !=
                    passing.end() ? begin + 1 : begin;
            if (name + 2 < end && tokens[name].kind == token_kind::identifier &&
                tokens[name + 1].text == ":") {
                continue;
            }
            diagnostics.push_back({"cpp1-requires-parameter", diagnostic_severity::error,
                tokens[begin].span, "a requires-expression parameter uses Cpp2 syntax, as in "
                "'a: T' or 'inout a: T'"});
        }
    }
}

auto is_value_keyword(const token& value) -> bool {
    return value.kind == token_kind::keyword &&
        (value.text == "true" || value.text == "false" || value.text == "nullptr" ||
         value.text == "this" || value.text == "that");
}

// A name, literal or value keyword.
auto is_operand(const token& value) -> bool {
    return value.kind == token_kind::identifier || value.kind == token_kind::integer_literal ||
        value.kind == token_kind::string_literal ||
        value.kind == token_kind::character_literal ||
        value.kind == token_kind::pointer_dereference || is_value_keyword(value);
}

// How the tokens of a Cpp2 section are used in expressions, read left to right
// with the expression parser's rules.
struct token_roles final {
    // The `>` closing each template argument list, indexed by its `<`.
    std::vector<std::optional<std::size_t>> template_close;
    // The number of template argument lists each `>`, `>>` ... closes.
    std::vector<std::size_t> closed_lists;
    // The token ends an operand, so a following `*`, `&`, `~`, `++` or `--`
    // is postfix or binary.
    std::vector<bool> ends_operand;
    // An `is` or `as` operator.
    std::vector<bool> is_as;
    // A `*` here begins or continues a type: after `:`, `->`, `const`, `is`,
    // `as`, `type ==`, a template argument list's `<` or `,`, or the `(` of
    // `sizeof`, `alignof` or `typeid`.
    std::vector<bool> in_type;
};

// With `try_operator`, a `?` after an operand is postfix and ends it.
auto roles_of(const token_list& tokens, bool try_operator = false) -> token_roles {
    const auto size = tokens.size();
    auto roles = token_roles{std::vector<std::optional<std::size_t>>(size),
                             std::vector<std::size_t>(size), std::vector<bool>(size),
                             std::vector<bool>(size), std::vector<bool>(size)};
    const auto closes = parser_detail::template_argument_closes(tokens);
    // Open brackets; true for a template argument list.
    auto opens = std::vector<bool>{};
    // The closes of the template parameter lists `:<...>` the scan is in.
    auto parameter_lists = std::vector<std::size_t>{};
    for (auto index = std::size_t{0}; index < size; ++index) {
        const auto& value = tokens[index];
        const auto& text = value.text;
        const auto after_operand = index != 0 && roles.ends_operand[index - 1];
        while (!parameter_lists.empty() && parameter_lists.back() < index) parameter_lists.pop_back();
        if (text == "<" && index != 0 && tokens[index - 1].kind == token_kind::identifier) {
            roles.template_close[index] = closes[index];
            if (roles.template_close[index]) ++roles.closed_lists[*roles.template_close[index]];
        }
        else if (text == "<" && index != 0 && tokens[index - 1].text == ":") {
            if (const auto close = parser_detail::angle_close(tokens, index, size)) {
                parameter_lists.push_back(*close);
            }
        }
        if (index != 0) {
            const auto& previous = tokens[index - 1].text;
            // The default of a type parameter `T = X` or `T: type = X`.
            const auto type_default = previous == "=" && !parameter_lists.empty() && index >= 3 &&
                ((tokens[index - 2].kind == token_kind::identifier &&
                  (tokens[index - 3].text == "<" || tokens[index - 3].text == ",")) ||
                 (tokens[index - 2].text == "type" && tokens[index - 3].text == ":"));
            roles.in_type[index] = type_default || previous == ":" || previous == "->" ||
                previous == "const" ||
                ((previous == "forward" || previous == "forward_ref") && index >= 2 &&
                 tokens[index - 2].text == "->") ||
                roles.is_as[index - 1] ||
                (previous == "==" && index >= 2 && tokens[index - 2].text == "type") ||
                roles.template_close[index - 1] ||
                (previous == "," && !opens.empty() && opens.back()) ||
                (previous == "*" && roles.in_type[index - 1]) ||
                (previous == "(" && index >= 2 &&
                 (tokens[index - 2].text == "sizeof" || tokens[index - 2].text == "alignof" ||
                  tokens[index - 2].text == "typeid"));
        }
        if (roles.template_close[index]) opens.push_back(true);
        else if (text == "(" || text == "[" || text == "{") opens.push_back(false);
        else if (!opens.empty() && (text == ")" || text == "]" || text == "}")) opens.pop_back();
        for (auto count = roles.closed_lists[index]; count != 0 && !opens.empty(); --count) {
            opens.pop_back();
        }
        if ((text == "is" || text == "as") && value.kind == token_kind::identifier &&
            after_operand) {
            roles.is_as[index] = true;
        }
        // `...` after an operand expands a pack; elsewhere it is a fold's operand.
        else if (is_operand(value) || text == ")" || text == "]" || text == "}" ||
                 text == "..." || roles.closed_lists[index] != 0) {
            roles.ends_operand[index] = true;
        }
        else if (after_operand &&
                 (text == "++" || text == "--" || text == "~" || text == "$" ||
                  (text == "?" && try_operator))) {
            roles.ends_operand[index] = true;
        }
        else if (after_operand && (text == "*" || text == "&")) {
            // As the expression parser: binary before an operand, `::`, `!` or `:`.
            const auto next = index + 1 < size ? std::string_view{tokens[index + 1].text}
                                               : std::string_view{};
            auto postfix = true;
            if (next == "(") postfix = index + 2 < size && tokens[index + 2].text == ")";
            else if ((index + 1 < size && is_operand(tokens[index + 1])) || next == "::" ||
                     next == "!" || next == ":") {
                postfix = false;
            }
            roles.ends_operand[index] = postfix;
        }
    }
    return roles;
}

// Cpp2 has only the prefix operators `!`, `+` and `-`; `*`, `&`, `~`, `++`
// and `--` are postfix.
auto validate_prefix_operators(const token_list& tokens, const token_roles& roles,
                               std::vector<diagnostic>& diagnostics) -> void {
    const auto unary = [&](std::size_t index) {
        const auto& text = tokens[index].text;
        return text == "*" || text == "&" || text == "~" || text == "!" || text == "-" ||
            text == "+" || text == "++" || text == "--";
    };
    for (auto index = std::size_t{0}; index + 1 < tokens.size(); ++index) {
        const auto& text = tokens[index].text;
        if (text != "*" && text != "&" && text != "~" && text != "++" && text != "--") continue;
        if ((index != 0 && roles.ends_operand[index - 1]) || roles.in_type[index]) continue;
        auto operand = index + 1;
        while (operand < tokens.size() && unary(operand)) ++operand;
        if (operand == tokens.size() ||
            !(is_operand(tokens[operand]) || tokens[operand].text == "(")) {
            continue;
        }
        auto [code, message] =
            text == "*" ? std::pair{"cpp1-prefix-dereference",
                                    "prefix '*ptr' dereference is not valid Cpp2; use postfix "
                                    "'ptr*' instead"}
            : text == "&" ? std::pair{"cpp1-prefix-address-of",
                                      "prefix '&var' address-of is not valid Cpp2; use postfix "
                                      "'var&' instead"}
            : text == "~" ? std::pair{"cpp1-prefix-complement",
                                      "prefix '~var' is not valid Cpp2; use postfix 'var~' "
                                      "instead"}
            : std::pair{"cpp1-prefix-increment", ""};
        diagnostics.push_back({
            code, diagnostic_severity::error, tokens[index].span,
            *message ? std::string{message}
                     : "prefix '" + text + "var' is not valid Cpp2; use postfix 'var" + text +
                           "' instead"});
    }
}

// `x is T as U` and `x is T is U` need parentheses.
auto validate_is_as_chains(const token_list& tokens, const token_roles& roles,
                           std::vector<diagnostic>& diagnostics) -> void {
    struct chain final { bool is = false; bool as = false; };
    auto chains = std::vector<chain>{{}};
    for (auto index = std::size_t{0}; index < tokens.size(); ++index) {
        const auto& text = tokens[index].text;
        // A brace also ends the enclosing expression.
        if (text == "{") chains.back() = {};
        if (roles.template_close[index] || text == "(" || text == "[" || text == "{") {
            chains.emplace_back();
            continue;
        }
        const auto closes = text == ")" || text == "]" || text == "}"
            ? std::size_t{1} : roles.closed_lists[index];
        if (closes != 0) {
            for (auto count = closes; count != 0 && chains.size() > 1; --count) chains.pop_back();
            if (text == "}") chains.back() = {};
            continue;
        }
        auto& current = chains.back();
        if (roles.is_as[index]) {
            if (current.is && text == "is") {
                diagnostics.push_back({"repeated-is", diagnostic_severity::error,
                                       tokens[index].span, "repeated 'is' are not allowed"});
            }
            else if ((current.is && text == "as") || (current.as && text == "is")) {
                diagnostics.push_back({"mixed-is-as", diagnostic_severity::error,
                                       tokens[index].span,
                                       "mixed 'is' and 'as' are not allowed; add parentheses"});
            }
            (text == "is" ? current.is : current.as) = true;
        }
        // Any other operator or separator ends the chain.
        else if (tokens[index].kind == token_kind::punctuation && !roles.in_type[index] &&
                 text != "::" && text != "." && text != ".." && text != "...") {
            current = {};
        }
    }
}

// `@` names a metafunction at the start of a declaration's type: after the
// `:` or an earlier metafunction, as in `x: @a @b<"y"> <T> type`. One after
// a template parameter list is left to the parser to diagnose.
auto validate_metafunction_markers(const token_list& tokens, const token_roles& roles,
                                   std::vector<diagnostic>& diagnostics) -> void {
    auto opens = std::vector<std::size_t>(tokens.size());
    // The `>` closing a template parameter list `: <...>`.
    auto closes_parameters = std::vector<bool>(tokens.size());
    for (auto index = std::size_t{0}; index < tokens.size(); ++index) {
        if (roles.template_close[index]) opens[*roles.template_close[index]] = index;
        if (tokens[index].text == "<" && index != 0 && tokens[index - 1].text == ":") {
            const auto close = parser_detail::angle_close(tokens, index, tokens.size());
            if (close) closes_parameters[*close] = true;
        }
    }
    for (auto index = std::size_t{0}; index < tokens.size(); ++index) {
        if (tokens[index].text != "@") continue;
        auto previous = index;
        if (previous != 0 && roles.closed_lists[previous - 1] != 0) previous = opens[previous - 1];
        const auto follows_metafunction = previous >= 2 &&
            tokens[previous - 1].kind == token_kind::identifier &&
            tokens[previous - 2].text == "@";
        if ((index != 0 && (tokens[index - 1].text == ":" || closes_parameters[index - 1])) ||
            follows_metafunction) {
            continue;
        }
        diagnostics.push_back({"unexpected-metafunction", diagnostic_severity::error,
                               tokens[index].span,
                               "unexpected text '@' - a metafunction '@name' is written "
                               "after a declaration's ':'"});
    }
}

// The end of [position, tokens.size()) at the first top-level token in
// `stops`, or where a closing delimiter leaves the range.
auto top_level_end(const token_list& tokens, std::size_t position,
                   std::initializer_list<std::string_view> stops) -> std::size_t {
    return parser_detail::find_top_level(tokens, position, tokens.size(),
        parser_detail::angle_rule::expression, [&](std::size_t at) {
            return std::find(stops.begin(), stops.end(), tokens[at].text) != stops.end();
        });
}

// `throw;` rethrows; otherwise `throw` takes one parenthesized value, `throw(value)`.
auto validate_throw(const token_list& tokens, std::vector<diagnostic>& diagnostics) -> void {
    const auto add = [&](const token& at, std::string code, std::string message) {
        diagnostics.push_back({
            std::move(code), diagnostic_severity::error, at.span, std::move(message)});
    };
    for (auto position = std::size_t{0}; position + 1 < tokens.size(); ++position) {
        if (tokens[position].text != "throw") continue;
        const auto& next = tokens[position + 1].text;
        if (next == ";" || next == "throws") continue;
        if (next != "(") {
            add(tokens[position], "throw-requires-parentheses",
                "Cpp2 requires parentheses around a thrown value: use 'throw(value)'");
            continue;
        }
        auto values = 0;
        for (auto index = position + 2; index < tokens.size() && tokens[index].text != ")";) {
            ++values;
            index = top_level_end(tokens, index, {","});
            if (index == tokens.size() || tokens[index].text != ",") break;
            ++index;
        }
        if (values == 0) {
            add(tokens[position], "invalid-throw",
                "'throw()' has no value: use 'throw(value)', or 'throw;' to rethrow the "
                "exception being handled");
        }
        else if (values > 1) {
            add(tokens[position], "invalid-throw", "'throw' takes one value: use 'throw(value)'");
        }
        else if (tokens[position + 2].text == "(") {
            // A list `(a, b)` lowers to a braced list, which has no type to throw.
            const auto close = parser_detail::bracket_close(tokens, position + 2, tokens.size());
            if (close && *close + 1 < tokens.size() && tokens[*close + 1].text == ")" &&
                top_level_end(tokens, position + 3, {","}) < *close) {
                add(tokens[position + 2], "invalid-throw",
                    "a list has no type to throw: name it, as in 'throw(T(a, b))'");
            }
        }
    }
}

// What an inspect alternative matches: anything (`_` or a tuple pattern
// without a guard), and whether it is `_` without a guard, after which no
// alternative is tested.
struct alternative_match final {
    bool anything = false;
    bool wildcard = false;
};

// The `patterns` experiment's alternative in tokens [begin, end): what it
// matches, or why it is invalid.
auto pattern_alternative(const token_list& tokens, std::size_t begin, std::size_t end,
                         const experiment_set& experiments)
    -> std::variant<alternative_match, std::string> {
    auto text = std::string{};
    for (auto index = begin; index < end && index < tokens.size(); ++index) {
        text += (text.empty() ? "" : " ") + tokens[index].text;
    }
    const auto parsed = parse_inspect_alternative(text, experiments.enabled("try-operator"),
                                                  experiments.enabled("comprehension"));
    if (!parsed) {
        return std::string{"expected 'is pattern [(names)] [| pattern]... [as name] [if guard] "
                           "= value;' in an inspect alternative"};
    }
    const auto& parsed_tokens = parsed->tokens;
    const auto parts = inspect_alternative_of(parsed_tokens, parsed->root);
    const auto tuple = parts.destructuring == &parsed->root.children.front();
    if (std::any_of(parts.patterns.begin(), parts.patterns.end(), [&](const expression* item) {
            return is_wildcard_pattern(parsed_tokens, *item);
        }) && (parts.patterns.size() > 1 || parts.destructuring != nullptr)) {
        return std::string{"'_' matches anything, so it takes no destructuring or '|' patterns"};
    }
    // A tuple pattern among the `|` patterns, or a destructuring with any.
    if (std::any_of(parts.patterns.begin(), parts.patterns.end(),
                    [](const expression* item) { return is_tuple_pattern(*item); }) ||
        (parts.destructuring != nullptr && parts.patterns.size() > (tuple ? 0U : 1U))) {
        return std::string{"a destructuring cannot be combined with '|' patterns"};
    }
    if (parts.destructuring == nullptr) {
        return alternative_match{parts.matches_anything(),
                                 parts.wildcard && parts.guard == nullptr};
    }
    auto names = std::vector<std::string_view>{};
    if (parts.binding != nullptr) names.push_back(parsed_tokens[parts.binding->first].text);
    for (const auto& item : parts.destructuring->children) {
        if (item.kind != expression_kind::name || item.last != item.first + 1) {
            return std::string{"a destructuring lists names and '_', as in 'is T(a, _)'"};
        }
        const auto& name = parsed_tokens[item.first].text;
        if (name == "_") continue;
        if (std::find(names.begin(), names.end(), name) != names.end()) {
            return "'" + name + "' is bound twice in an inspect alternative";
        }
        names.push_back(name);
    }
    if (parts.destructuring->children.empty()) {
        return std::string{"a destructuring lists one or more names and '_'"};
    }
    return alternative_match{parts.matches_anything(), false};
}

// cppfront accepts `inspect` only as an expression with a result type whose
// alternatives are `is` alternatives with expression values, including an
// `is _` wildcard. The `patterns` experiment's forms are rejected without it;
// with it, each alternative must parse as one, and an alternative that
// matches anything, `_` or a tuple pattern without a guard, is the wildcard.
// `check_inspect_patterns` removes the wildcard error from an inspect that
// it proves exhaustive. An alternative after an unguarded `is _` can never be
// chosen (`unreachable-alternative`).
auto validate_inspect(const token_list& tokens, const experiment_set& experiments,
                      std::vector<diagnostic>& diagnostics) -> void {
    const auto patterns = experiments.enabled("patterns");
    const auto add = [&](const token& at, std::string message) {
        diagnostics.push_back({
            "invalid-inspect", diagnostic_severity::error, at.span, std::move(message)});
    };
    const auto text = [&](std::size_t position) -> std::string_view {
        return position < tokens.size() ? std::string_view{tokens[position].text} : "";
    };
    for (auto position = std::size_t{0}; position < tokens.size(); ++position) {
        const auto& start = tokens[position];
        if (!is_word(start, "inspect")) continue;
        const auto previous = position == 0 ? ";" : text(position - 1);
        if (previous == ";" || previous == "{" || previous == "}") {
            add(start, "(temporary alpha limitation) cppfront is still learning 'inspect' - "
                       "only inspect expressions are currently supported");
            continue;
        }
        auto index = top_level_end(tokens, position + 1, {"->", "{", ";"});
        if (text(index) == "{") {
            add(start, "an inspect expression must have an explicit '-> result_type'");
            continue;
        }
        if (text(index) != "->") continue;
        const auto result = index + 1;
        index = top_level_end(tokens, result, {"{", ";"});
        if (text(index) != "{") continue;
        // The value of each alternative converts to the result type, so it
        // is neither deduced nor `void`.
        auto core = result;
        while (core < index && (text(core) == "*" || text(core) == "const")) ++core;
        if (text(core) == "_" || text(core) == "auto" ||
            (index == result + 1 && text(result) == "void")) {
            add(tokens[core], text(core) == "void"
                ? "an inspect expression has a value, so its result type cannot be 'void'"
                : "an inspect expression's result type cannot be deduced - write the type "
                  "after '->'");
            continue;
        }
        const auto open = index++;
        auto alternatives = 0;
        auto wildcard = false;
        // An earlier alternative is an unguarded `is _`; the first one after it
        // is reported.
        auto ended = false;
        auto reported = false;
        auto valid = true;
        while (valid && index < tokens.size() && text(index) != "}") {
            const auto& keyword = tokens[index];
            const auto pattern = index + 1;
            if (ended && !std::exchange(reported, true)) {
                diagnostics.push_back({
                    "unreachable-alternative", diagnostic_severity::error, keyword.span,
                    "this alternative follows 'is _', which matches anything, so it can "
                    "never be chosen"});
            }
            index = top_level_end(tokens, pattern, {"=", ";", "{", "}"});
            if (!is_word(keyword, "is") && !is_word(keyword, "as")) {
                add(keyword, "invalid alternative in inspect");
                valid = false;
            }
            else if (text(index) != "=") {
                add(keyword, "expected = at start of inspect alternative body");
                valid = false;
            }
            else if (keyword.text == "as") {
                add(keyword, "(temporary alpha limitation) cppfront is still learning 'inspect' - "
                             "only simple 'is' alternatives are currently supported");
                valid = false;
            }
            else if (text(index + 1) == "{") {
                add(tokens[index + 1], "an inspect expression alternative must be just an "
                    "expression (not a braced block) that will be used as the value of the "
                    "inspect expression");
                valid = false;
            }
            else if (!patterns) {
                // A value pattern and a `decltype` operand are parenthesized;
                // a type pattern continues only with the experiment's forms.
                const auto operand = text(pattern) == "(" ? pattern
                    : (text(pattern) == "decltype" || text(pattern) == "type_of") &&
                            text(pattern + 1) == "(" ? pattern + 1 : index;
                auto at = operand < index ? top_level_end(tokens, operand + 1, {")"}) + 1
                                          : pattern + 1;
                at = top_level_end(tokens, at, {"(", "|", "as", "if"});
                if (at < index) {
                    add(tokens[at], "destructuring, alternatives, bindings and guards in an "
                        "inspect alternative require '-experiment=patterns'");
                    valid = false;
                }
            }
            const auto end = top_level_end(tokens, index + 1, {";"});
            // A value ends at its `;`; one that runs into the close or into
            // the next alternative's `is pattern =` is missing it.
            auto next = std::optional<std::size_t>{};
            if (valid && text(end) == ";") {
                const auto equals = top_level_end(tokens, index + 1, {"=", ";"});
                for (auto at = top_level_end(tokens, index + 1, {"is", "as"});
                     text(equals) == "=" && at < equals;
                     at = top_level_end(tokens, at + 1, {"is", "as"})) {
                    next = at;
                }
            }
            if (valid && (text(end) != ";" || next)) {
                add(tokens[(next ? *next : end) - 1],
                    "expected ';' at the end of the inspect alternative");
                valid = false;
            }
            if (valid && patterns) {
                const auto parts = pattern_alternative(tokens, pattern - 1, end + 1, experiments);
                if (std::holds_alternative<std::string>(parts)) {
                    add(keyword, std::get<std::string>(parts));
                    valid = false;
                }
                else {
                    const auto match = std::get<alternative_match>(parts);
                    wildcard = wildcard || match.anything;
                    ended = ended || match.wildcard;
                }
            }
            ++alternatives;
            if (!patterns && index == pattern + 1 && text(pattern) == "_") {
                wildcard = true;
                ended = true;
            }
            index = end + 1;
        }
        if (!valid || text(index) != "}") continue;
        if (alternatives == 0) {
            add(tokens[open], "inspect body cannot be empty - add at least one alternative");
        }
        else if (!wildcard) {
            add(start, std::string{inspect_wildcard_message});
        }
    }
}

}

// A name before `:` or `:=` is declared, so is not qualified; a conversion
// function `operator std::string:` names its type.
auto validate_declaration_names(const token_list& tokens,
                                std::vector<diagnostic>& diagnostics) -> void {
    for (auto position = std::size_t{0}; position + 1 < tokens.size(); ++position) {
        const auto& name = tokens[position];
        const auto& next = tokens[position + 1].text;
        if (name.kind != token_kind::identifier || name.text.find("::") == std::string::npos ||
            name.text.ends_with("::") || (next != ":" && next != ":=") ||
            (position != 0 && tokens[position - 1].text == "operator")) {
            continue;
        }
        diagnostics.push_back({
            "qualified-declaration-name", diagnostic_severity::error, name.span,
            "a declaration declares an unqualified name, not '" + name.text + "'"});
    }
}

// `post(r: r >= 0)`: a contract is a condition, not a declaration.
auto validate_contract_condition(const token_list& tokens,
                                 std::vector<diagnostic>& diagnostics) -> void {
    for (auto position = std::size_t{0}; position + 1 < tokens.size(); ++position) {
        const auto& word = tokens[position].text;
        if (word != "pre" && word != "post") continue;
        auto open = position + 1;
        if (tokens[open].text == "<") {
            open = parser_detail::angle_close(tokens, open, tokens.size()).value_or(open) + 1;
        }
        if (open >= tokens.size() || tokens[open].text != "(" ||
            !parser_detail::bracket_close(tokens, open, tokens.size())) {
            continue;
        }
        const auto at = top_level_end(tokens, open + 1, {":", ")"});
        if (at >= tokens.size() || tokens[at].text != ":" ||
            tokens[at - 1].kind != token_kind::identifier) {
            continue;
        }
        diagnostics.push_back({
            "contract-declaration", diagnostic_severity::error, tokens[at].span,
            "expected ')' at the end of the contract"});
    }
}

auto validate_nesting_depth(const token_list& tokens) -> std::optional<diagnostic> {
    // Each bracket and template argument list adds one level from its open
    // through its close. `closes` holds the closes of the open levels.
    const auto lists = parser_detail::template_argument_closes(tokens);
    auto closes = std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<>>{};
    auto depth = 0;
    for (auto position = std::size_t{0}; position < tokens.size(); ++position) {
        for (; !closes.empty() && closes.top() < position; closes.pop()) --depth;
        const auto& text = tokens[position].text;
        if (text == "(" || text == "[" || text == "{") ++depth;
        else if (text == ")" || text == "]" || text == "}") closes.push(position);
        else if (text == "<" && position != 0 &&
                 tokens[position - 1].kind == token_kind::identifier) {
            if (const auto close = lists[position]) {
                ++depth;
                closes.push(*close);
            }
        }
        if (depth > max_nesting_depth) {
            return diagnostic{
                "nesting-too-deep", diagnostic_severity::error, tokens[position].span,
                "brackets and template argument lists are nested more than " +
                    std::to_string(max_nesting_depth) + " levels deep"};
        }
    }
    return std::nullopt;
}

auto validate_preprocessor(const source_text& source) -> std::vector<diagnostic> {
    auto diagnostics = std::vector<diagnostic>{};
    // One entry per open #if, after the file-level entry; true once its #else is seen.
    auto seen_else = std::vector<bool>{false};
    const auto& lines = source.lines();
    for (auto index = std::size_t{0}; index < lines.size(); ++index) {
        if (lines[index].category != source_category::preprocessor) continue;
        auto directive = std::string_view{lines[index].text};
        const auto hash = directive.find('#');
        directive.remove_prefix(hash + 1);
        directive.remove_prefix(std::min(directive.find_first_not_of(" \t"), directive.size()));
        directive = directive.substr(0, directive.find_first_of(" \t\r("));
        const auto add = [&](std::string message) {
            const auto line = static_cast<std::uint32_t>(index + 1);
            const auto column = static_cast<std::uint32_t>(hash + 1);
            diagnostics.push_back({"unmatched-preprocessor", diagnostic_severity::error,
                                   {{line, column}, {line, column}}, std::move(message)});
        };
        if (directive == "if" || directive == "ifdef" || directive == "ifndef") {
            seen_else.push_back(false);
        }
        else if (directive == "else") {
            if (seen_else.size() < 2) add("#else does not match a prior #if");
            if (seen_else.back()) add("#else already encountered for this #if");
            seen_else.back() = true;
        }
        else if (directive == "endif") {
            if (seen_else.size() < 2) add("#endif does not match a prior #if");
            else seen_else.pop_back();
        }
    }
    return diagnostics;
}

auto validate_cpp1_braces(const source_text& source) -> std::vector<diagnostic> {
    auto diagnostics = std::vector<diagnostic>{};
    auto depth = 0;
    auto scanner = detail::line_scanner{};
    const auto& lines = source.lines();
    for (auto index = std::size_t{0}; index < lines.size(); ++index) {
        const auto& text = lines[index].text;
        const auto cpp1 = lines[index].category == source_category::cpp1 &&
            !lines[index].in_later_branch;
        scanner.scan(text, [&](std::size_t column, std::size_t, detail::lexeme_kind kind) {
            if (!cpp1 || kind != detail::lexeme_kind::none) return;
            if (text[column] == '{') ++depth;
            else if (text[column] == '}' && --depth < 0) {
                const auto line = static_cast<std::uint32_t>(index + 1);
                const auto at = static_cast<std::uint32_t>(column + 1);
                diagnostics.push_back({"unmatched-close-brace", diagnostic_severity::error,
                                       {{line, at}, {line, at + 1}},
                                       "closing } does not match a prior {"});
                depth = 0;
            }
        });
    }
    return diagnostics;
}

auto validate_cpp2(const token_list& tokens, const experiment_set& experiments)
    -> std::vector<diagnostic> {
    auto diagnostics = std::vector<diagnostic>{};
    // The parser reports nesting that is too deep, and nothing else.
    if (validate_nesting_depth(tokens)) return diagnostics;
    const auto try_operator = experiments.enabled("try-operator");
    const auto roles = roles_of(tokens, try_operator);
    validate_prefix_operators(tokens, roles, diagnostics);
    validate_is_as_chains(tokens, roles, diagnostics);
    validate_metafunction_markers(tokens, roles, diagnostics);
    if (!try_operator) validate_conditional_operator(tokens, diagnostics);
    validate_pointer_to_member(tokens, diagnostics);
    validate_incomplete_statement(tokens, diagnostics);
    validate_contract_condition(tokens, diagnostics);
    validate_declaration_names(tokens, diagnostics);
    validate_captures(tokens, diagnostics);
    validate_cpp1_words(tokens, diagnostics);
    validate_legacy_types(tokens, diagnostics);
    validate_throw(tokens, diagnostics);
    validate_template_arguments(tokens, roles.template_close, diagnostics);
    validate_requires_parameters(tokens, diagnostics);
    validate_function_type_colon(tokens, diagnostics);
    validate_typed_value_initializer(tokens, diagnostics);
    validate_inspect(tokens, experiments, diagnostics);
    std::sort(diagnostics.begin(), diagnostics.end(), compare_by_source);
    return diagnostics;
}

}
