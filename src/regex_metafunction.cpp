#include "regex_metafunction.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

// A port of upstream's `@regex` parser and generator (reflect.h2). The parser
// builds a token tree for the pattern; generation walks the tree in
// continuation-passing style, emitting one `func_N` function object per
// matching step. The generated Cpp2 uses the runtime in cpp2regex.h.

namespace cppfrontier {

namespace {

enum expression_flags : unsigned {
    flag_case_insensitive = 1,
    flag_multiple_lines = 2,
    flag_single_line = 4,
    flag_no_group_captures = 8,
    flag_perl_code_syntax = 16,
    flag_perl_code_syntax_in_classes = 32,
};

// `cpp2::regex::range_flags` values.
constexpr int range_not_greedy = 1;
constexpr int range_greedy = 2;
constexpr int range_possessive = 3;

auto bool_text(bool value) -> std::string { return value ? "true" : "false"; }

auto is_space(char c) -> bool {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

auto trim_copy(std::string_view text) -> std::string {
    const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!text.empty() && space(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && space(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return std::string{text};
}

// `std::stoi` semantics, as upstream's `string_util::string_to_int`.
auto string_to_int(const std::string& text, int& value, int base = 10) -> bool {
    try {
        auto end = std::size_t{0};
        value = std::stoi(text, &end, base);
        return end == text.size();  // `2B(` is not a number
    }
    catch (const std::invalid_argument&) {
        return false;
    }
    catch (const std::out_of_range&) {
        return false;
    }
}

auto int_to_string(int value, bool octal) -> std::string {
    auto stream = std::ostringstream{};
    stream << (octal ? std::oct : std::hex) << value;
    return stream.str();
}

auto join(const std::vector<std::string>& list) -> std::string {
    auto result = std::string{};
    for (const auto& item : list) result += (result.empty() ? "" : ", ") + item;
    return result;
}

// A character in a string or character literal. `$` is escaped so that `)$`
// does not interpolate.
auto escape_char(char c, char quote) -> std::string {
    switch (c) {
    case '\\': return "\\\\";
    case '\a': return "\\a";
    case '\f': return "\\f";
    case '\n': return "\\n";
    case '\r': return "\\r";
    case '\t': return "\\t";
    default: break;
    }
    if (c == quote) return std::string{"\\"} + c;
    const auto code = static_cast<unsigned char>(c);
    if (code < 0x20 || code == 0x7f || c == '$') {
        // Three octal digits cannot run into a following character.
        const char digits[] = {'\\', static_cast<char>('0' + (code >> 6)),
                               static_cast<char>('0' + ((code >> 3) & 7)),
                               static_cast<char>('0' + (code & 7)), '\0'};
        return digits;
    }
    return std::string(1, c);
}

auto string_literal(std::string_view text) -> std::string {
    auto result = std::string{"\""};
    for (const auto c : text) result += escape_char(c, '"');
    return result + "\"";
}

auto char_literal(char c) -> std::string { return "'" + escape_char(c, '\'') + "'"; }

auto raw_string_literal(std::string_view text) -> std::string {
    const auto delimiter = text.find(")\"") == std::string_view::npos ? "" : "cpp2";
    return std::string{"R\""} + delimiter + "(" + std::string{text} + ")" + delimiter + "\"";
}

// Perl matches a lookbehind of at most 255 characters.
constexpr int max_lookbehind = 255;
constexpr int longer_than_lookbehind = max_lookbehind + 1;

auto capped_length(long long length) -> int {
    return static_cast<int>(std::min<long long>(length, longer_than_lookbehind));
}

struct generation_context;
struct regex_token;
using token_ptr = std::shared_ptr<regex_token>;
using token_vec = std::vector<token_ptr>;

struct regex_token {
    std::string string_rep;

    explicit regex_token(std::string text = {}) : string_rep{std::move(text)} {}
    regex_token(const regex_token&) = default;
    auto operator=(const regex_token&) -> regex_token& = default;
    virtual ~regex_token() = default;

    virtual auto generate_code(generation_context& ctx) const -> void = 0;
    // A token for matching backwards, used by lookbehind.
    [[nodiscard]] virtual auto reverse() const -> token_ptr = 0;
    virtual auto add_groups(std::set<int>&) const -> void {}
    // The most characters the token matches, up to `longer_than_lookbehind`.
    [[nodiscard]] virtual auto max_length() const -> int { return 0; }
    [[nodiscard]] auto to_string() const -> const std::string& { return string_rep; }
};

// Generated code for one function at a time; generation of functions interleaves.
struct generation_context {
    struct function_context {
        std::string name;
        std::string code;
        std::string tabs;
        bool saves_groups = false;
    };
    std::vector<function_context> stack = std::vector<function_context>(1);
    int matcher_func = 0;
    int temp_name = 0;
    std::string entry_func;

    static constexpr std::string_view match_parameters = "r.pos, ctx";

    auto add(std::string_view line) -> void {
        auto& current = stack.back();
        current.code += current.tabs;
        current.code += line;
        current.code += '\n';
    }

    // `check` is a call that returns whether the token matches.
    auto add_check(const std::string& check) -> void {
        add("if !cpp2::regex::" + check + " { r.matched = false; break; }");
    }

    // `check` returns a `match_return` and continues with `next_func`.
    auto add_statefull(const std::string& next_func, const std::string& check) -> void {
        end_func_statefull(check);
        start_func_named(next_func.substr(0, next_func.size() - 2));
    }

    auto start_func_named(const std::string& name) -> void {
        stack.push_back({name});
        stack.back().tabs = "        ";
    }

    auto start_func() -> std::string {
        const auto name = gen_func_name();
        start_func_named(name);
        return name + "()";
    }

    // The current function changes groups: when it fails, it restores them as they were on entry.
    auto save_groups() -> void { stack.back().saves_groups = true; }
    auto save_groups_of(const token_ptr& token) -> void {
        auto groups = std::set<int>{};
        token->add_groups(groups);
        if (!groups.empty()) save_groups();
    }

    auto end_func_statefull(const std::string& result) -> void {
        auto& current = stack.back();
        current.code = "  " + current.name + ": @struct<noforward> type = {\n"
            "    operator(): <Iter> (this, cur: Iter, forward ctx, other) "
            "-> cpp2::regex::match_return<Iter> = {\n"
            "      r := ctx..pass(cur);\n" +
            (current.saves_groups
                 ? "      groups_on_entry := cpp2::regex::make_forward_match_context(ctx);\n"
                 : "") +
            "      do {\n" + current.code + "      } while false;\n";
        current.tabs.clear();
        add("      if r.matched {");
        add("        r = " + result + ";");
        add("      }");
        add("      else {");
        add("        r.pos = ctx.end;");
        add("      }");
        if (current.saves_groups) {
            add("      if !r.matched {");
            add("        cpp2::regex::make_forward_match_context(ctx) = groups_on_entry;");
            add("      }");
        }
        add("      _ = ctx;");
        add("      return r;");
        add("    }");
        add("  }");
        finish_context();
    }

    auto generate_func(const token_ptr& token) -> std::string {
        const auto name = start_func();
        token->generate_code(*this);
        end_func_statefull("other(" + std::string{match_parameters} + ")");
        return name;
    }

    auto gen_func_name() -> std::string { return "func_" + std::to_string(matcher_func++); }
    auto next_func_name() -> std::string { return gen_func_name() + "()"; }
    auto gen_temp() -> std::string { return "tmp_" + std::to_string(temp_name++); }

    auto finish_context() -> void {
        auto code = std::move(stack.back().code);
        stack.pop_back();
        stack.front().code += code;
    }

    auto run(const token_ptr& token) -> std::string {
        entry_func = generate_func(token);
        return stack.front().code;
    }
};

// Adds a call to the check `check(r.pos, ctx)`, which matches `width` characters.
struct check_token final : regex_token {
    std::string check;
    int width;

    check_token(std::string text, std::string check_, int width_)
        : regex_token{std::move(text)}, check{std::move(check_)}, width{width_} {}
    auto generate_code(generation_context& ctx) const -> void override {
        ctx.add_check(check + "(" + std::string{ctx.match_parameters} + ")");
    }
    auto max_length() const -> int override { return width; }
    auto reverse() const -> token_ptr override { return std::make_shared<check_token>(*this); }
};

// Code that sets a group, such as `\K`.
struct code_token final : regex_token {
    std::string code;

    code_token(std::string text, std::string code_)
        : regex_token{std::move(text)}, code{std::move(code_)} {}
    auto generate_code(generation_context& ctx) const -> void override {
        ctx.save_groups();
        ctx.add(code);
    }
    auto add_groups(std::set<int>& groups) const -> void override { groups.insert(0); }
    auto reverse() const -> token_ptr override { return std::make_shared<code_token>(*this); }
};

// A token that does not affect matching, such as a comment.
struct empty_token final : regex_token {
    using regex_token::regex_token;
    auto generate_code(generation_context&) const -> void override {}
    auto reverse() const -> token_ptr override { return std::make_shared<empty_token>(*this); }
};

auto concatenated_string(const token_vec& tokens) -> std::string {
    auto result = std::string{};
    for (const auto& token : tokens) result += token->to_string();
    return result;
}

struct list_token final : regex_token {
    token_vec tokens;

    explicit list_token(token_vec tokens_)
        : regex_token{concatenated_string(tokens_)}, tokens{std::move(tokens_)} {}
    auto generate_code(generation_context& ctx) const -> void override {
        for (const auto& token : tokens) token->generate_code(ctx);
    }
    auto add_groups(std::set<int>& groups) const -> void override {
        for (const auto& token : tokens) token->add_groups(groups);
    }
    auto max_length() const -> int override {
        auto length = 0LL;
        for (const auto& token : tokens) length += token->max_length();
        return capped_length(length);
    }
    auto reverse() const -> token_ptr override {
        auto reversed = token_vec{};
        for (auto token = tokens.rbegin(); token != tokens.rend(); ++token) {
            reversed.push_back((*token)->reverse());
        }
        return std::make_shared<list_token>(std::move(reversed));
    }
};

auto alternatives_string(const token_vec& alternatives) -> std::string {
    auto result = std::string{};
    auto separator = std::string{};
    for (const auto& alternative : alternatives) {
        result += separator + alternative->to_string();
        separator = "|";
    }
    return result;
}

// `a|b`: the first alternative that matches is chosen.
struct alternative_token final : regex_token {
    token_vec alternatives;

    explicit alternative_token(token_vec alternatives_)
        : regex_token{alternatives_string(alternatives_)}, alternatives{std::move(alternatives_)} {}
    auto generate_code(generation_context& ctx) const -> void override {
        auto functions = std::string{};
        for (const auto& alternative : alternatives) {
            functions += ", " + ctx.generate_func(alternative);
        }
        const auto next_name = ctx.next_func_name();
        ctx.add_statefull(next_name, "cpp2::regex::alternative_token_matcher<char>::match(" +
            std::string{ctx.match_parameters} + ", other, " + next_name + functions + ")");
    }
    auto add_groups(std::set<int>& groups) const -> void override {
        for (const auto& alternative : alternatives) alternative->add_groups(groups);
    }
    auto max_length() const -> int override {
        auto length = 0;
        for (const auto& alternative : alternatives) length = std::max(length, alternative->max_length());
        return length;
    }
    auto reverse() const -> token_ptr override {
        // Only the alternatives are reversed, not the order in which they are tried.
        auto reversed = token_vec{};
        for (const auto& alternative : alternatives) reversed.push_back(alternative->reverse());
        return std::make_shared<alternative_token>(std::move(reversed));
    }
};

// `(?>...)`
struct atomic_group_token final : regex_token {
    token_ptr inner;

    auto generate_code(generation_context& ctx) const -> void override {
        const auto inner_name = ctx.generate_func(inner);
        const auto next_name = ctx.next_func_name();
        ctx.save_groups_of(inner);
        ctx.add_statefull(next_name, "cpp2::regex::atomic_group_matcher<char>(" +
            std::string{ctx.match_parameters} + ", " + inner_name + ", other, " + next_name + ")");
    }
    auto add_groups(std::set<int>& groups) const -> void override { inner->add_groups(groups); }
    auto max_length() const -> int override { return inner->max_length(); }
    auto reverse() const -> token_ptr override {
        auto result = std::make_shared<atomic_group_token>();
        result->inner = inner->reverse();
        return result;
    }
};

// A literal character sequence.
struct char_token final : regex_token {
    std::string token;
    bool ignore_case;

    char_token(std::string token_, bool ignore_case_)
        : regex_token{token_}, token{std::move(token_)}, ignore_case{ignore_case_} {}
    char_token(char c, bool ignore_case_) : char_token{std::string(1, c), ignore_case_} {}

    auto generate_code(generation_context& ctx) const -> void override {
        auto lower = token;
        auto upper = token;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const auto name = "str_" + ctx.gen_temp();
        const auto size = std::to_string(token.size());
        const auto array = ": std::array<char, " + std::to_string(token.size() + 1) + "> = ";
        auto test = std::string{};
        if (ignore_case && lower != upper) {
            ctx.add("lower_" + name + array + string_literal(lower) + ";");
            ctx.add("upper_" + name + array + string_literal(upper) + ";");
            test = "!(lower_" + name + "[i] == r.pos[i] || upper_" + name + "[i] == r.pos[i])";
        }
        else {
            ctx.add(name + array + string_literal(token) + ";");
            test = name + "[i] != r.pos[i]";
        }
        ctx.add("if std::distance(r.pos, ctx.end) < " + size + " {");
        ctx.add("  r.matched = false;");
        ctx.add("  break;");
        ctx.add("}");
        ctx.add("(copy i: int = 0) while i < " + size + " next i += 1 {");
        ctx.add("  if " + test + " { r.matched = false; }");
        ctx.add("}");
        ctx.add("if r.matched { r.pos += " + size + "; }");
        ctx.add("else { break; }");
    }
    auto max_length() const -> int override { return capped_length(static_cast<long long>(token.size())); }
    auto reverse() const -> token_ptr override {
        return std::make_shared<char_token>(std::string(token.rbegin(), token.rend()), ignore_case);
    }
    auto append(const char_token& that) -> void {
        token += that.token;
        string_rep += that.string_rep;
    }
};

// `[...]`
struct class_token final : regex_token {
    bool negate;
    bool case_insensitive;
    std::string class_str;

    class_token(bool negate_, bool case_insensitive_, std::string class_str_, std::string text)
        : regex_token{std::move(text)}, negate{negate_}, case_insensitive{case_insensitive_},
          class_str{std::move(class_str_)} {}
    auto generate_code(generation_context& ctx) const -> void override {
        ctx.add_check("class_token_matcher<char, " + bool_text(negate) + ", " +
            bool_text(case_insensitive) + ", " + class_str + ">::match(" +
            std::string{ctx.match_parameters} + ")");
    }
    auto max_length() const -> int override { return 1; }
    auto reverse() const -> token_ptr override { return std::make_shared<class_token>(*this); }
};

// `\1`, `\g{name}`, `\k<name>`, ...
struct group_ref_token final : regex_token {
    int id;
    bool case_insensitive;

    group_ref_token(int id_, bool case_insensitive_, std::string text)
        : regex_token{std::move(text)}, id{id_}, case_insensitive{case_insensitive_} {}
    auto generate_code(generation_context& ctx) const -> void override {
        ctx.add_check("group_ref_token_matcher<char, " + std::to_string(id) + ", " +
            bool_text(case_insensitive) + ", false>(" + std::string{ctx.match_parameters} + ")");
    }
    auto max_length() const -> int override { return longer_than_lookbehind; }
    // Not reached: the parser rejects a backreference in a lookbehind.
    auto reverse() const -> token_ptr override { return std::make_shared<group_ref_token>(*this); }
};

// `(...)`; `number` is -1 for a group without a capture.
struct group_token final : regex_token {
    int number = -1;
    token_ptr inner;

    // A reverse context opens a group at its end and closes it at its start.
    auto generate_code(generation_context& ctx) const -> void override {
        const auto number_text = std::to_string(number);
        if (number != -1) {
            ctx.save_groups();
            ctx.add("ctx..open_group(" + number_text + ", r.pos);");
        }
        inner->generate_code(ctx);
        if (number == -1) return;
        ctx.save_groups();
        ctx.add("ctx..close_group(" + number_text + ", r.pos);");
    }
    auto add_groups(std::set<int>& groups) const -> void override {
        inner->add_groups(groups);
        if (number != -1) groups.insert(number);
    }
    auto max_length() const -> int override { return inner->max_length(); }
    auto reverse() const -> token_ptr override {
        auto result = std::make_shared<group_token>();
        result->number = number;
        result->inner = inner->reverse();
        return result;
    }
};

// `(?=...)`, `(?!...)`, `(?<=...)`, `(?<!...)`
struct lookaround_token final : regex_token {
    bool lookahead;
    bool positive;
    token_ptr inner;

    lookaround_token(bool lookahead_, bool positive_) : lookahead{lookahead_}, positive{positive_} {}
    auto generate_code(generation_context& ctx) const -> void override {
        const auto inner_name = ctx.generate_func(inner);
        ctx.save_groups_of(inner);
        ctx.add_check(std::string{lookahead ? "lookahead" : "lookbehind"} + "_token_matcher<char, " +
            bool_text(positive) + ">(" + std::string{ctx.match_parameters} + ", " + inner_name + ")");
    }
    auto add_groups(std::set<int>& groups) const -> void override { inner->add_groups(groups); }
    auto reverse() const -> token_ptr override {
        // Nested lookarounds keep their direction.
        auto result = std::make_shared<lookaround_token>(lookahead, positive);
        result->inner = inner;
        return result;
    }
};

// `{n,m}`, `*`, `+`, `?`
struct range_token final : regex_token {
    int min_count = -1;
    int max_count = -1;
    int kind = range_greedy;
    token_ptr inner;

    auto generate_code(generation_context& ctx) const -> void override {
        const auto inner_name = ctx.generate_func(inner);
        const auto next_name = ctx.next_func_name();
        ctx.save_groups_of(inner);
        ctx.add_statefull(next_name, "cpp2::regex::range_token_matcher<char, " +
            std::to_string(min_count) + ", " + std::to_string(max_count) + ", " +
            std::to_string(kind) + ">::match(" + std::string{ctx.match_parameters} + ", " +
            inner_name + ", other, " + next_name + ")");
    }
    auto add_groups(std::set<int>& groups) const -> void override { inner->add_groups(groups); }
    auto max_length() const -> int override {
        const auto length = inner->max_length();
        if (length == 0) return 0;
        if (max_count == -1) return longer_than_lookbehind;
        return capped_length(static_cast<long long>(max_count) * length);
    }
    auto reverse() const -> token_ptr override {
        auto result = std::make_shared<range_token>();
        result->min_count = min_count;
        result->max_count = max_count;
        result->kind = kind;
        result->inner = inner->reverse();
        return result;
    }
    [[nodiscard]] auto mod_string() const -> std::string {
        return kind == range_not_greedy ? "?" : kind == range_possessive ? "+" : "";
    }
    [[nodiscard]] auto range_string() const -> std::string {
        if (min_count == max_count) return "{" + std::to_string(min_count) + "}";
        if (min_count == -1) return "{," + std::to_string(max_count) + "}";
        if (max_count == -1) return "{" + std::to_string(min_count) + ",}";
        return "{" + std::to_string(min_count) + "," + std::to_string(max_count) + "}";
    }
};

// Whether `token` is a quantifier, also with a comment after it.
auto is_quantified(const token_ptr& token) -> bool {
    if (dynamic_cast<const range_token*>(token.get())) return true;
    const auto* list = dynamic_cast<const list_token*>(token.get());
    return list && list->tokens.size() == 2 &&
        dynamic_cast<const empty_token*>(list->tokens[1].get()) && is_quantified(list->tokens[0]);
}

// Merges adjacent characters into one token.
auto post_process_list(token_vec& list) -> void {
    for (auto merge = list.begin(); merge != list.end(); ++merge) {
        auto* target = dynamic_cast<char_token*>(merge->get());
        if (!target) continue;
        auto combine = merge + 1;
        while (combine != list.end()) {
            const auto* source = dynamic_cast<const char_token*>(combine->get());
            if (!source) break;
            target->append(*source);
            combine = list.erase(combine);
        }
    }
}

// The current capturing group's tokens and alternatives.
struct group_state final {
    token_vec cur_match_list;
    token_vec alternate_match_lists;
    unsigned modifiers = 0;
    bool is_branch_reset = false;  // `(?|...)`: each alternative numbers its groups from the same start.

    auto next_alternative() -> void {
        auto list = std::exchange(cur_match_list, {});
        post_process_list(list);
        alternate_match_lists.push_back(std::make_shared<list_token>(std::move(list)));
    }
    auto get_as_token() -> token_ptr {
        if (alternate_match_lists.empty()) {
            post_process_list(cur_match_list);
            return std::make_shared<list_token>(cur_match_list);
        }
        next_alternative();
        return std::make_shared<alternative_token>(alternate_match_lists);
    }
};

// Group numbering for branch reset groups, `(?|...)`.
struct branch_reset_state final {
    int cur_group = 1;  // 0 is the whole match.
    int max_group = 1;
    int from = 1;

    auto next() -> int {
        const auto group = cur_group++;
        max_group = std::max(max_group, cur_group);
        return group;
    }
    auto set_next(int group) -> void {
        cur_group = group;
        max_group = std::max(max_group, group);
    }
    auto next_alternative() -> void { cur_group = from; }
    auto set_active_reset(int restart) -> void {
        cur_group = from = max_group = restart;
    }
};

class parse_context final {
public:
    explicit parse_context(std::string_view regex)
        : regex_{regex}, root_{std::make_shared<empty_token>("")} {}

    std::map<std::string, int> named_groups;
    std::string error_message;
    // The number of lookarounds around the current position.
    int lookaround_depth = 0;

    auto start_group() -> group_state {
        auto old_state = std::exchange(cur_group_state_, {});
        cur_group_state_.modifiers = old_state.modifiers;
        return old_state;
    }
    auto end_group(group_state old_state) -> token_ptr {
        auto inner = cur_group_state_.get_as_token();
        cur_group_state_ = std::move(old_state);
        return inner;
    }
    [[nodiscard]] auto get_modifiers() const -> unsigned { return cur_group_state_.modifiers; }
    [[nodiscard]] auto has_modifier(unsigned flag) const -> bool { return (get_modifiers() & flag) != 0; }
    auto set_modifiers(unsigned modifiers) -> void { cur_group_state_.modifiers = modifiers; }
    auto set_branch_reset() -> void { cur_group_state_.is_branch_reset = true; }

    auto branch_reset_new_state() -> branch_reset_state {
        auto old_state = std::exchange(cur_branch_reset_state_, {});
        cur_branch_reset_state_.set_active_reset(old_state.cur_group);
        return old_state;
    }
    auto branch_reset_restore_state(branch_reset_state old_state) -> void {
        const auto max_group = cur_branch_reset_state_.max_group;
        cur_branch_reset_state_ = old_state;
        cur_branch_reset_state_.set_next(max_group);
    }
    auto next_alternative() -> void {
        cur_group_state_.next_alternative();
        if (cur_group_state_.is_branch_reset) cur_branch_reset_state_.next_alternative();
    }

    auto add_token(token_ptr token) -> void { cur_group_state_.cur_match_list.push_back(std::move(token)); }
    [[nodiscard]] auto has_token() const -> bool { return !cur_group_state_.cur_match_list.empty(); }
    // Whether there is an element for a quantifier: a token that matches or asserts something,
    // not the start of an alternative or a modifier group such as `(?i)`.
    [[nodiscard]] auto has_element() const -> bool {
        return has_token() &&
            dynamic_cast<const empty_token*>(cur_group_state_.cur_match_list.back().get()) == nullptr;
    }
    [[nodiscard]] auto last_token() const -> const token_ptr& { return cur_group_state_.cur_match_list.back(); }
    auto pop_token() -> token_ptr {
        if (!has_token()) return nullptr;
        auto result = cur_group_state_.cur_match_list.back();
        cur_group_state_.cur_match_list.pop_back();
        return result;
    }
    [[nodiscard]] auto get_as_token() const -> const token_ptr& { return root_; }

    [[nodiscard]] auto get_cur_group() const -> int { return cur_branch_reset_state_.cur_group; }
    auto next_group() -> int { return cur_branch_reset_state_.next(); }
    auto set_named_group(const std::string& name, int id) -> void {
        // The leftmost definition of a name is kept.
        named_groups.emplace(name, id);
    }
    [[nodiscard]] auto get_named_group(const std::string& name) const -> int {
        const auto found = named_groups.find(name);
        return found == named_groups.end() ? -1 : found->second;
    }

    [[nodiscard]] auto current() const -> char { return pos_ < regex_.size() ? regex_[pos_] : '\0'; }

    auto next() -> bool { return next_impl(false, false); }
    auto next_in_class() -> bool { return next_impl(true, false); }
    auto next_no_skip() -> bool { return next_impl(false, true); }
    auto next_n(int n) -> bool {
        auto result = true;
        for (auto count = 0; result && count < n; ++count) result = next();
        return result;
    }
    [[nodiscard]] auto has_next() const -> bool { return pos_ < regex_.size(); }

    auto grab_until(std::string_view end, std::string& result) -> bool {
        return grab_until_impl(end, result, false);
    }
    auto grab_until(char end, std::string& result) -> bool {
        return grab_until_impl(std::string_view{&end, 1}, result, false);
    }
    auto grab_until_one_of(std::string_view end, std::string& result) -> bool {
        return grab_until_impl(end, result, true);
    }
    auto grab_n(int n, std::string& result) -> bool {
        const auto count = static_cast<std::size_t>(n);
        if (pos_ + count > regex_.size()) {
            result.clear();
            return false;
        }
        result = regex_.substr(pos_, count);
        pos_ += count - 1;
        return true;
    }
    auto grab_number() -> std::string {
        const auto start = pos_;
        auto search = pos_;
        if (current() == '-') ++search;
        const auto end = regex_.find_first_not_of("1234567890", search);
        if (end == std::string_view::npos) {
            pos_ = regex_.size() - 1;
            return std::string{regex_.substr(start)};
        }
        pos_ = end - 1;
        return std::string{regex_.substr(start, end - start)};
    }
    [[nodiscard]] auto peek() const -> char { return peek_impl(false); }
    [[nodiscard]] auto peek_in_class() const -> char { return peek_impl(true); }
    [[nodiscard]] auto peek_no_skip() const -> char {
        return pos_ + 1 < regex_.size() ? regex_[pos_ + 1] : '\0';
    }

    auto parser_group_modifiers(std::string_view change, unsigned& modifiers) -> bool;
    auto parse_until(char term) -> bool;
    auto parse(std::string_view modifiers) -> bool {
        auto flags = 0u;
        if (!parser_group_modifiers(modifiers, flags)) return false;
        set_modifiers(flags);
        const auto result = parse_until('\0');
        if (result) root_ = cur_group_state_.get_as_token();
        return result;
    }

    [[nodiscard]] auto get_pos() const -> std::size_t { return pos_; }
    [[nodiscard]] auto rest() const -> std::string_view { return regex_.substr(pos_); }
    [[nodiscard]] auto get_range(std::size_t start, std::size_t end) const -> std::string {
        return std::string{regex_.substr(start, end - start + 1)};
    }
    [[nodiscard]] auto valid() const -> bool { return has_next() && error_message.empty(); }
    auto fail(const std::string& message) -> bool {
        error(message);
        return false;
    }
    auto error(const std::string& message) -> token_ptr {
        if (error_message.empty()) {
            error_message = "Error during parsing of regex '" + std::string{regex_} +
                "' at position '" + std::to_string(pos_) + "': " + message;
        }
        return nullptr;
    }

private:
    // The position of the next token, skipping whitespace and comments for `x`/`xx`.
    [[nodiscard]] auto get_next_position(bool in_class, bool no_skip) const -> std::size_t {
        auto perl_syntax = false;
        if (!no_skip) {
            perl_syntax = in_class
                ? has_modifier(flag_perl_code_syntax) && has_modifier(flag_perl_code_syntax_in_classes)
                : has_modifier(flag_perl_code_syntax);
        }
        auto cur = pos_ + 1;
        if (perl_syntax) {
            for (; cur < regex_.size(); ++cur) {
                const auto c = regex_[cur];
                if (is_space(c)) continue;
                if (in_class || c != '#') break;
                cur = regex_.find('\n', cur);
                if (cur == std::string_view::npos) cur = regex_.size();
            }
        }
        return std::min(cur, regex_.size());
    }
    auto next_impl(bool in_class, bool no_skip) -> bool {
        pos_ = get_next_position(in_class, no_skip);
        return pos_ != regex_.size();
    }
    auto grab_until_impl(std::string_view end, std::string& result, bool any) -> bool {
        const auto found = any ? regex_.find_first_of(end, pos_) : regex_.find(end, pos_);
        if (found == std::string_view::npos) {
            result.clear();
            return false;
        }
        result = regex_.substr(pos_, found - pos_);
        pos_ = found;
        return true;
    }
    [[nodiscard]] auto peek_impl(bool in_class) const -> char {
        const auto next = get_next_position(in_class, false);
        return next < regex_.size() ? regex_[next] : '\0';
    }

    std::string_view regex_;
    std::size_t pos_ = 0;
    token_ptr root_;
    group_state cur_group_state_;
    branch_reset_state cur_branch_reset_state_;
};

auto parse_context::parser_group_modifiers(std::string_view change, unsigned& modifiers) -> bool {
    auto is_negative = false;
    auto is_reset = false;
    const auto apply = [&](unsigned flag) {
        if (is_negative) modifiers &= ~flag;
        else modifiers |= flag;
    };
    for (auto it = change.begin(); it != change.end(); ++it) {
        const auto c = *it;
        if (c == '^') {
            is_reset = true;
            modifiers = 0;
        }
        else if (c == '-') {
            if (is_reset) {
                error("No negative modifier allowed.");
                return false;
            }
            is_negative = true;
        }
        else if (c == 'i') apply(flag_case_insensitive);
        else if (c == 'm') apply(flag_multiple_lines);
        else if (c == 's') apply(flag_single_line);
        else if (c == 'n') apply(flag_no_group_captures);
        else if (c == 'x') {
            if (it + 1 == change.end() || *(it + 1) != 'x') {
                // `x` alone also clears `xx`.
                apply(flag_perl_code_syntax);
                modifiers &= ~flag_perl_code_syntax_in_classes;
            }
            else {
                apply(flag_perl_code_syntax);
                apply(flag_perl_code_syntax_in_classes);
                ++it;
            }
        }
        else {
            error(std::string{"Unknown modifier: "} + c);
            return false;
        }
    }
    return true;
}

// A character code must fit a `char`, as Unicode is not supported.
auto byte_number(parse_context& ctx, int number) -> bool {
    if (0 <= number && number <= 0xFF) return true;
    return ctx.fail("Character code " + std::to_string(number) + " is not in 0..255; Unicode is "
                    "not supported.");
}

auto is_word_char(char c) -> bool {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// The character a `\a`, `\f`, `\e`, `\n`, `\r` or `\t` escape stands for, or a
// non-word character a backslash quotes; `\0` for other characters.
auto escaped_char(char c) -> char {
    static const auto specials = std::map<char, char>{{'a', '\a'}, {'f', '\f'}, {'e', '\x1b'},
        {'n', '\n'}, {'r', '\r'}, {'t', '\t'}};
    const auto special = specials.find(c);
    if (special != specials.end()) return special->second;
    return c != '\0' && !is_word_char(c) ? c : '\0';
}

auto is_hex_digit(char c) -> bool { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

// The number of `\x62` or `\x{62}`, at the `x`; `brackets` tells which form. As in
// Perl, `\x` without brackets takes at most two digits, so `\x4g` is `\x04` and `g`.
auto hexadecimal_number(parse_context& ctx, int& number, bool& brackets) -> bool {
    if (!ctx.next()) return ctx.fail("x escape without number.");
    auto number_str = std::string{};
    brackets = ctx.current() == '{';
    if (brackets) {
        ctx.next();
        if (!ctx.grab_until('}', number_str)) return ctx.fail("No ending bracket for \\x");
    }
    else {
        if (!is_hex_digit(ctx.current())) return ctx.fail("Missing hexadecimal digits after \\x.");
        number_str = ctx.current();
        if (is_hex_digit(ctx.peek_no_skip())) {
            ctx.next_no_skip();
            number_str += ctx.current();
        }
    }
    if (!std::all_of(number_str.begin(), number_str.end(), is_hex_digit) ||
        !string_to_int(number_str, number, 16)) {
        return ctx.fail("Could not convert hexadecimal to int.");
    }
    return byte_number(ctx, number);
}

// The number of `\o{142}`, at the `o`.
auto octal_number(parse_context& ctx, int& number) -> bool {
    if (!ctx.next()) return ctx.fail("o escape without number.");
    if (ctx.current() != '{') return ctx.fail("Missing opening bracket for \\o.");
    auto number_str = std::string{};
    ctx.next();
    if (!ctx.grab_until('}', number_str)) return ctx.fail("No ending bracket for \\o");
    if (number_str.find_first_not_of("01234567") != std::string::npos ||
        !string_to_int(number_str, number, 8)) {
        return ctx.fail("Could not convert octal to int.");
    }
    return byte_number(ctx, number);
}

// A check that matches `width` characters: 1 for `.` and `\d`, 0 for an assertion.
auto check(std::string text, std::string matcher, int width = 0) -> token_ptr {
    return std::make_shared<check_token>(std::move(text), std::move(matcher), width);
}

auto with_string(std::shared_ptr<char_token> token, std::string text) -> token_ptr {
    token->string_rep = std::move(text);
    return token;
}

// `|`; an alternative may be empty, and then matches the empty string.
auto alternative_parse(parse_context& ctx) -> token_ptr {
    if (ctx.current() != '|') return nullptr;
    ctx.next_alternative();
    return std::make_shared<empty_token>("");
}

// `.`
auto any_parse(parse_context& ctx) -> token_ptr {
    if (ctx.current() != '.') return nullptr;
    return check(".", "any_token_matcher<char, " + bool_text(ctx.has_modifier(flag_single_line)) + ">", 1);
}

auto class_matcher(const std::string& name, const std::string& arguments) -> std::string {
    return "::cpp2::regex::" + name + "<char" + (arguments.empty() ? "" : ", ") + arguments + ">";
}

// A character, a named class, or an unescaped `-` of a bracketed class.
struct class_item final {
    char c = '\0';
    std::string class_name;
    bool is_negated_class = false;
    bool is_dash = false;
};

// The escape at the backslash in a bracketed class, as Perl reads it.
auto class_escape_parse(parse_context& ctx, class_item& item) -> bool {
    if (!ctx.next_no_skip()) return ctx.fail("Escape without a following character.");
    static const auto names = std::map<char, std::string>{
        {'d', "short_digits"}, {'D', "short_not_digits"},
        {'h', "short_hor_space"}, {'H', "short_not_hor_space"},
        {'s', "short_space"}, {'S', "short_not_space"},
        {'v', "short_ver_space"}, {'V', "short_not_ver_space"},
        {'w', "short_word"}, {'W', "short_not_word"}};
    const auto c = ctx.current();
    auto number = 0;
    if (const auto found = names.find(c); found != names.end()) {
        item.class_name = found->second;
        return true;
    }
    if (c == 'x') {
        auto brackets = false;
        if (!hexadecimal_number(ctx, number, brackets)) return false;
    }
    else if (c == 'o') {
        if (!octal_number(ctx, number)) return false;
    }
    else if ('0' <= c && c <= '7') {
        // Up to three octal digits.
        number = c - '0';
        for (auto count = 1; count < 3 && '0' <= ctx.peek_no_skip() && ctx.peek_no_skip() <= '7';
             ++count) {
            ctx.next_no_skip();
            number = number * 8 + (ctx.current() - '0');
        }
        if (!byte_number(ctx, number)) return false;
    }
    else if (c == 'b') {
        number = '\b';
    }
    else if (escaped_char(c) != '\0') {
        number = static_cast<unsigned char>(escaped_char(c));
    }
    else {
        return ctx.fail(std::string{"Unknown escape sequence in character class: \\"} + c);
    }
    item.c = static_cast<char>(number);
    return true;
}

// `[abcx-y[:digit:][:^alpha:]\d\x41]`
auto class_parse(parse_context& ctx) -> token_ptr {
    if (ctx.current() != '[') return nullptr;
    const auto start_pos = ctx.get_pos();
    static const auto supported_classes = std::vector<std::string>{"alnum", "alpha", "ascii",
        "blank", "cntrl", "digit", "graph", "lower", "print", "punct", "space", "upper", "word",
        "xdigit"};
    auto items = std::vector<class_item>{};
    auto is_negate = false;
    auto first = true;
    while (ctx.next_in_class() && (ctx.current() != ']' || first)) {
        if (ctx.current() == '^' && first && !is_negate) {
            is_negate = true;
            continue;
        }
        first = false;
        auto item = class_item{};
        if (ctx.current() == '[' && ctx.peek_in_class() == ':') {
            ctx.next_n(2);
            auto name = std::string{};
            if (!ctx.grab_until(":]", name)) return ctx.error("Could not find end of character class.");
            item.is_negated_class = name.starts_with('^');
            if (item.is_negated_class) name.erase(0, 1);
            if (name == "digits") name = "digit";  // upstream's spelling
            if (std::find(supported_classes.begin(), supported_classes.end(), name) ==
                supported_classes.end()) {
                return ctx.error("Unsupported character class. Supported ones are: " +
                                 join(supported_classes));
            }
            item.class_name = name == "digit" ? "digits" : name;
            ctx.next();  // The ':' before the closing ']'.
        }
        else if (ctx.current() == '\\') {
            if (!class_escape_parse(ctx, item)) return nullptr;
        }
        else {
            item.c = ctx.current();
            item.is_dash = item.c == '-';
        }
        items.push_back(std::move(item));
    }
    if (ctx.current() != ']') {
        return ctx.error("Error end of character class definition before terminating ']'.");
    }
    const auto end_pos = ctx.get_pos();
    // A `-` between two characters makes a range; any other `-` is literal.
    const auto is_char = [&](std::size_t index) {
        return index < items.size() && items[index].class_name.empty() && !items[index].is_dash;
    };
    auto classes = std::vector<std::string>{};
    for (std::size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        if (!item.class_name.empty()) {
            auto matcher = class_matcher(item.class_name + "_class", "");
            classes.push_back(item.is_negated_class
                                  ? class_matcher("negated_class_entry", matcher)
                                  : matcher);
        }
        else if (is_char(index) && index + 2 < items.size() && items[index + 1].is_dash &&
                 is_char(index + 2)) {
            const auto last = items[index + 2].c;
            if (static_cast<unsigned char>(last) < static_cast<unsigned char>(item.c)) {
                return ctx.error("Invalid range in character class.");
            }
            classes.push_back(class_matcher("range_class_entry",
                                            char_literal(item.c) + ", " + char_literal(last)));
            index += 2;
        }
        else {
            classes.push_back(class_matcher("single_class_entry", char_literal(item.c)));
        }
    }
    return std::make_shared<class_token>(is_negate, ctx.has_modifier(flag_case_insensitive),
                                         join(classes), ctx.get_range(start_pos, end_pos));
}

// `\a`, `\n`, `\[`, `\$`, `\-`, ...: a control character or a quoted non-word character.
auto escape_parse(parse_context& ctx) -> token_ptr {
    if (ctx.current() != '\\' || escaped_char(ctx.peek_no_skip()) == '\0') return nullptr;
    ctx.next_no_skip();
    const auto c = ctx.current();
    return with_string(std::make_shared<char_token>(escaped_char(c), false),
                       std::string{"\\"} + c);
}

// A backslash that no other parser takes.
auto unknown_escape_parse(parse_context& ctx) -> token_ptr {
    if (ctx.current() != '\\') return nullptr;
    if (ctx.peek_no_skip() == '\0') return ctx.error("Escape without a following character.");
    return ctx.error(std::string{"Unknown escape sequence: \\"} + ctx.peek_no_skip());
}

// `\K`
auto global_group_reset_parse(parse_context& ctx) -> token_ptr {
    if (!(ctx.current() == '\\' && ctx.peek() == 'K')) return nullptr;
    if (ctx.lookaround_depth > 0) return ctx.error("\\K is not permitted in a lookahead or lookbehind.");
    ctx.next();
    return std::make_shared<code_token>("\\K", "ctx..set_group_start(0, r.pos);");
}

// `\1`, `\g1`, `\g{name}`, `\k{name}`, `\k<name>`, `\k'name'`
auto group_ref_parse(parse_context& ctx) -> token_ptr {
    if (ctx.current() != '\\') return nullptr;
    auto str = std::string{"\\"};
    auto group = std::string{};
    const auto next_char = ctx.peek();
    if ('0' <= next_char && next_char <= '9') {
        ctx.next();
        group = ctx.grab_number();
        if (group.size() >= 3) {
            // `\000` is an octal character, not a group reference.
            auto number = 0;
            if (!string_to_int(group, number, 8)) return ctx.error("Could not convert octal to int.");
            if (!byte_number(ctx, number)) return nullptr;
            return with_string(std::make_shared<char_token>(static_cast<char>(number),
                                                            ctx.has_modifier(flag_case_insensitive)),
                               "\\" + int_to_string(number, true));
        }
        str += group;
    }
    else if (next_char == 'g') {
        ctx.next();
        if (!ctx.next()) return ctx.error("Group escape without a following char.");
        str += "g";
        if (ctx.current() == '{') {
            str += "{";
            if (!(ctx.next() && ctx.grab_until('}', group))) return ctx.error("No ending bracket.");
            str += group + "}";
        }
        else {
            group = ctx.grab_number();
            str += group;
        }
    }
    else if (next_char == 'k') {
        ctx.next();
        if (!ctx.next()) return ctx.error("Group escape without a following char.");
        str += "k";
        auto term = '\0';
        if (ctx.current() == '{') term = '}';
        else if (ctx.current() == '<') term = '>';
        else if (ctx.current() == '\'') term = '\'';
        else return ctx.error("Group escape has wrong operator.");
        str += ctx.current();
        if (!(ctx.next() && ctx.grab_until(term, group))) return ctx.error("No ending bracket.");
        str += group + term;
    }
    else {
        return nullptr;
    }
    group = trim_copy(group);
    auto group_id = 0;
    if (string_to_int(group, group_id)) {
        if (group_id < 0) {
            group_id = ctx.get_cur_group() + group_id;
            if (group_id < 1) {
                return ctx.error("Relative group reference does not reference a valid group. "
                                 "(Would be " + std::to_string(group_id) + ".)");
            }
        }
        if (group_id >= ctx.get_cur_group()) {
            return ctx.error("Group reference is used before the group is declared.");
        }
    }
    else {
        group_id = ctx.get_named_group(group);
        if (group_id == -1) return ctx.error("Group names does not exist. (Name is: " + group + ")");
    }
    return std::make_shared<group_ref_token>(group_id, ctx.has_modifier(flag_case_insensitive),
                                             str);
}

auto parse_lookaround(parse_context& ctx, const std::string& syntax, bool lookahead,
                      bool positive) -> token_ptr {
    ctx.next();  // The last character of the syntax.
    auto result = std::make_shared<lookaround_token>(lookahead, positive);
    auto old_state = ctx.start_group();
    ++ctx.lookaround_depth;
    if (!ctx.parse_until(')')) return ctx.error("Group without a closing bracket.");
    --ctx.lookaround_depth;
    result->inner = ctx.end_group(std::move(old_state));
    result->string_rep = "(" + syntax + result->inner->to_string() + ")";
    if (lookahead) return result;
    // As in Perl; a backreference has no bound.
    if (result->inner->max_length() > max_lookbehind) {
        return ctx.error("Lookbehind longer than " + std::to_string(max_lookbehind) +
                         " not implemented.");
    }
    result->inner = result->inner->reverse();
    return result;
}

auto parse_atomic_pattern(parse_context& ctx, const std::string& syntax) -> token_ptr {
    ctx.next();  // The last character of the syntax.
    auto result = std::make_shared<atomic_group_token>();
    auto old_state = ctx.start_group();
    if (!ctx.parse_until(')')) return ctx.error("Group without a closing bracket.");
    result->inner = ctx.end_group(std::move(old_state));
    result->string_rep = "(" + syntax + result->inner->to_string() + ")";
    return result;
}

auto group_string(const std::string& name, bool name_brackets, bool has_modifier,
                  const std::string& modifiers, const token_ptr& inner) -> std::string {
    auto start = std::string{"("};
    if (!name.empty()) start += name_brackets ? "?<" + name + ">" : "?'" + name + "'";
    else if (has_modifier) start += "?" + modifiers + ":";
    return start + inner->to_string() + ")";
}

// `(...)`, `(?<name>...)`, `(?i)`, `(?i:...)`, `(?#...)`, `(?|...)`, `(?=...)`,
// `(?!...)`, `(?<=...)`, `(?<!...)`, `(?>...)`, and `(*name:...)`
auto group_parse(parse_context& ctx) -> token_ptr {
    if (ctx.current() != '(') return nullptr;
    auto has_id = !ctx.has_modifier(flag_no_group_captures);
    auto has_pattern = true;
    auto group_name = std::string{};
    auto group_name_brackets = true;
    auto modifiers = std::string{};
    auto modifiers_change_to = ctx.get_modifiers();
    if (!ctx.next()) return ctx.error("Group without closing bracket.");
    if (ctx.current() == '?') {
        if (!ctx.next_no_skip()) return ctx.error("Missing character after group opening.");
        if (ctx.current() == '<' && (ctx.peek() == '=' || ctx.peek() == '!')) {
            ctx.next();
            return parse_lookaround(ctx, std::string{"?<"} + ctx.current(), false,
                                    ctx.current() == '=');
        }
        if (ctx.current() == '<' || ctx.current() == '\'') {
            auto end_char = ctx.current();
            if (end_char == '<') end_char = '>';
            else group_name_brackets = false;
            has_id = true;  // Named groups always capture.
            if (!ctx.next()) return ctx.error("Missing ending bracket for named group.");
            if (!ctx.grab_until(end_char, group_name)) {
                return ctx.error("Missing ending bracket for named group.");
            }
            if (!ctx.next()) return ctx.error("Group without closing bracket.");
        }
        else if (ctx.current() == '#') {
            auto comment = std::string{};
            ctx.next();
            if (!ctx.grab_until(")", comment)) return ctx.error("Group without closing bracket.");
            // The comment stays with the previous token, so a following range applies to it.
            auto text = std::make_shared<empty_token>("(?#" + comment + ")");
            if (!ctx.has_token()) return text;
            return std::make_shared<list_token>(token_vec{ctx.pop_token(), text});
        }
        else if (ctx.current() == '|') {
            if (!ctx.next()) return ctx.error("Missing ending bracket for named group.");
            auto old_parser_state = ctx.start_group();
            ctx.set_branch_reset();
            const auto old_branch_state = ctx.branch_reset_new_state();
            if (!ctx.parse_until(')')) return nullptr;
            ctx.branch_reset_restore_state(old_branch_state);
            auto inner = ctx.end_group(std::move(old_parser_state));
            return std::make_shared<list_token>(token_vec{std::make_shared<empty_token>("(?|"),
                inner, std::make_shared<empty_token>(")")});
        }
        else if (ctx.current() == '=' || ctx.current() == '!') {
            return parse_lookaround(ctx, std::string{"?"} + ctx.current(), true, ctx.current() == '=');
        }
        else if (ctx.current() == '>') {
            return parse_atomic_pattern(ctx, std::string{"?"} + ctx.current());
        }
        else {
            has_id = false;
            if (!ctx.grab_until_one_of("):", modifiers)) {
                return ctx.error("Missing ending bracket for group.");
            }
            if (!ctx.parser_group_modifiers(modifiers, modifiers_change_to)) return nullptr;
            if (ctx.current() == ')') has_pattern = false;
            else if (!ctx.next()) return ctx.error("Missing ending bracket for group.");
        }
    }
    else if (ctx.current() == '*') {
        ctx.next();
        auto name = std::string{};
        if (!ctx.grab_until(':', name)) return ctx.error("Missing colon for named pattern.");
        const auto syntax = "*" + name + ":";
        if (name == "pla" || name == "positive_lookahead") return parse_lookaround(ctx, syntax, true, true);
        if (name == "nla" || name == "negative_lookahead") return parse_lookaround(ctx, syntax, true, false);
        if (name == "plb" || name == "positive_lookbehind") return parse_lookaround(ctx, syntax, false, true);
        if (name == "nlb" || name == "negative_lookbehind") return parse_lookaround(ctx, syntax, false, false);
        if (name == "atomic") return parse_atomic_pattern(ctx, syntax);
        return ctx.error("Unknown named group pattern: '" + name + "'");
    }
    if (!has_pattern) {
        ctx.set_modifiers(modifiers_change_to);
        return std::make_shared<empty_token>("(?" + modifiers + ")");
    }
    auto result = std::make_shared<group_token>();
    if (has_id) {
        result->number = ctx.next_group();
        if (!group_name.empty()) ctx.set_named_group(group_name, result->number);
    }
    auto old_state = ctx.start_group();
    ctx.set_modifiers(modifiers_change_to);
    if (!ctx.parse_until(')')) return nullptr;
    result->inner = ctx.end_group(std::move(old_state));
    result->string_rep = group_string(group_name, group_name_brackets, !has_id, modifiers,
                                      result->inner);
    return result;
}

// `\x62` or `\x{62}`
auto hexadecimal_parse(parse_context& ctx) -> token_ptr {
    if (!(ctx.current() == '\\' && ctx.peek() == 'x')) return nullptr;
    ctx.next();
    auto number = 0;
    auto has_brackets = false;
    if (!hexadecimal_number(ctx, number, has_brackets)) return nullptr;
    auto syntax = int_to_string(number, false);
    if (has_brackets) syntax = "{" + syntax + "}";
    return with_string(std::make_shared<char_token>(static_cast<char>(number),
                                                    ctx.has_modifier(flag_case_insensitive)),
                       "\\x" + syntax);
}

// `$`, `\z`, `\Z`
auto line_end_parse(parse_context& ctx) -> token_ptr {
    if (ctx.current() == '$') {
        return check("$", "line_end_token_matcher<char, " +
                          bool_text(ctx.has_modifier(flag_multiple_lines)) + ", true>");
    }
    if (ctx.current() == '\\' && (ctx.peek() == 'z' || ctx.peek() == 'Z')) {
        ctx.next();
        return check(std::string{"\\"} + ctx.current(),
                     "line_end_token_matcher<char, false, " + bool_text(ctx.current() == 'Z') + ">");
    }
    return nullptr;
}

// `^`, `\A`
auto line_start_parse(parse_context& ctx) -> token_ptr {
    if (ctx.current() != '^' && !(ctx.current() == '\\' && ctx.peek() == 'A')) return nullptr;
    if (ctx.current() == '\\') {
        ctx.next();
        return check("\\A", "line_start_token_matcher<char, false>");
    }
    return check("^", "line_start_token_matcher<char, " +
                      bool_text(ctx.has_modifier(flag_multiple_lines)) + ">");
}

// `\d`, `\w`, ...
auto named_class_parse(parse_context& ctx) -> token_ptr {
    if (ctx.current() != '\\') return nullptr;
    static const auto names = std::map<char, std::string>{
        {'d', "named_class_digits"}, {'D', "named_class_not_digits"},
        {'h', "named_class_hor_space"}, {'H', "named_class_not_hor_space"},
        {'N', "named_class_no_new_line"}, {'s', "named_class_space"},
        {'S', "named_class_not_space"}, {'v', "named_class_ver_space"},
        {'V', "named_class_not_ver_space"}, {'w', "named_class_word"},
        {'W', "named_class_not_word"}};
    const auto found = names.find(ctx.peek());
    if (found == names.end()) return nullptr;
    ctx.next();
    return check(std::string{"\\"} + ctx.current(), found->second + "<char, " +
                 bool_text(ctx.has_modifier(flag_case_insensitive)) + ">::match", 1);
}

// `\o{142}`
auto octal_parse(parse_context& ctx) -> token_ptr {
    if (!(ctx.current() == '\\' && ctx.peek() == 'o')) return nullptr;
    ctx.next();
    auto number = 0;
    if (!octal_number(ctx, number)) return nullptr;
    return with_string(std::make_shared<char_token>(static_cast<char>(number),
                                                    ctx.has_modifier(flag_case_insensitive)),
                       "\\o{" + int_to_string(number, true) + "}");
}

// A trailing `?` (not greedy) or `+` (possessive).
auto parse_range_modifier(parse_context& ctx, range_token& token) -> void {
    if (ctx.peek() == '?') {
        token.kind = range_not_greedy;
        ctx.next();
    }
    else if (ctx.peek() == '+') {
        token.kind = range_possessive;
        ctx.next();
    }
}

// The bounds of a quantifier `{n}`, `{n,}`, `{,m}` or `{n,m}` at the start of `text`,
// with spaces and tabs allowed inside the braces, or nothing.
struct range_bounds {
    std::string min;
    std::string max;
    bool has_comma = false;
};
auto read_range_bounds(std::string_view text) -> std::optional<range_bounds> {
    auto i = std::size_t{1};
    const auto blanks = [&] {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    };
    const auto digits = [&] {
        const auto start = i;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) != 0) ++i;
        return std::string{text.substr(start, i - start)};
    };
    auto result = range_bounds{};
    blanks();
    result.min = digits();
    blanks();
    if (i < text.size() && text[i] == ',') {
        result.has_comma = true;
        ++i;
        blanks();
        result.max = digits();
        blanks();
    }
    if (i == text.size() || text[i] != '}' || (result.min.empty() && result.max.empty())) {
        return std::nullopt;
    }
    return result;
}

// `{n}`, `{n,}`, `{,m}`, `{n,m}`. As in Perl, any other `{`, or one without a previous
// element, is a literal character, except after an escape such as `\d`, where Perl reserves it.
auto range_parse(parse_context& ctx) -> token_ptr {
    if (ctx.current() != '{') return nullptr;
    const auto bounds = read_range_bounds(ctx.rest());
    if (!bounds || !ctx.has_element()) {
        const auto& previous = ctx.has_element() ? ctx.last_token()->to_string() : std::string{};
        if (previous.size() == 2 && previous[0] == '\\' &&
            std::isalpha(static_cast<unsigned char>(previous[1])) != 0) {
            return ctx.error("A '{' after " + previous + " must be escaped as '\\{'.");
        }
        return nullptr;
    }
    for (const auto& bound : {bounds->min, bounds->max}) {
        if (bound.size() > 1 && bound[0] == '0') {
            return ctx.error("Range bound " + bound + " has a leading zero.");
        }
    }
    if (is_quantified(ctx.last_token())) return ctx.error("Nested quantifiers.");
    auto inner = std::string{};
    ctx.grab_until('}', inner);
    auto result = std::make_shared<range_token>();
    parse_range_modifier(ctx, *result);
    constexpr auto number_message = "Could not convert range to number.";
    if (!bounds->min.empty() && !string_to_int(bounds->min, result->min_count)) {
        return ctx.error(number_message);
    }
    if (!bounds->max.empty() && !string_to_int(bounds->max, result->max_count)) {
        return ctx.error(number_message);
    }
    if (!bounds->has_comma) result->max_count = result->min_count;
    const auto min_count = result->min_count;
    const auto max_count = result->max_count;
    if (min_count != -1 && max_count != -1 && min_count > max_count) {
        return ctx.error("Min and max values in range are wrong it should hold 0 <= min <= max. "
                         "Have 0 <= " + std::to_string(min_count) + " <= " +
                         std::to_string(max_count));
    }
    result->inner = ctx.pop_token();
    result->string_rep = result->inner->to_string() + result->range_string() + result->mod_string();
    return result;
}

// `*`, `+`, `?`
auto special_range_parse(parse_context& ctx) -> token_ptr {
    auto result = std::make_shared<range_token>();
    const auto symbol = ctx.current();
    if (symbol == '*') result->min_count = 0;
    else if (symbol == '+') result->min_count = 1;
    else if (symbol == '?') {
        result->min_count = 0;
        result->max_count = 1;
    }
    else return nullptr;
    if (!ctx.has_element()) return ctx.error(std::string{"'"} + symbol + "' without previous element.");
    if (is_quantified(ctx.last_token())) return ctx.error("Nested quantifiers.");
    parse_range_modifier(ctx, *result);
    result->inner = ctx.pop_token();
    result->string_rep = result->inner->to_string() + symbol + result->mod_string();
    return result;
}

// `\G`, the position where the search starts, as Perl's pos().
auto start_match_parse(parse_context& ctx) -> token_ptr {
    if (ctx.current() != '\\' || ctx.peek() != 'G') return nullptr;
    ctx.next();
    return check("\\G", "search_start_token_matcher<char>");
}

// `\b`, `\B`
auto word_boundary_parse(parse_context& ctx) -> token_ptr {
    if (ctx.current() != '\\') return nullptr;
    if (ctx.peek() == 'b') {
        ctx.next();
        return check("\\b", "word_boundary_token_matcher<char, false>");
    }
    if (ctx.peek() == 'B') {
        ctx.next();
        return check("\\B", "word_boundary_token_matcher<char, true>");
    }
    return nullptr;
}

auto char_parse(parse_context& ctx) -> token_ptr {
    return std::make_shared<char_token>(ctx.current(), ctx.has_modifier(flag_case_insensitive));
}

auto parse_context::parse_until(char term) -> bool {
    static constexpr token_ptr (*parsers[])(parse_context&) = {alternative_parse, any_parse,
        class_parse, escape_parse, global_group_reset_parse, group_ref_parse, group_parse,
        hexadecimal_parse, line_end_parse, line_start_parse, named_class_parse, octal_parse,
        range_parse, special_range_parse, start_match_parse, word_boundary_parse,
        unknown_escape_parse, char_parse};
    for (; valid(); next()) {
        if (current() == term) return true;
        auto token = token_ptr{};
        for (const auto parser : parsers) {
            if (token || !valid()) break;
            token = parser(*this);
        }
        if (!token) break;
        add_token(std::move(token));
    }
    if (term != '\0') error("Group without closing bracket.");
    return error_message.empty();
}

auto named_group_lookup(const std::map<std::string, int>& named_groups) -> std::string {
    auto result = std::string{"  get_named_group_index: (name) -> int = {\n"};
    if (named_groups.empty()) return result + "    _ = name;\n    return -1;\n  }\n";
    auto separator = std::string{"    "};
    for (const auto& [name, id] : named_groups) {
        result += separator + "if name == " + string_literal(name) + " { return " +
            std::to_string(id) + "; }\n";
        separator = "    else ";
    }
    return result + "    else { return -1; }\n  }\n";
}

struct regex_matcher final {
    // Cpp2 declaration of the type `<name>_matcher: <CharT> type`.
    std::string cpp2;
    // Set instead of `cpp2` when the pattern is invalid.
    std::string error;
};

// Parses `pattern` (optionally `/pattern/flags` or `'pattern'flags`) and generates
// the matcher built on the `cpp2::regex` templates of upstream's cpp2regex.h.
auto generate_regex_matcher(std::string_view name, std::string_view pattern) -> regex_matcher {
    // `/pattern/flags` and `'pattern'flags` carry modifiers.
    auto modifier = std::string{};
    auto modifier_escape = std::string{};
    if (pattern.find_first_of("'/") == 0) {
        const auto end = pattern.rfind(pattern[0]);
        if (end != 0) {
            modifier = pattern.substr(end + 1);
            modifier_escape = pattern[0];
            pattern = pattern.substr(1, end - 1);
        }
    }
    auto ctx = parse_context{pattern};
    if (!ctx.parse(modifier)) return {{}, ctx.error_message};
    auto generation = generation_context{};
    auto source = "public " + std::string{name} + "_matcher: <CharT> type = {\n"
        "  context: <Iter> type == cpp2::regex::match_context<CharT, Iter, " +
        std::to_string(ctx.get_cur_group()) + ">;\n";
    source += generation.run(ctx.get_as_token());
    source += "  entry: <Iter> (cur: Iter, inout ctx: context<Iter>) "
              "-> cpp2::regex::match_return<Iter> = {\n"
              "    ctx..set_group_start(0, cur);\n"
              "    r := " + generation.entry_func + "(cur, ctx, cpp2::regex::true_end_func());\n"
              "    if r.matched { ctx..set_group_end(0, r.pos); }\n"
              "    return r;\n"
              "  }\n";
    source += named_group_lookup(ctx.named_groups);
    source += "  to_string: () -> std::string = { return " +
        raw_string_literal(modifier_escape + ctx.get_as_token()->to_string() + modifier_escape +
                           modifier) + "; }\n";
    source += "}\n";
    return {std::move(source), {}};
}

}

auto is_regex_member_name(std::string_view name) -> bool {
    return name == "regex" || name.starts_with("regex_");
}

auto apply_regex(ir_declaration& type, std::vector<diagnostic>& diagnostics) -> std::string {
    const auto error = [&](source_span span, std::string message) {
        diagnostics.push_back({"regex-metafunction", diagnostic_severity::error, span,
                               std::move(message)});
    };
    // Ordered by name, as upstream generates them.
    auto expressions = std::map<std::string, std::pair<std::string, source_span>>{};
    std::erase_if(type.members, [&](const ir_declaration& member) {
        if (member.kind != declaration_kind::object || !member.alias.empty() ||
            !is_regex_member_name(member.name)) {
            return false;
        }
        const auto& text = member.body.source();
        if (text.empty()) {
            error(member.span, "Regular expression must have an initializer.");
        }
        else if (member.body.tree().tokens.size() != 1) {
            error(member.span, "Unknown string format '" + text + "'");
        }
        else if (text.starts_with("R\"(") && text.ends_with(")\"") && text.size() >= 5) {
            expressions[member.name] = {text.substr(3, text.size() - 5), member.span};
        }
        else if (text.size() >= 2 && text.starts_with('"') && text.ends_with('"')) {
            expressions[member.name] = {text.substr(1, text.size() - 2), member.span};
        }
        else {
            error(member.span, "Unknown string format '" + text + "'");
        }
        return true;
    });
    auto generated = std::string{};
    for (const auto& [name, expression] : expressions) {
        auto matcher = generate_regex_matcher(name, expression.first);
        if (!matcher.error.empty()) {
            error(expression.second, std::move(matcher.error));
            continue;
        }
        generated += matcher.cpp2 + "public " + name +
            ": cpp2::regex::regular_expression<char, " + name + "_matcher<char>> = ();\n";
    }
    return generated;
}

}
