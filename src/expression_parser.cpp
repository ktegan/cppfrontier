#include <cppfrontier/expression.hpp>
#include <cppfrontier/type.hpp>

#include "brackets.hpp"
#include "cpp1_text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <utility>

namespace cppfrontier {
namespace {

// `>` is always a single token so nested template argument lists close one
// level at a time; `>>`, `>=` and `>>=` are recognized from joined tokens.
constexpr std::array<std::string_view, 26> punctuators{{
    "<<=", "<=>", "...", "..<", "..=", "->*", "->", "++", "--", "<<", "<=", "==", "!=",
    "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "::", ".*", "..",
}};

auto is_identifier_start(char value) -> bool {
    return std::isalpha(static_cast<unsigned char>(value)) || value == '_';
}

auto tokenize(std::string_view text, expression_text& result) -> void {
    auto leading = std::string{};
    auto position = std::size_t{0};
    while (position < text.size()) {
        const auto value = text[position];
        if (std::isspace(static_cast<unsigned char>(value))) {
            leading += value;
            ++position;
            continue;
        }
        const auto lexeme = detail::lexeme_at(text, position);
        if (lexeme.kind == detail::lexeme_kind::line_comment ||
            lexeme.kind == detail::lexeme_kind::block_comment) {
            leading.append(text, position, lexeme.end - position);
            position = lexeme.end;
            continue;
        }
        auto token = expression_token{};
        auto end = position + 1;
        if (lexeme.kind != detail::lexeme_kind::none) {
            end = lexeme.end;
            token.kind = lexeme.kind == detail::lexeme_kind::number ? expression_token_kind::number
                : lexeme.kind == detail::lexeme_kind::character ? expression_token_kind::character
                : expression_token_kind::string;
        }
        else if (is_identifier_start(value)) {
            while (end < text.size() && detail::is_identifier_continue(text[end])) ++end;
            token.kind = expression_token_kind::identifier;
        }
        else {
            const auto rest = text.substr(position);
            const auto found = std::find_if(punctuators.begin(), punctuators.end(),
                [&](auto candidate) { return rest.starts_with(candidate); });
            if (found != punctuators.end()) end = position + found->size();
        }
        token.text = std::string{text.substr(position, end - position)};
        token.leading = std::exchange(leading, {});
        // `not` followed by whitespace is the operator `!`.
        if (token.text == "not" && end < text.size() &&
            std::isspace(static_cast<unsigned char>(text[end]))) {
            token.spelling = std::exchange(token.text, "!");
            token.kind = expression_token_kind::punctuation;
        }
        result.tokens.push_back(std::move(token));
        position = end;
    }
    result.trailing = std::move(leading);
}

// Statement words that may also name objects.
constexpr std::array<std::string_view, 4> contextual_names{{"type", "pre", "post", "assert"}};

// The passing styles an argument may name, as `f(out x)`.
constexpr std::array<std::string_view, 3> passing_words{{"move", "forward", "out"}};

constexpr std::array<std::string_view, 7> parameter_passing_words{{
    "in", "in_ref", "copy", "inout", "out", "move", "forward",
}};

constexpr std::array<std::string_view, 11> assignment_operators{{
    "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=",
}};

constexpr std::array<std::array<std::string_view, 5>, 10> binary_levels{{
    {"||"}, {"&&"}, {"|"}, {"^"}, {"&"}, {"==", "!="}, {"<", ">", "<=", ">=", "<=>"},
    {"<<", ">>"}, {"+", "-"}, {"*", "/", "%"},
}};

template<std::size_t N>
auto contains(const std::array<std::string_view, N>& values, std::string_view value) -> bool {
    return !value.empty() && std::find(values.begin(), values.end(), value) != values.end();
}

class parser final {
public:
    parser(const std::vector<expression_token>& tokens, bool try_operator, bool comprehension,
           bool bindings = false)
        : tokens_{tokens}, try_operator_{try_operator}, comprehension_{comprehension},
          bindings_{bindings}, template_closes_{template_closes(tokens)} {}

    auto sequence(std::size_t begin, std::size_t end) -> expression {
        const auto saved = std::pair{position_, end_};
        position_ = begin;
        end_ = end;
        auto result = expression{expression_kind::sequence, begin, end, 0, {}};
        while (position_ < end_) {
            const auto start = position_;
            const auto after_separator = start > 0 &&
                (tokens_[start - 1].text == "(" || tokens_[start - 1].text == ",");
            auto parsed = after_separator ? item() : assignment();
            if (parsed && position_ > start) {
                result.children.push_back(*std::move(parsed));
            }
            else {
                position_ = start + 1;
            }
        }
        std::tie(position_, end_) = saved;
        return result;
    }

    // One inspect alternative that is every token.
    auto whole_alternative() -> std::optional<expression> {
        position_ = 0;
        end_ = tokens_.size();
        auto result = alternative(end_);
        if (position_ != end_) return std::nullopt;
        return result;
    }

private:
    // The close of each template argument list in `tokens`, indexed by its `<`.
    static auto template_closes(const std::vector<expression_token>& tokens)
        -> std::vector<std::optional<std::size_t>> {
        const auto text = [&](std::size_t index) -> std::string_view {
            return index < tokens.size() ? std::string_view{tokens[index].text}
                                         : std::string_view{};
        };
        // `>=` and `>>=` are spelled by joined tokens, and are operators that
        // never close a template argument list.
        const auto joined = [&](std::size_t index, std::string_view value) {
            return text(index) == value && tokens[index].leading.empty();
        };
        const auto spelling = [&](std::size_t index) -> std::string_view {
            if (text(index) == ">" && joined(index + 1, ">") && joined(index + 2, "=")) {
                return ">>=";
            }
            if (text(index) == ">" && joined(index + 1, "=")) return ">=";
            return text(index);
        };
        const auto operand = [&](std::size_t index) {
            return index < tokens.size() &&
                tokens[index].kind != expression_token_kind::punctuation &&
                !detail::is_statement_word(tokens[index].text);
        };
        return detail::template_argument_closes(spelling, operand, tokens.size());
    }

