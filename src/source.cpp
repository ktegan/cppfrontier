#include <cppfrontier/source.hpp>

#include <cppfrontier/expression.hpp>

#include "brackets.hpp"
#include "cpp1_text.hpp"

#include <algorithm>
#include <cctype>

namespace cppfrontier {

namespace {

auto match_line(std::string_view text, std::size_t& offset) -> std::string_view {
    const auto end = text.find('\n', offset);
    const auto found = end != std::string_view::npos;
    const auto line = text.substr(offset, (found ? end : text.size()) - offset);
    offset = found ? end + 1 : text.size();
    return line;
}

auto starts_with(std::string_view text, std::string_view prefix) -> bool {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

auto starts_with_two(std::string_view text, char one, char two) -> bool {
    return text.size() >= 2 && text.front() == one && text[1] == two;
}

auto trimmed(std::string_view text) -> std::string_view {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
        text.remove_prefix(1);
    }
    return text;
}

// `(a, b) :=` or `(a, b):`, a destructuring declaration (experiment
// `destructuring`), which the parser rejects outside a function body.
auto begins_destructuring_declaration(std::string_view text) -> bool {
    if (!starts_with(text, "(")) return false;
    const auto tokens = parse_expression_text(text).tokens;
    return detail::destructuring_names_end(tokens, 0, [](const expression_token& value) {
        return value.kind == expression_token_kind::identifier;
    }).has_value();
}

auto is_identifier_character(char value) -> bool {
    return std::isalnum(static_cast<unsigned char>(value)) || value == '_';
}

// `using a::b;` or `using a::_;` on one line: a qualified name, which Cpp2
// and Cpp1 read alike, then `;`.
auto is_cpp2_using(std::string_view text) -> bool {
    text = trimmed(text.substr(6));
    if (starts_with(text, "::")) text = trimmed(text.substr(2));
    for (;;) {
        auto length = std::size_t{0};
        while (length < text.size() && is_identifier_character(text[length])) ++length;
        if (length == 0 || std::isdigit(static_cast<unsigned char>(text[0]))) return false;
        text = trimmed(text.substr(length));
        if (starts_with(text, ";")) return true;
        if (!starts_with(text, "::")) return false;
        text = trimmed(text.substr(2));
    }
}

// A `using` line is Cpp2 only in the form `using a::b;`; other forms, such
// as `using a = b;`, `using namespace n;` and `using enum E;`, are Cpp1.
auto begins_cpp2_declaration(std::string_view text, const experiment_set& experiments) -> bool {
    text = trimmed(text);
    if (experiments.enabled("destructuring") && begins_destructuring_declaration(text)) return true;
    // A `defer` statement or an `unsafe` block, which the parser rejects
    // outside a function body (experiments `defer`, `unsafe`).
    const auto word = text.substr(0, std::min(text.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"), text.size()));
    if (word == "defer" && experiments.enabled("defer")) return true;
    if (word == "unsafe" && experiments.enabled("unsafe")) {
        const auto rest = trimmed(text.substr(word.size()));
        if (rest.empty() || rest.starts_with('{')) return true;
    }
    if (starts_with(text, "using ")) return is_cpp2_using(text);
    if (starts_with(text, "public ") || starts_with(text, "protected ") ||
        starts_with(text, "private ")) {
        text.remove_prefix(text.find(' ') + 1);
        text = trimmed(text);
    }
    if (text.empty() || !(std::isalpha(static_cast<unsigned char>(text.front())) || text.front() == '_')) {
        return false;
    }
    auto position = std::size_t{0};
    while (position < text.size() &&
           (std::isalnum(static_cast<unsigned char>(text[position])) || text[position] == '_')) {
        ++position;
    }
    // Cpp1 labels, which no Cpp2 declaration can be named.
    const auto name = text.substr(0, position);
    if (name == "public" || name == "protected" || name == "private" || name == "default") {
        return false;
    }
    if (name == "operator") {
        while (position < text.size() &&
               std::string_view{"=!<>+-*/%^&|~()[]"}.find(text[position]) != std::string_view::npos) {
            ++position;
        }
        // `operator T:` is a Cpp2-style conversion function, and `operator "" _x:` a
        // literal operator, which the parser diagnoses.
        if (position == name.size()) {
            const auto is_name = [&](char c) {
                return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ' ' ||
                    c == '\t';
            };
            while (position < text.size() && (text[position] == ' ' || text[position] == '\t')) {
                ++position;
            }
            if (text.substr(position, 2) == "\"\"") position += 2;
            while (position < text.size() &&
                   (is_name(text[position]) || text.substr(position, 2) == "::")) {
                position += text[position] == ':' ? 2 : 1;
            }
            // `operator new[]`
            if (text.substr(position, 2) == "[]") position += 2;
        }
    }
    while (position < text.size() && (text[position] == ' ' || text[position] == '\t')) ++position;
    return position < text.size() && text[position] == ':' &&
        (position + 1 == text.size() || text[position + 1] != ':');
}

// The name of the directive on a line that begins with `#`.
auto directive_name(std::string_view line) -> std::string_view {
    line = trimmed(line.substr(1));
    auto length = std::size_t{0};
    while (length < line.size() && is_identifier_character(line[length])) ++length;
    return line.substr(0, length);
}

// A backslash at the end of a Cpp1 line splices the next line onto it.
auto ends_with_splice(std::string_view line) -> bool {
    const auto end = line.find_last_not_of(" \t\r");
    return end != std::string_view::npos && line[end] == '\\';
}

struct scanned_line final {
    // Change in the depth of `{`, `(` and `[`, and in that of `{` alone.
    int depth_delta = 0;
    int brace_delta = 0;
    char last_code = '\0';
    std::size_t first_code = std::string_view::npos;

    [[nodiscard]] auto has_code() const noexcept -> bool {
        return first_code != std::string_view::npos;
    }
};

auto scan_line(std::string_view line, detail::line_scanner& state) -> scanned_line {
    auto result = scanned_line{};
    state.scan(line, [&](std::size_t begin, std::size_t end, detail::lexeme_kind kind) {
        if (!result.has_code()) result.first_code = begin;
        const auto character = line[begin];
        if (kind == detail::lexeme_kind::number) result.last_code = line[end - 1];
        else if (kind == detail::lexeme_kind::character) result.last_code = '\'';
        else if (kind != detail::lexeme_kind::none) result.last_code = '"';
        else result.last_code = character;
        if (kind != detail::lexeme_kind::none) return;
        if (character == '{' || character == '(' || character == '[') ++result.depth_delta;
        else if (character == '}' || character == ')' || character == ']') --result.depth_delta;
        if (character == '{') ++result.brace_delta;
        else if (character == '}') --result.brace_delta;
    });
    return result;
}

// `text` without a leading UTF-8 byte order mark.
auto without_byte_order_mark(std::string text) -> std::string {
    if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
    return text;
}

} // namespace

auto categorize_lines(std::string_view text, const experiment_set& experiments)
    -> std::vector<source_line> {
    auto lines = std::vector<source_line>{};
    lines.reserve(32);
    auto offset = std::size_t{0};
    auto state = detail::line_scanner{};
    auto in_cpp2 = false;
    // A stray `)` or `]` does not close the braces of a Cpp2 declaration.
    auto cpp2_depth = 0;
    auto cpp2_braces = 0;
    auto cpp1_depth = 0;
    auto spliced = false;
    // A block comment that a Cpp2 line opened is still open.
    auto cpp2_comment = false;
    // The last Cpp1 code line ends with an operator, so the next continues it.
    auto cpp1_continues = false;
    // One entry per open `#if`: whether its `#elif` or `#else` has been seen.
    auto later_branches = std::vector<bool>{};

    while (offset < text.size()) {
        const auto line = match_line(text, offset);
        const auto view = trimmed(line);
        const auto raw_string_at_line_start = !state.raw_terminator.empty();
        const auto comment_at_line_start = state.in_block_comment;
        const auto scanned = scan_line(line, state);
        auto category = source_category::cpp1;

        const auto spliced_from_previous = spliced && !raw_string_at_line_start;
        if (raw_string_at_line_start) {
            category = source_category::raw_string;
        }
        else if (spliced) {
            category = lines.back().category;
        }
        else if (cpp2_comment) {
            category = source_category::cpp2;
            in_cpp2 = in_cpp2 || scanned.has_code();
        }
        else if (view.empty()) {
            category = source_category::empty;
        }
        else if (!scanned.has_code()) {
            category = source_category::comment;
        }
        else if (starts_with_two(view, '/', '/')) {
            category = source_category::comment;
        }
        else if (starts_with(view, "#import") || starts_with(view, "import ")) {
            category = source_category::import;
        }
        else if (view.front() == '#') {
            category = source_category::preprocessor;
            const auto directive = directive_name(view);
            if (directive == "if" || directive == "ifdef" || directive == "ifndef") {
                later_branches.push_back(false);
            }
            else if (!later_branches.empty() && (directive == "else" || directive.starts_with("elif"))) {
                later_branches.back() = true;
            }
            else if (!later_branches.empty() && directive == "endif") {
                later_branches.pop_back();
            }
        }
        else {
            // Cpp2 is not nested inside Cpp1 braces, parentheses or brackets,
            // and does not begin inside a comment.
            if (!in_cpp2 && cpp1_depth == 0 && !comment_at_line_start && !cpp1_continues &&
                begins_cpp2_declaration(line.substr(scanned.first_code), experiments)) {
                in_cpp2 = true;
            }
            category = in_cpp2 ? source_category::cpp2 : source_category::cpp1;
        }
        const auto in_later_branch =
            std::find(later_branches.begin(), later_branches.end(), true) != later_branches.end();
        if (category == source_category::cpp1 && !in_later_branch) {
            cpp1_depth = std::max(0, cpp1_depth + scanned.depth_delta);
        }
        cpp2_comment = category == source_category::cpp2 && state.in_block_comment;
        if (category == source_category::cpp1 && scanned.has_code()) {
            cpp1_continues = std::string_view{"?:=,+-*/%&|^<>!~"}.find(scanned.last_code) !=
                std::string_view::npos;
        }
        else if (category != source_category::empty && category != source_category::comment) {
            cpp1_continues = false;
        }
        spliced = (category == source_category::preprocessor ||
                   category == source_category::cpp1 ||
                   (category == source_category::comment && !in_cpp2)) &&
            ends_with_splice(line);

        if (in_cpp2) {
            cpp2_depth += scanned.depth_delta;
            cpp2_braces += scanned.brace_delta;
            if (cpp2_depth <= 0 && cpp2_braces <= 0 &&
                (scanned.last_code == ';' || scanned.last_code == '}')) {
                in_cpp2 = false;
                cpp2_depth = 0;
                cpp2_braces = 0;
            }
        }
        lines.push_back({std::string{line}, category, in_later_branch, spliced_from_previous});
    }

    if (text.empty()) lines.push_back({});
    return lines;
}

source_text::source_text(std::string text, const experiment_set& experiments)
    : text_{without_byte_order_mark(std::move(text))}, lines_{categorize_lines(text_, experiments)}, line_starts_{0} {
    for (auto offset = text_.find('\n'); offset != std::string::npos;
         offset = text_.find('\n', offset + 1)) {
        line_starts_.push_back(offset + 1);
    }
    for (auto index = std::size_t{0}; index < lines_.size();) {
        if (lines_[index].category != source_category::cpp2) {
            ++index;
            continue;
        }
        const auto begin = index;
        auto end = index + 1;
        auto state = detail::line_scanner{};
        auto cpp2_depth = 0;
        auto cpp2_braces = 0;
        auto last_code = '\0';
        auto scan = [&](std::size_t line) {
            const auto scanned = scan_line(lines_[line].text, state);
            cpp2_depth += scanned.depth_delta;
            cpp2_braces += scanned.brace_delta;
            if (scanned.last_code != '\0') last_code = scanned.last_code;
        };
        scan(begin);
        while (end < lines_.size()) {
            if (lines_[end].category == source_category::cpp2) {
                scan(end);
                ++end;
                continue;
            }
            if (lines_[end].category == source_category::raw_string &&
                (lines_[end - 1].category == source_category::cpp2 ||
                 lines_[end - 1].category == source_category::raw_string)) {
                scan(end);
                ++end;
                continue;
            }
            auto next = end;
            auto saw_comment = false;
            while (next < lines_.size() &&
                   (lines_[next].category == source_category::empty ||
                    lines_[next].category == source_category::comment)) {
                saw_comment = saw_comment || lines_[next].category == source_category::comment;
                ++next;
            }
            // A declaration left open takes the comments that end the file,
            // where an unterminated block comment may have hidden its close.
            if (next == lines_.size() && (cpp2_depth > 0 || cpp2_braces > 0)) {
                end = next;
                break;
            }
            if (next == lines_.size() || lines_[next].category != source_category::cpp2) break;
            const auto ends_declaration = last_code == ';' || last_code == '}';
            for (auto current = end; current < next; ++current) scan(current);
            // A comment line ends the section only between complete declarations.
            if (saw_comment && cpp2_depth <= 0 && cpp2_braces <= 0 && ends_declaration) break;
            end = next;
        }
        sections_.push_back({static_cast<std::uint32_t>(begin + 1), static_cast<std::uint32_t>(end + 1)});
        index = end;
    }
}

source_text source_text::from_bytes(std::string text, const experiment_set& experiments) {
    return source_text{std::move(text), experiments};
}

auto source_text::data() const noexcept -> const std::string& {
    return text_;
}

auto source_text::lines() const noexcept -> const std::vector<source_line>& {
    return lines_;
}

auto source_text::line_starts() const noexcept -> const std::vector<std::size_t>& {
    return line_starts_;
}

auto source_text::text_between(source_position begin, source_position end) const -> std::string {
    if (begin.line == 0 || end.line == 0 || begin.line > lines_.size() || end.line > lines_.size()) {
        return {};
    }
    if (end.line < begin.line || (end.line == begin.line && end.column < begin.column)) return {};
    const auto begin_column = begin.column == 0 ? 1u : begin.column;
    const auto end_column = end.column == 0 ? 1u : end.column;
    if (begin.line == end.line) {
        const auto& line = lines_[begin.line - 1].text;
        const auto left = std::min<std::size_t>(begin_column - 1, line.size());
        const auto right = std::min<std::size_t>(end_column - 1, line.size());
        return line.substr(left, right - left);
    }
    auto result = std::string{};
    for (auto line = begin.line; line <= end.line; ++line) {
        const auto& value = lines_[line - 1].text;
        const auto left = line == begin.line ? std::min<std::size_t>(begin_column - 1, value.size()) : 0;
        const auto right = line == end.line ? std::min<std::size_t>(end_column - 1, value.size()) : value.size();
        if (line != begin.line) result += '\n';
        result.append(value, left, right - left);
    }
    return result;
}

auto source_text::has_cpp2() const noexcept -> bool {
    return !sections_.empty();
}

auto source_text::cpp2_sections() const -> std::vector<section_range> {
    return sections_;
}

auto category_name(source_category category) noexcept -> std::string_view {
    switch (category) {
    case source_category::empty: return "empty";
    case source_category::comment: return "comment";
    case source_category::import: return "import";
    case source_category::preprocessor: return "preprocessor";
    case source_category::cpp1: return "cpp1";
    case source_category::cpp2: return "cpp2";
    case source_category::raw_string: return "raw_string";
    }
    return "cpp1";
}

auto continues_line(const source_text& source, std::size_t index) -> bool {
    return index < source.lines().size() && source.lines()[index].spliced;
}

// The leading lines end at the first code line, or before an `#if` still
// open there, so that what follows them is not conditional. An `#if` without
// `#elif` or `#else` whose `#endif` no code follows, such as a header guard,
// may stay open.
auto code_begin_index(const source_text& source) -> std::size_t {
    const auto& lines = source.lines();
    const auto is_code = [&](std::size_t index) {
        return lines[index].category == source_category::cpp1 ||
            lines[index].category == source_category::cpp2 ||
            lines[index].category == source_category::raw_string;
    };
    const auto directive = [&](std::size_t index) -> std::string_view {
        if (lines[index].category != source_category::preprocessor ||
            continues_line(source, index)) {
            return {};
        }
        return directive_name(trimmed(lines[index].text));
    };
    const auto opens = [](std::string_view name) {
        return name == "if" || name == "ifdef" || name == "ifndef";
    };
    auto first = std::size_t{0};
    while (first < lines.size() && !is_code(first)) ++first;
    auto open = std::vector<std::size_t>{};
    for (auto index = std::size_t{0}; index < first; ++index) {
        const auto name = directive(index);
        if (opens(name)) open.push_back(index);
        else if (name == "endif" && !open.empty()) open.pop_back();
    }
    // Whether each conditional open at `first` encloses the rest of the file.
    auto encloses_rest = std::vector<bool>(open.size(), true);
    auto level = open.size();
    auto nested = 0;
    for (auto index = first; index < lines.size() && level > 0; ++index) {
        const auto name = directive(index);
        if (opens(name)) ++nested;
        else if (nested > 0) nested -= name == "endif" ? 1 : 0;
        else if (name == "else" || name.starts_with("elif")) encloses_rest[level - 1] = false;
        else if (name == "endif") {
            --level;
            for (auto after = index + 1; after < lines.size() && encloses_rest[level]; ++after) {
                encloses_rest[level] = !is_code(after);
            }
        }
    }
    const auto partial = std::find(encloses_rest.begin(), encloses_rest.end(), false);
    if (partial == encloses_rest.end()) return first;
    return open[static_cast<std::size_t>(partial - encloses_rest.begin())];
}
}