    const std::vector<expression_token>& tokens_;
    // `?` is the postfix propagation operator rather than a conditional's.
    bool try_operator_ = false;
    // `for` begins a for-comprehension.
    bool comprehension_ = false;
    // An `is` may be a pattern test with bindings.
    bool bindings_ = false;
    std::size_t position_ = 0;
    std::size_t end_ = 0;
    std::vector<std::optional<std::size_t>> template_closes_;

    [[nodiscard]] auto text(std::size_t index) const -> std::string_view {
        return index < end_ ? std::string_view{tokens_[index].text} : std::string_view{};
    }
    [[nodiscard]] auto at(std::string_view value) const -> bool { return text(position_) == value; }
    [[nodiscard]] auto joined(std::size_t index) const -> bool {
        return index < end_ && tokens_[index].leading.empty();
    }
    [[nodiscard]] auto kind(std::size_t index) const -> std::optional<expression_token_kind> {
        if (index >= end_) return std::nullopt;
        return tokens_[index].kind;
    }
    [[nodiscard]] auto is_operand_token(std::size_t index) const -> bool {
        const auto value = kind(index);
        return value && *value != expression_token_kind::punctuation &&
            !detail::is_statement_word(text(index));
    }

    // The operator at `index`, joining `>` with following `>` and `=`.
    [[nodiscard]] auto operator_at(std::size_t index) const -> std::pair<std::string, std::size_t> {
        if (text(index) != ">") return {std::string{text(index)}, 1};
        if (text(index + 1) == ">" && joined(index + 1)) {
            if (text(index + 2) == "=" && joined(index + 2)) return {">>=", 3};
            return {">>", 2};
        }
        if (text(index + 1) == "=" && joined(index + 1)) return {">=", 2};
        return {">", 1};
    }

    auto item() -> std::optional<expression> {
        const auto start = position_;
        if (contains(passing_words, text(start)) &&
            kind(start + 1) == expression_token_kind::identifier &&
            !detail::is_statement_word(text(start + 1))) {
            ++position_;
            if (auto operand = assignment()) {
                return expression{expression_kind::argument, start, operand->last, start,
                                  {*std::move(operand)}};
            }
            position_ = start;
        }
        return assignment();
    }

    auto assignment() -> std::optional<expression> {
        auto lhs = conditional();
        if (!lhs) return std::nullopt;
        const auto [op, length] = operator_at(position_);
        if (!contains(assignment_operators, op)) return lhs;
        const auto op_index = position_;
        position_ += length;
        auto rhs = assignment();
        if (!rhs) {
            position_ = op_index;
            return lhs;
        }
        const auto first = lhs->first;
        const auto last = rhs->last;
        return expression{expression_kind::assignment, first, last, op_index,
                          {*std::move(lhs), *std::move(rhs)}};
    }

    auto conditional() -> std::optional<expression> {
        auto condition = binary(0);
        if (!condition || !at("?")) return condition;
        const auto question = position_++;
        auto when_true = assignment();
        if (when_true && at(":")) {
            ++position_;
            if (auto when_false = assignment()) {
                const auto first = condition->first;
                const auto last = when_false->last;
                return expression{expression_kind::conditional, first, last, question,
                                  {*std::move(condition), *std::move(when_true),
                                   *std::move(when_false)}};
            }
        }
        position_ = question;
        return condition;
    }

    auto binary(std::size_t level) -> std::optional<expression> {
        if (level == binary_levels.size()) return is_as();
        auto first = binary(level + 1);
        if (!first) return std::nullopt;
        auto chain = expression{expression_kind::binary, first->first, first->last, 0, {}};
        chain.children.push_back(*std::move(first));
        for (;;) {
            const auto [op, length] = operator_at(position_);
            if (!contains(binary_levels[level], op)) break;
            const auto op_index = position_;
            position_ += length;
            auto term = binary(level + 1);
            if (!term) {
                position_ = op_index;
                break;
            }
            chain.children.push_back(*std::move(term));
        }
        if (chain.children.size() == 1) return std::move(chain.children.front());
        chain.last = chain.children.back().last;
        return chain;
    }

    auto is_as() -> std::optional<expression> {
        auto operand = prefix();
        if (!operand) return std::nullopt;
        while (at("is") || at("as")) {
            const auto op = position_++;
            // `is` takes a value when its operand is parenthesized, a literal,
            // or signed; anything else is a type-id.
            auto type = pattern(text(op) == "is");
            if (!type) {
                position_ = op;
                break;
            }
            if (bindings_ && text(op) == "is") {
                if (auto test = pattern_test(*operand, op, *type)) return test;
            }
            const auto first = operand->first;
            const auto last = type->last;
            operand = expression{expression_kind::is_as, first, last, op,
                                 {*std::move(operand), *std::move(type)}};
        }
        return operand;
    }

    // The pattern test `subject is matched` followed by `(names)` after a
    // type, `as name`, or both, or with a tuple pattern `matched`; nullopt
    // when it binds nothing. `as` followed by a name that is not qualified or
    // a template is a binding.
    auto pattern_test(expression& subject, std::size_t op, expression& matched)
        -> std::optional<expression> {
        const auto start = position_;
        const auto empty = [&] {
            return expression{expression_kind::sequence, position_, position_, position_, {}};
        };
        auto destructuring = empty();
        if (matched.kind == expression_kind::sequence && at("(")) {
            auto group = primary();
            if (!group || group->kind != expression_kind::group) {
                position_ = start;
                return std::nullopt;
            }
            destructuring = *std::move(group);
        }
        auto alternatives = empty();
        auto binding = empty();
        if (at("as") && kind(position_ + 1) == expression_token_kind::identifier &&
            text(position_ + 2) != "::" && text(position_ + 2) != "<") {
            binding = expression{expression_kind::name, position_ + 1, position_ + 2,
                                 position_ + 1, {}};
            position_ += 2;
        }
        if (position_ == start && !is_tuple_pattern(matched)) return std::nullopt;
        auto alternative = expression{expression_kind::alternative, op, position_, op,
                                      {std::move(matched), std::move(destructuring),
                                       std::move(alternatives), std::move(binding), empty(),
                                       empty()}};
        return expression{expression_kind::pattern_test, subject.first, position_, op,
                          {std::move(subject), std::move(alternative)}};
    }

    // `is` takes a value when its operand is parenthesized, a literal
    // (`true`, `false` and `nullptr` among them), or begins with `-`, `+` or
    // `!`; anything else is a type-id.
    auto pattern(bool allow_value) -> std::optional<expression> {
        const auto value = allow_value &&
            (at("(") || at("-") || at("+") || at("!") || at("true") || at("false") ||
             at("nullptr") ||
             (is_operand_token(position_) && kind(position_) != expression_token_kind::identifier));
        return value ? prefix() : type_id();
    }

    // Parses with the end of input moved to `end`.
    template<class Parse>
    auto bounded(std::size_t end, Parse parse) -> std::optional<expression> {
        const auto saved = std::exchange(end_, std::min(end, end_));
        auto result = parse();
        end_ = saved;
        return result;
    }

    // `for range do (name) [if guard] yield value`, or `for ...` in place of
    // `yield value` for a nested generator (experiment `comprehension`).
    auto comprehension() -> std::optional<expression> {
        const auto start = position_;
        const auto do_index = top_level(start + 1, [&](std::size_t at) {
            return text(at) == "do" || text(at) == ";" || text(at) == "{";
        });
        if (text(do_index) != "do" || do_index == start + 1 || text(do_index + 1) != "(" ||
            kind(do_index + 2) != expression_token_kind::identifier ||
            detail::is_statement_word(text(do_index + 2)) || text(do_index + 3) != ")") {
            return std::nullopt;
        }
        position_ = start + 1;
        auto range = bounded(do_index, [&] { return assignment(); });
        if (!range || position_ != do_index) {
            position_ = start;
            return std::nullopt;
        }
        auto result = expression{expression_kind::comprehension, start, start, start,
                                 {*std::move(range),
                                  expression{expression_kind::name, do_index + 2, do_index + 3,
                                             do_index + 2, {}}}};
        position_ = do_index + 4;
        if (at("if")) {
            const auto body = top_level(position_ + 1, [&](std::size_t at) {
                return text(at) == "for" || text(at) == "yield" || text(at) == ";";
            });
            ++position_;
            auto guard = bounded(body, [&] { return assignment(); });
            if (!guard || position_ != body) {
                position_ = start;
                return std::nullopt;
            }
            result.children.push_back(*std::move(guard));
        }
        result.op = position_;
        auto value = std::optional<expression>{};
        if (at("for")) {
            value = comprehension();
        }
        else if (at("yield")) {
            ++position_;
            value = assignment();
        }
        if (!value) {
            position_ = start;
            return std::nullopt;
        }
        result.children.push_back(*std::move(value));
        result.last = position_;
        return result;
    }

    // `inspect constexpr? subject -> type { alternatives }`.
    auto inspect() -> std::optional<expression> {
        const auto start = position_;
        auto subject_begin = start + 1;
        if (text(subject_begin) == "constexpr") ++subject_begin;
        const auto arrow = top_level(subject_begin, [&](std::size_t at) {
            return text(at) == "->" || text(at) == ";" || text(at) == "{";
        });
        if (text(arrow) != "->") return std::nullopt;
        auto brace = arrow + 1;
        while (brace < end_ && text(brace) != "{" && text(brace) != ";") ++brace;
        const auto close = text(brace) == "{" ? matching_close(brace) : std::nullopt;
        if (brace == arrow + 1 || !close) return std::nullopt;
        position_ = subject_begin;
        auto subject = bounded(arrow, [&] { return assignment(); });
        if (!subject || position_ != arrow) {
            position_ = start;
            return std::nullopt;
        }
        auto result = expression{expression_kind::inspect, start, *close + 1, start,
                                 {*std::move(subject), sequence(arrow + 1, brace)}};
        position_ = brace + 1;
        while (position_ < *close) {
            auto parsed = alternative(*close);
            if (!parsed) {
                position_ = start;
                return std::nullopt;
            }
            result.children.push_back(*std::move(parsed));
        }
        position_ = *close + 1;
        return result;
    }

    // `is pattern = value;` or `as type = value;`, ending before `close`.
    // An `is` alternative may have the `patterns` experiment's forms,
    // `is T(a, _) | U as c if g = value;`, which fill the slots between the
    // pattern and the value (see `expression_kind::alternative`).
    auto alternative(std::size_t close) -> std::optional<expression> {
        if (!at("is") && !at("as")) return std::nullopt;
        const auto op = position_++;
        auto matched = bounded(close, [&] { return pattern(text(op) == "is"); });
        if (!matched) return std::nullopt;
        auto result = expression{expression_kind::alternative, op, op, op, {*std::move(matched)}};
        if (text(op) == "is" && (at("(") || at("|") || at("as") || at("if"))) {
            const auto empty = [&] {
                return expression{expression_kind::sequence, position_, position_, position_, {}};
            };
            // The slot `word parsed`, or an empty one without `word`.
            const auto slot = [&](std::string_view word, auto parse) {
                if (!at(word)) {
                    result.children.push_back(empty());
                    return true;
                }
                if (word != "(") ++position_;
                auto parsed = bounded(close, parse);
                if (parsed) result.children.push_back(*std::move(parsed));
                return parsed.has_value();
            };
            if (!slot("(", [&] { return primary(); })) return std::nullopt;
            auto alternatives = empty();
            while (at("|")) {
                ++position_;
                auto parsed = bounded(close, [&] { return pattern(true); });
                if (!parsed) return std::nullopt;
                alternatives.children.push_back(*std::move(parsed));
            }
            alternatives.last = position_;
            result.children.push_back(std::move(alternatives));
            if (!slot("as", [&] { return name(); }) ||
                !slot("if", [&] { return conditional(); })) {
                return std::nullopt;
            }
        }
        if (!at("=")) return std::nullopt;
        ++position_;
        auto value = bounded(close, [&] { return assignment(); });
        // Validation reports a missing `;` before the close.
        if (!value || (!at(";") && position_ != close)) return std::nullopt;
        if (at(";")) ++position_;
        result.children.push_back(*std::move(value));
        result.last = position_;
        return result;
    }

    auto type_id() -> std::optional<expression> {
        const auto begin = position_;
        while (at("const") || at("*") || at("&")) ++position_;
        if (at("(")) {
            const auto close = matching_close(position_);
            if (!close) {
                position_ = begin;
                return std::nullopt;
            }
            position_ = *close + 1;
        }
        else if (is_operand_token(position_) && kind(position_) != expression_token_kind::identifier) {
            ++position_;
        }
        else if (!name()) {
            position_ = begin;
            return std::nullopt;
        }
        else if ((text(position_ - 1) == "decltype" || text(position_ - 1) == "type_of") &&
                 at("(")) {
            const auto close = matching_close(position_);
            if (!close) {
                position_ = begin;
                return std::nullopt;
            }
            position_ = *close + 1;
        }
        const auto end = position_;
        auto result = sequence(begin, end);
        position_ = end;
        return result;
    }

    auto prefix() -> std::optional<expression> {
        const auto start = position_;
        while (at("-") || at("+") || at("!") || at("~") || at("++") || at("--") || at("*") ||
               at("&")) {
            ++position_;
        }
        auto operand = postfix();
        if (!operand) {
            position_ = start;
            return std::nullopt;
        }
        if (start != operand->first) {
            const auto last = operand->last;
            operand = expression{expression_kind::prefix, start, last, start, {*std::move(operand)}};
        }
        if (at("..<") || at("..=")) {
            const auto op = position_++;
            if (auto rhs = assignment()) {
                const auto last = rhs->last;
                return expression{expression_kind::range, start, last, op,
                                  {*std::move(operand), *std::move(rhs)}};
            }
            position_ = op;
        }
        return operand;
    }

    auto postfix() -> std::optional<expression> {
        auto operand = primary();
        if (!operand) return std::nullopt;
        for (;;) {
            const auto op = position_;
            const auto value = text(op);
            if (auto call = value == "(" ? type_id_call(*operand) : std::nullopt) {
                operand = *std::move(call);
            }
            else if (value == "(" || value == "[" || value == "{") {
                auto items = list();
                if (!items) break;
                const auto kind = value == "[" ? expression_kind::subscript : expression_kind::call;
                const auto first = operand->first;
                items->insert(items->begin(), *std::move(operand));
                operand = expression{kind, first, position_, op, *std::move(items)};
            }
            else if (value == "." || value == ".." ||
                     (value == "::" && is_decltype(*operand))) {
                ++position_;
                auto member = name();
                if (!member) {
                    position_ = op;
                    break;
                }
                const auto first = operand->first;
                const auto last = member->last;
                operand = expression{expression_kind::member, first, last, op,
                                     {*std::move(operand), *std::move(member)}};
            }
            else if (value == "++" || value == "--" || value == "..." || value == "$" ||
                     (value == "?" && try_operator_) || value == "~" ||
                     ((value == "*" || value == "&") && is_postfix_dereference(op))) {
                ++position_;
                const auto first = operand->first;
                operand = expression{expression_kind::postfix, first, position_, op,
                                     {*std::move(operand)}};
            }
            else {
                break;
            }
        }
        return operand;
    }

    // `sizeof`, `alignof` or `typeid` whose argument begins with `*` or
    // `const` takes a type-id, as a template argument does: `sizeof(*int)`.
    // The argument is a sequence.
    auto type_id_call(expression& callee) -> std::optional<expression> {
        const auto open = position_;
        const auto word = text(callee.first);
        if (callee.kind != expression_kind::name || callee.last != callee.first + 1 ||
            (word != "sizeof" && word != "alignof" && word != "typeid") ||
            (text(open + 1) != "*" && text(open + 1) != "const")) {
            return std::nullopt;
        }
        const auto close = matching_close(open);
        if (!close) return std::nullopt;
        position_ = *close + 1;
        return expression{expression_kind::call, callee.first, position_, open,
                          {std::move(callee), sequence(open + 1, *close)}};
    }

    // `decltype(e)`, which `::` may follow.
    [[nodiscard]] auto is_decltype(const expression& node) const -> bool {
        return node.kind == expression_kind::call &&
            node.children.front().kind == expression_kind::name &&
            node.children.front().last == node.first + 1 && text(node.first) == "decltype";
    }

    // `*` and `&` are binary when followed by an operand; `p*()` calls `*p`.
    [[nodiscard]] auto is_postfix_dereference(std::size_t op) const -> bool {
        const auto next = text(op + 1);
        if (next == "(") return text(op + 2) == ")";
        // An `inspect` expression is an operand, so `2 * inspect x -> ...` multiplies,
        // and `!` and the `:` of a typed value `:T = v` only begin one.
        return !is_operand_token(op + 1) && next != "inspect" && next != "::" && next != "!" &&
            next != ":";
    }

    auto primary() -> std::optional<expression> {
        const auto start = position_;
        const auto value = kind(start);
        if (!value) return std::nullopt;
        if (*value == expression_token_kind::identifier) {
            if (text(start) == "inspect") return inspect();
            if (text(start) == "for" && comprehension_) return comprehension();
            if (text(start) == "requires") return requires_expression();
            if (is_type_marker(start)) return name();
            // `in(...)` calls a function named `in`, and a contextual word such
            // as `type` or `pre` is a name where an operand is expected.
            if (detail::is_statement_word(text(start)) &&
                !(text(start) == "in" && text(start + 1) == "(") &&
                !contains(contextual_names, text(start))) {
                return std::nullopt;
            }
            if (kind(start + 1) == expression_token_kind::string && joined(start + 1)) {
                position_ += 2;
                return literal(start);
            }
            return name();
        }
        if (*value == expression_token_kind::string) {
            ++position_;
            return literal(start);
        }
        if (*value != expression_token_kind::punctuation) {
            ++position_;
            return expression{expression_kind::literal, start, position_, start, {}};
        }
        if (at("::")) return name();
        if (at("(")) {
            if (auto parsed = fold()) return parsed;
            auto items = list();
            if (!items) return std::nullopt;
            return expression{expression_kind::group, start, position_, start, *std::move(items)};
        }
        // After a name, `:` begins a declaration rather than a typed value.
        if (at(":") && (start == 0 || !(is_operand_token(start - 1) ||
                                        text(start - 1) == ")" || text(start - 1) == "]" ||
                                        text(start - 1) == ">"))) {
            return typed_value();
        }
        return std::nullopt;
    }

    // `(pack op ...)`, `(... op pack)` or `(a op ... op b)`, where `op` is a
    // binary or assignment operator or `,` (`*` and `&` only after `...`), and each operand is a prefix
    // expression, possibly with `is` and `as`.
    auto fold() -> std::optional<expression> {
        const auto open = position_;
        const auto close = matching_close(open);
        if (!close) return std::nullopt;
        // The length of the fold operator at `index`, or 0.
        const auto operator_length = [&](std::size_t index) -> std::size_t {
            const auto [op, length] = operator_at(index);
            const auto binary = std::any_of(binary_levels.begin(), binary_levels.end(),
                [&](const auto& level) { return contains(level, op); });
            return binary || op == "," || contains(assignment_operators, op) ? length : 0;
        };
        // The start of the fold operator ending at `end`, or `end`. A `*` or
        // `&` before `...` is postfix, as before any token that is not an operand.
        const auto operator_before = [&](std::size_t end) {
            for (auto length = std::size_t{3}; length != 0; --length) {
                if (end > open + length && operator_length(end - length) == length &&
                    !((text(end - 1) == "*" || text(end - 1) == "&") && length == 1 &&
                      is_postfix_dereference(end - 1))) {
                    return end - length;
                }
            }
            return end;
        };
        const auto is_ellipsis = [&](std::size_t at) { return text(at) == "..."; };
        for (auto ellipsis = top_level(open + 1, is_ellipsis); ellipsis < *close;
             ellipsis = top_level(ellipsis + 1, is_ellipsis)) {
            // Each side of `...` is empty, or an operator and an operand; with
            // both, the operators are the same.
            const auto left = operator_before(ellipsis);
            const auto right = ellipsis + 1 + operator_length(ellipsis + 1);
            const auto has_left = ellipsis != open + 1;
            const auto has_right = ellipsis + 1 != *close;
            if ((has_left && left == ellipsis) || (has_right && right == ellipsis + 1) ||
                (!has_left && !has_right) ||
                (has_left && has_right &&
                 operator_at(left).first != operator_at(ellipsis + 1).first)) {
                continue;
            }
            auto result = expression{expression_kind::fold, open, *close + 1, ellipsis, {}};
            const auto operand = [&](std::size_t begin, std::size_t end) {
                position_ = begin;
                auto parsed = bounded(end, [&] { return is_as(); });
                if (!parsed || position_ != end) return false;
                result.children.push_back(*std::move(parsed));
                return true;
            };
            if ((!has_left || operand(open + 1, left)) && (!has_right || operand(right, *close))) {
                position_ = *close + 1;
                return result;
            }
        }
        position_ = open;
        return std::nullopt;
    }

    // `requires (parameters) { requirements }` or `requires { requirements }`.
    auto requires_expression() -> std::optional<expression> {
        const auto start = position_++;
        auto result = expression{expression_kind::requires_expression, start, start, start, {}};
        if (const auto close = at("(") ? matching_close(position_) : std::nullopt) {
            for (auto begin = position_ + 1; begin < *close;) {
                const auto end = detail::find_top_level(tokens_, begin, *close,
                    [&](std::size_t open) { return detail::angle_close(tokens_, open, *close); },
                    [&](std::size_t at) { return text(at) == ","; });
                if (end > begin) result.children.push_back(parameter(begin, end));
                begin = end + 1;
            }
            position_ = *close + 1;
        }
        const auto close = at("{") ? matching_close(position_) : std::nullopt;
        if (!close) {
            position_ = start;
            return std::nullopt;
        }
        ++position_;
        while (position_ < *close) result.children.push_back(requirement(*close));
        position_ = *close + 1;
        result.last = position_;
        return result;
    }

    // `passing? name: type` in [begin, end), or a sequence for a parameter in
    // Cpp1 syntax, which validation rejects.
    auto parameter(std::size_t begin, std::size_t end) -> expression {
        const auto name = contains(parameter_passing_words, text(begin)) ? begin + 1 : begin;
        if (kind(name) == expression_token_kind::identifier && text(name + 1) == ":" &&
            name + 2 < end) {
            return expression{expression_kind::parameter, begin, end, name,
                              {sequence(name + 2, end)}};
        }
        return expression{expression_kind::sequence, begin, end, begin, {}};
    }

    // One requirement, through its `;` or up to `close`.
    auto requirement(std::size_t close) -> expression {
        const auto begin = position_;
        const auto end = std::min(close, top_level(begin, [&](std::size_t at) {
            return text(at) == ";";
        }));
        auto children = std::vector<expression>{};
        const auto part = [&](std::size_t stop, auto parse) {
            if (auto parsed = bounded(stop, parse)) children.push_back(*std::move(parsed));
        };
        const auto brace = at("{") ? matching_close(position_) : std::nullopt;
        if (brace && *brace < end) {
            ++position_;
            part(*brace, [&] { return assignment(); });
            if (position_ == *brace) {
                ++position_;
                if (at("noexcept")) ++position_;
                if (at("->")) {
                    ++position_;
                    part(end, [&] { return type_id(); });
                }
            }
        }
        else if (at("typename")) {
            ++position_;
            part(end, [&] { return type_id(); });
        }
        else {
            if (at("requires")) ++position_;
            part(end, [&] { return assignment(); });
        }
        if (position_ != end) children = {sequence(begin, end)};
        position_ = end < close ? end + 1 : end;
        return expression{expression_kind::requirement, begin, position_, begin,
                          std::move(children)};
    }

    auto literal(std::size_t start) -> expression {
        while (kind(position_) == expression_token_kind::string) ++position_;
        // A user-defined literal suffix, as in `"abc"s`.
        if (kind(position_) == expression_token_kind::identifier && joined(position_)) {
            ++position_;
        }
        return expression{expression_kind::literal, start, position_, start, {}};
    }

    // `: type = value`; unnamed functions are left to the enclosing sequence.
    auto typed_value() -> std::optional<expression> {
        const auto start = position_;
        const auto type_begin = start + 1;
        if (text(type_begin) == "(" || text(type_begin) == "<") return std::nullopt;
        const auto equals = detail::find_top_level(tokens_, type_begin, end_,
            [&](std::size_t open) { return detail::angle_close(tokens_, open, end_); },
            [&](std::size_t at) {
                return text(at) == "=" || text(at) == "," || text(at) == ";" || text(at) == "{";
            });
        if (text(equals) != "=" || equals == type_begin) return std::nullopt;
        auto type = sequence(type_begin, equals);
        position_ = equals + 1;
        auto value = assignment();
        if (!value) {
            position_ = start;
            return std::nullopt;
        }
        const auto last = value->last;
        return expression{expression_kind::typed_value, start, last, equals,
                          {std::move(type), *std::move(value)}};
    }

    // `type` before a name marks it a dependent type, `type T::x`.
    [[nodiscard]] auto is_type_marker(std::size_t index) const -> bool {
        const auto previous = index == 0 ? std::string_view{} : text(index - 1);
        return text(index) == "type" && previous != "." && previous != ".." &&
            previous != "->" && previous != "::" &&
            (text(index + 1) == "::" || kind(index + 1) == expression_token_kind::identifier);
    }

    // An id-expression with template argument lists, possibly marked a type:
    // `type T::x`.
    auto name() -> std::optional<expression> {
        const auto start = position_;
        auto result = expression{expression_kind::name, start, start, start, {}};
        if (is_type_marker(position_)) ++position_;
        if (at("::")) ++position_;
        for (;;) {
            if (kind(position_) != expression_token_kind::identifier) {
                position_ = start;
                return std::nullopt;
            }
            ++position_;
            if (text(position_ - 1) == "operator") operator_symbol();
            else if (at("<")) {
                if (const auto close = template_close(position_)) {
                    if (*close > position_ + 1) {
                        result.children.push_back(sequence(position_ + 1, *close));
                    }
                    position_ = *close + 1;
                }
            }
            if (!at("::") || kind(position_ + 1) != expression_token_kind::identifier) break;
            ++position_;
        }
        result.last = position_;
        return result;
    }

    // The symbol after `operator`: `()`, `[]`, or a punctuator such as `==`,
    // with `>` joined to a following `>` or `=`.
    auto operator_symbol() -> void {
        if ((at("(") && text(position_ + 1) == ")") || (at("[") && text(position_ + 1) == "]")) {
            position_ += 2;
            return;
        }
        if (kind(position_) != expression_token_kind::punctuation || at("(") || at("[") ||
            at(",") || at(";")) {
            return;
        }
        const auto greater = at(">");
        ++position_;
        while (greater && joined(position_) && (at(">") || at("="))) ++position_;
    }

    // The `>` closing the template argument list opened at `open`, when the
    // text reads as one before the end of the input.
    [[nodiscard]] auto template_close(std::size_t open) const -> std::optional<std::size_t> {
        const auto close = template_closes_[open];
        return close && *close < end_ ? close : std::nullopt;
    }

    // The close of the template list the `<` at `open` begins, if it begins one.
    [[nodiscard]] auto list_close(std::size_t open) const -> std::optional<std::size_t> {
        if (open == 0) return std::nullopt;
        if (text(open - 1) == ":") return detail::angle_close(tokens_, open, end_);
        if (kind(open - 1) == expression_token_kind::identifier) return template_close(open);
        return std::nullopt;
    }

    // The first position from `begin` outside brackets and template lists at
    // which `stop` holds or a closer leaves the input.
    template<class Stop>
    [[nodiscard]] auto top_level(std::size_t begin, Stop stop) const -> std::size_t {
        return detail::find_top_level(tokens_, begin, end_,
            [&](std::size_t open) { return list_close(open); }, stop);
    }

    [[nodiscard]] auto matching_close(std::size_t open) const -> std::optional<std::size_t> {
        return detail::bracket_close(tokens_, open, end_);
    }

    // `(items)`, `[items]` or `{items}`; leaves the position after the close.
    auto list() -> std::optional<std::vector<expression>> {
        const auto open = position_;
        const auto close = text(open) == "(" ? ")" : text(open) == "[" ? "]" : "}";
        ++position_;
        auto items = std::vector<expression>{};
        while (!at(close)) {
            auto parsed = item();
            if (!parsed) {
                position_ = open;
                return std::nullopt;
            }
            items.push_back(*std::move(parsed));
            if (at(",")) {
                ++position_;
            }
            else if (!at(close)) {
                position_ = open;
                return std::nullopt;
            }
        }
        ++position_;
        return items;
    }
};

}

namespace {

// The first token of an operand in `node` that directly follows another one.
// A literal's attached suffix and a conversion function's type after
// `operator` are not operands.
auto juxtaposed_operand(const expression_text& text, const expression& node)
    -> std::optional<std::size_t> {
    const auto is_operand = [&](const expression& child) {
        const auto& first = text.tokens[child.first];
        return child.kind != expression_kind::sequence &&
            (first.kind != expression_token_kind::punctuation || first.text == "(" ||
             child.kind == expression_kind::typed_value || child.kind == expression_kind::prefix) &&
            !detail::is_statement_word(first.text);
    };
    for (auto index = std::size_t{0}; index < node.children.size(); ++index) {
        const auto& child = node.children[index];
        if (node.kind == expression_kind::sequence && index != 0) {
            const auto& previous = node.children[index - 1];
            const auto& last = text.tokens[previous.last - 1];
            const auto is_suffix = last.kind != expression_token_kind::identifier &&
                last.kind != expression_token_kind::punctuation &&
                text.tokens[child.first].leading.empty();
            if (previous.last == child.first && is_operand(previous) && is_operand(child) &&
                !is_suffix && last.text != "operator") {
                return child.first;
            }
        }
        // Template arguments and the types of typed values and parameters are
        // not values.
        if (node.kind == expression_kind::name || node.kind == expression_kind::parameter ||
            (index == 0 && node.kind == expression_kind::typed_value)) {
            continue;
        }
        if (const auto found = juxtaposed_operand(text, child)) return found;
    }
    return std::nullopt;
}

}

auto find_juxtaposed_operand(const expression_text& text) -> std::optional<std::size_t> {
    return juxtaposed_operand(text, text.root);
}

namespace {

auto comprehension_heads(const expression& node, std::vector<std::size_t>& heads) -> void {
    if (node.kind == expression_kind::comprehension) heads.push_back(node.first);
    for (const auto& child : node.children) comprehension_heads(child, heads);
}

}

auto find_braced_list(const expression_text& text) -> std::optional<std::size_t> {
    for (auto index = std::size_t{1}; index < text.tokens.size(); ++index) {
        const auto& previous = text.tokens[index - 1].text;
        if (text.tokens[index].text == "{" && (previous == "(" || previous == ",")) return index;
    }
    return std::nullopt;
}

namespace {

auto comma_list(const expression_text& text, const expression& node, bool list)
    -> std::optional<std::size_t> {
    // The parenthesized value of `throw` is checked by validation.
    if (node.first > 0 && text.tokens[node.first - 1].text == "throw") return std::nullopt;
    const auto is_list = node.kind == expression_kind::group && node.children.size() > 1;
    if (is_list && !list) return node.first;
    for (std::size_t index = 0; index < node.children.size(); ++index) {
        const auto last = index + 1 == node.children.size();
        const auto assigned = node.kind == expression_kind::assignment &&
            text.tokens[node.op].text == "=" && last;
        // An inspect alternative's patterns are checked as patterns; its value
        // initializes the result type, so it may be a list.
        if (node.kind == expression_kind::alternative && !last) continue;
        // Parentheses around one value keep its context: `= ((1, 2))`.
        const auto child_list = is_list || assigned ||
            (node.kind == expression_kind::alternative && last) ||
            (node.kind == expression_kind::group && list) ||
            (node.kind == expression_kind::typed_value && last) ||
            (node.kind == expression_kind::comprehension && index == 0) ||
            (node.kind == expression_kind::sequence && list);
        if (const auto found = comma_list(text, node.children[index], child_list)) return found;
    }
    return std::nullopt;
}

}

auto find_comma_list(const expression_text& text, bool list) -> std::optional<std::size_t> {
    return comma_list(text, text.root, list);
}

namespace {

// The operator of the first assignment in `node` used as a value; an
// assignment that is `node` itself is one only when `value`.
auto nested_assignment(const expression& node, bool value) -> std::optional<std::size_t> {
    if (node.kind == expression_kind::assignment && value) return node.op;
    for (const auto& child : node.children) {
        // A requirement `a = b;` checks that the assignment is valid, and
        // parentheses do not use a value: `next (i += 1)`.
        const auto child_value = node.kind != expression_kind::requirement &&
            (node.kind != expression_kind::sequence || value) &&
            (node.kind != expression_kind::group || node.children.size() != 1 || value);
        if (const auto found = nested_assignment(child, child_value)) return found;
    }
    return std::nullopt;
}

}

namespace {

auto misleading_postfix(const expression_text& text, const expression& node)
    -> std::optional<std::size_t> {
    // The tokens Cpp1 reads as prefix operators, and the `...` of a fold.
    constexpr auto prefix = std::array<std::string_view, 8>{"+", "-", "*", "&", "~", "++", "--",
                                                            "..."};
    if (node.kind == expression_kind::postfix && node.last < text.tokens.size() &&
        (text.tokens[node.op].text == "*" || text.tokens[node.op].text == "&") &&
        !text.tokens[node.op].leading.empty() && contains(prefix, text.tokens[node.last].text)) {
        // A run `a * ** -1` reports its first operator, the innermost node.
        for (const auto& child : node.children) {
            if (const auto found = misleading_postfix(text, child)) return found;
        }
        return node.op;
    }
    for (const auto& child : node.children) {
        if (const auto found = misleading_postfix(text, child)) return found;
    }
    return std::nullopt;
}

auto type_operand(const expression_text& text, const expression& node)
    -> std::optional<type_operand_use> {
    // Template arguments may be types, as `* i32` in `is_void_v<* i32>`, and so
    // is the type of a typed value, as `:std::vector<int>=(1)`.
    if (node.kind == expression_kind::name) return std::nullopt;
    if (node.kind == expression_kind::typed_value) {
        return node.children.size() == 2 ? type_operand(text, node.children[1]) : std::nullopt;
    }
    const auto cpp2_prefix = node.kind == expression_kind::prefix &&
        (text.tokens[node.op].text == "!" || text.tokens[node.op].text == "+" ||
         text.tokens[node.op].text == "-");
    if (node.kind == expression_kind::binary || cpp2_prefix) {
        for (auto index = std::size_t{0}; index < node.children.size(); ++index) {
            const auto& child = node.children[index];
            if (child.kind == expression_kind::name && child.last == child.first + 1 &&
                is_fundamental_type_name(text.tokens[child.first].text)) {
                const auto& before = node.children[index == 0 ? 0 : index - 1];
                return type_operand_use{child.first, index != 0 &&
                    before.kind == expression_kind::is_as && text.tokens[before.op].text == "is"};
            }
        }
    }
    for (const auto& child : node.children) {
        if (const auto found = type_operand(text, child)) return found;
    }
    return std::nullopt;
}

auto address_arithmetic(const expression_text& text, const expression& node)
    -> std::optional<std::size_t> {
    // A postfix `&`, also in parentheses.
    const auto is_address = [&](const expression& value) {
        const auto* inner = &value;
        while (inner->kind == expression_kind::group && inner->children.size() == 1) {
            inner = &inner->children.front();
        }
        return inner->kind == expression_kind::postfix && text.tokens[inner->op].text == "&";
    };
    const auto additive = [&](std::size_t index) {
        return text.tokens[index].text == "+" || text.tokens[index].text == "-";
    };
    if (node.kind == expression_kind::binary) {
        for (auto index = std::size_t{0}; index < node.children.size(); ++index) {
            const auto& term = node.children[index];
            if (!is_address(term)) continue;
            if (index > 0 && additive(term.first - 1)) return term.first - 1;
            if (index + 1 < node.children.size() && additive(term.last)) return term.last;
        }
    }
    const auto stepped = node.kind == expression_kind::subscript ||
        (node.kind == expression_kind::postfix &&
         (text.tokens[node.op].text == "++" || text.tokens[node.op].text == "--"));
    if (stepped && is_address(node.children.front())) return node.op;
    for (const auto& child : node.children) {
        if (const auto found = address_arithmetic(text, child)) return found;
    }
    return std::nullopt;
}

}

auto find_misleading_postfix(const expression_text& text) -> std::optional<std::size_t> {
    return misleading_postfix(text, text.root);
}

auto find_type_operand(const expression_text& text) -> std::optional<type_operand_use> {
    return type_operand(text, text.root);
}

auto find_address_arithmetic(const expression_text& text) -> std::optional<std::size_t> {
    return address_arithmetic(text, text.root);
}

auto find_value_assignment(const expression_text& text, bool statement)
    -> std::optional<std::size_t> {
    return nested_assignment(text.root, !statement);
}

auto whole_list(const expression_text& text) -> const expression* {
    const auto& root = text.root.kind == expression_kind::sequence &&
            text.root.children.size() == 1
        ? text.root.children.front() : text.root;
    if (root.kind != expression_kind::group || root.children.size() < 2 || root.first != 0) {
        return nullptr;
    }
    for (auto index = root.last; index < text.tokens.size(); ++index) {
        if (text.tokens[index].text != ";") return nullptr;
    }
    return &root;
}

auto find_unparsed_tokens(const expression_text& text) -> std::vector<std::size_t> {
    auto result = std::vector<std::size_t>{};
    auto covered = text.root.first;
    const auto skip_to = [&](std::size_t end) {
        for (; covered < end; ++covered) result.push_back(covered);
    };
    for (const auto& child : text.root.children) {
        skip_to(child.first);
        covered = child.last;
    }
    skip_to(text.root.last);
    if (!result.empty() && result.front() == 0 && text.tokens.front().text == "throw") {
        result.erase(result.begin());
    }
    while (!result.empty() && text.tokens[result.back()].text == ";" &&
           std::all_of(text.tokens.begin() + static_cast<std::ptrdiff_t>(result.back()),
                       text.tokens.end(), [](const auto& token) { return token.text == ";"; })) {
        result.pop_back();
    }
    return result;
}

auto find_stray_for(const expression_text& text) -> std::optional<std::size_t> {
    auto heads = std::vector<std::size_t>{};
    comprehension_heads(text.root, heads);
    for (auto index = std::size_t{0}; index < text.tokens.size(); ++index) {
        if (text.tokens[index].kind == expression_token_kind::identifier &&
            text.tokens[index].text == "for" &&
            std::find(heads.begin(), heads.end(), index) == heads.end()) {
            return index;
        }
    }
    return std::nullopt;
}

auto is_wildcard_pattern(const std::vector<expression_token>& tokens, const expression& node)
    -> bool {
    return node.kind == expression_kind::sequence && node.last == node.first + 1 &&
        tokens[node.first].text == "_";
}

auto is_tuple_pattern(const expression& node) -> bool {
    return node.kind == expression_kind::group && node.children.size() >= 2 &&
        std::all_of(node.children.begin(), node.children.end(), [](const expression& item) {
            return item.kind == expression_kind::name && item.last == item.first + 1;
        });
}

auto inspect_alternative_of(const std::vector<expression_token>& tokens,
                            const expression& alternative) -> inspect_alternative {
    const auto& children = alternative.children;
    const auto& pattern = children.front();
    const auto slots = children.size() == 6;
    const auto present = [&](std::size_t index) {
        return slots && children[index].first != children[index].last ? &children[index]
                                                                       : nullptr;
    };
    auto result = inspect_alternative{{}, present(1), present(3), present(4), &children.back()};
    if (result.destructuring == nullptr && is_tuple_pattern(pattern)) result.destructuring = &pattern;
    else result.patterns.push_back(&pattern);
    if (slots) {
        for (const auto& item : children[2].children) result.patterns.push_back(&item);
    }
    result.wildcard = result.patterns.size() == 1 && is_wildcard_pattern(tokens, pattern);
    return result;
}

auto parse_inspect_alternative(std::string_view text, bool try_operator, bool comprehension)
    -> std::optional<expression_text> {
    auto result = expression_text{};
    tokenize(text, result);
    auto parsed = parser{result.tokens, try_operator, comprehension}.whole_alternative();
    if (!parsed) return std::nullopt;
    result.root = *std::move(parsed);
    return result;
}

auto parse_expression_text(std::string_view text, bool try_operator, bool comprehension,
                           bool bindings) -> expression_text {
    auto result = expression_text{};
    tokenize(text, result);
    result.root = parser{result.tokens, try_operator, comprehension, bindings}
                      .sequence(0, result.tokens.size());
    return result;
}

auto parsed_expression::position_of(std::size_t index) const -> std::optional<source_position> {
    if (!begin_ || index >= tree_.tokens.size()) return std::nullopt;
    auto result = *begin_;
    const auto advance = [&](std::string_view text) {
        for (const auto value : text) {
            if (value == '\n') {
                ++result.line;
                result.column = 1;
            }
            else {
                ++result.column;
            }
        }
    };
    for (auto token = std::size_t{0}; token < index; ++token) {
        advance(tree_.tokens[token].leading);
        const auto& spelled = tree_.tokens[token];
        advance(spelled.spelling.empty() ? spelled.text : spelled.spelling);
    }
    advance(tree_.tokens[index].leading);
    return result;
}

}
