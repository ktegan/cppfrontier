#include "cpp1_expression_renderer.hpp"
#include "cpp1_text.hpp"

#include <cppfrontier/ast.hpp>
#include <cppfrontier/expression.hpp>
#include <cppfrontier/ir.hpp>
#include <cppfrontier/token.hpp>
#include <cppfrontier/type.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cppfrontier {
namespace {

// The objects of the innermost `lazy_objects_scope` on this thread.
thread_local const std::vector<lazy_object>* current_lazy_objects = nullptr;

struct delimiter_depths final {
    std::size_t angle = 0;
    std::size_t parenthesis = 0;
    std::size_t bracket = 0;
    std::size_t brace = 0;

    [[nodiscard]] auto at_top_level() const noexcept -> bool {
        return angle == 0 && parenthesis == 0 && bracket == 0 && brace == 0;
    }
};

auto update_forward_depths(char value, delimiter_depths& depths) -> void {
    switch (value) {
    case '<': ++depths.angle; break;
    case '>': if (depths.angle != 0) --depths.angle; break;
    case '(': ++depths.parenthesis; break;
    case ')': if (depths.parenthesis != 0) --depths.parenthesis; break;
    case '[': ++depths.bracket; break;
    case ']': if (depths.bracket != 0) --depths.bracket; break;
    case '{': ++depths.brace; break;
    case '}': if (depths.brace != 0) --depths.brace; break;
    default: break;
    }
}

constexpr std::array support_names{
    std::string_view{"unchecked_narrow"}, std::string_view{"unchecked_cast"},
    std::string_view{"unchecked_cmp_less"}, std::string_view{"unchecked_cmp_less_eq"},
    std::string_view{"unchecked_cmp_greater"},
    std::string_view{"unchecked_cmp_greater_eq"}, std::string_view{"unchecked_div"},
    std::string_view{"unchecked_dereference"}, std::string_view{"unchecked_subscript"},
    std::string_view{"finally"}, std::string_view{"cpp1_ref"},
    std::string_view{"cpp1_rvalue_ref"},
};

// Support names of experiment `checked-arithmetic`.
constexpr std::array wrapping_names{
    std::string_view{"wrapping_add"}, std::string_view{"wrapping_sub"},
    std::string_view{"wrapping_mul"},
};

// Whether the Cpp1 type `type` is arithmetic, so that a constant converts to it
// without narrowing only before it is forwarded.
auto is_arithmetic_type(std::string_view type) -> bool {
    static constexpr auto fixed_width = std::array<std::string_view, 10>{
        "std::int8_t", "std::int16_t", "std::int32_t", "std::int64_t", "std::uint8_t",
        "std::uint16_t", "std::uint32_t", "std::uint64_t", "std::size_t", "std::ptrdiff_t"};
    static constexpr auto single_words = std::array<std::string_view, 6>{
        "bool", "float", "wchar_t", "char8_t", "char16_t", "char32_t"};
    if (std::find(fixed_width.begin(), fixed_width.end(), type) != fixed_width.end() ||
        std::find(single_words.begin(), single_words.end(), type) != single_words.end()) {
        return true;
    }
    auto words = 0;
    for (auto position = std::size_t{0}; position < type.size();) {
        const auto end = std::min(type.find(' ', position), type.size());
        if (!is_cpp1_fundamental_word(type.substr(position, end - position))) return false;
        ++words;
        position = end + 1;
    }
    return words > 0;
}

auto is_integer_literal(std::string_view text) -> bool {
    if (text.empty() || !std::isdigit(static_cast<unsigned char>(text.front())) ||
        text.find_first_of(".eE") != std::string_view::npos) {
        return false;
    }
    const auto suffix = text.find_first_not_of("0123456789'");
    if (suffix == std::string_view::npos) return true;
    // An optional `u` with an optional `l`, `ll` or `z`, in either order.
    auto rest = text.substr(suffix);
    const auto take_unsigned = [&] {
        if (!rest.empty() && (rest.front() == 'u' || rest.front() == 'U')) rest.remove_prefix(1);
    };
    take_unsigned();
    const auto unsigned_first = rest.size() != text.size() - suffix;
    if (rest.starts_with("ll") || rest.starts_with("LL")) rest.remove_prefix(2);
    else if (!rest.empty() && std::string_view{"lLzZ"}.find(rest.front()) != std::string_view::npos) {
        rest.remove_prefix(1);
    }
    if (!unsigned_first) take_unsigned();
    return rest.empty();
}

// Whether `type` is a known integer type: a Cpp2 or Cpp1 fundamental integer
// spelling (not `bool` or a wide character type), or a `<cstdint>` name such
// as `std::int64_t` or `std::size_t`, possibly `const`. Pointers are not.
auto is_integer_type(const type_id& type) -> bool {
    if (pointer_depth(type) != 0) return false;
    const auto& core = type.core();
    auto text = std::string_view{core.text};
    if (core.kind == type_kind::fundamental) {
        if (const auto cpp1 = cpp1_fundamental_spelling(text); !cpp1.empty()) text = cpp1;
        if (text == "size_t") return true;
    }
    else if (core.kind != type_kind::name) {
        return false;
    }
    if (text.starts_with("std::")) text.remove_prefix(5);
    static constexpr auto cstdint_names = std::array<std::string_view, 14>{
        "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t",
        "uint64_t", "size_t", "ptrdiff_t", "intptr_t", "uintptr_t", "intmax_t", "uintmax_t"};
    if (std::find(cstdint_names.begin(), cstdint_names.end(), text) != cstdint_names.end()) {
        return true;
    }
    if (core.kind != type_kind::fundamental) return false;
    auto words = 0;
    for (auto position = std::size_t{0}; position < text.size();) {
        const auto end = std::min(text.find(' ', position), text.size());
        if (!is_cpp1_fundamental_word(text.substr(position, end - position)) ||
            text.substr(position, end - position) == "double") {
            return false;
        }
        ++words;
        position = end + 1;
    }
    return words > 0;
}

// Whether the number token `text` is an integer literal, including a hexadecimal
// or binary one.
auto is_integer_literal_token(std::string_view text) -> bool {
    if (is_integer_literal(text)) return true;
    return text.size() > 2 && text[0] == '0' &&
        std::string_view{"xXbB"}.find(text[1]) != std::string_view::npos &&
        text.find_first_of(".pP") == std::string_view::npos;
}

// Decides which expressions of a statement are known to have an integer
// type, for the `checked-arithmetic` experiment. Known are an integer
// literal; a local, parameter or data member declared with an integer type,
// a data member named `x`, `this.x` or `that.x`, or a local whose deduced
// type comes from a known integer initializer; `x as T` and `: T = x` with
// an integer `T`; and the arithmetic, shift, bitwise, sign, `~`, `++` and
// `--` operators applied to known operands. Anything else, such as a call, a
// member of another object or a deduced type, is not known and keeps the
// plain operators.
class integer_knowledge final {
public:
    // `bound` names the locals bound within the text, which `scopes` does not know.
    integer_knowledge(const std::vector<expression_token>& tokens, const scope_model* scopes,
                      scope_point point, const std::vector<std::string>& bound = {},
                      int depth = 0)
        : tokens_{tokens}, scopes_{scopes}, point_{point}, bound_{bound}, depth_{depth} {}

    [[nodiscard]] auto known(const expression& node) const -> bool {
        switch (node.kind) {
        case expression_kind::sequence:
            // The root of a parsed text holds the one expression it is.
            return node.children.size() == 1 && node.children.front().first == node.first &&
                node.children.front().last == node.last && known(node.children.front());
        case expression_kind::literal:
            return node.last == node.first + 1 &&
                tokens_[node.first].kind == expression_token_kind::number &&
                is_integer_literal_token(tokens_[node.first].text);
        case expression_kind::name:
            return node.last == node.first + 1 &&
                tokens_[node.first].kind == expression_token_kind::identifier &&
                declared_integer(tokens_[node.first].text);
        case expression_kind::group:
            return node.children.size() == 1 && node.last == node.children.front().last + 1 &&
                known(node.children.front());
        case expression_kind::member:
            return member_integer(node);
        case expression_kind::prefix:
            for (auto index = node.first; index < node.children.front().first; ++index) {
                const auto op = std::string_view{tokens_[index].text};
                if (op != "-" && op != "+" && op != "~") return false;
            }
            return known(node.children.front());
        case expression_kind::postfix:
            return (tokens_[node.op].text == "++" || tokens_[node.op].text == "--") &&
                known(node.children.front());
        case expression_kind::binary:
            for (auto index = std::size_t{1}; index < node.children.size(); ++index) {
                static constexpr auto integer_operators = std::array<std::string_view, 10>{
                    "+", "-", "*", "/", "%", "<<", ">>", "&", "|", "^"};
                const auto op = binary_operator(tokens_, node, index);
                if (std::find(integer_operators.begin(), integer_operators.end(), op) ==
                    integer_operators.end()) {
                    return false;
                }
            }
            return std::all_of(node.children.begin(), node.children.end(),
                               [&](const expression& child) { return known(child); });
        case expression_kind::is_as:
            return tokens_[node.op].text == "as" && is_integer_type(type_of(node.children[1]));
        case expression_kind::typed_value:
            return is_integer_type(type_of(node.children[0]));
        default:
            return false;
        }
    }

private:
    const std::vector<expression_token>& tokens_;
    const scope_model* scopes_;
    scope_point point_;
    const std::vector<std::string>& bound_;
    // Initializers of deduced locals followed so far, which bounds the recursion.
    int depth_;

    [[nodiscard]] auto type_of(const expression& node) const -> type_id {
        return parse_dependent_type(tokens_, node.first, node.last, scopes_, point_);
    }

    [[nodiscard]] auto declared_integer(std::string_view name) const -> bool {
        if (scopes_ == nullptr || !point_.valid() ||
            std::find(bound_.begin(), bound_.end(), name) != bound_.end()) {
            return false;
        }
        return integer_object(scopes_->lookup(name, point_));
    }

    // `this.x` or `that.x`: a data member of the type whose members are visible.
    [[nodiscard]] auto member_integer(const expression& node) const -> bool {
        if (scopes_ == nullptr || !point_.valid() || tokens_[node.op].text != "." ||
            node.children.size() != 2) {
            return false;
        }
        const auto& object = node.children[0];
        const auto& member = node.children[1];
        if (object.kind != expression_kind::name || object.last != object.first + 1 ||
            (tokens_[object.first].text != "this" && tokens_[object.first].text != "that") ||
            member.kind != expression_kind::name || member.last != member.first + 1) {
            return false;
        }
        const auto* type = scopes_->enclosing_type(point_);
        return type != nullptr &&
            integer_object(scopes_->member(*type, tokens_[member.first].text));
    }

    // Whether `found` is an object declared with an integer type, or a local
    // deduced from a known integer initializer.
    [[nodiscard]] auto integer_object(const scope_model::entry* found) const -> bool {
        if (found == nullptr) return false;
        using kind = scope_model::entry_kind;
        if (found->kind != kind::object && found->kind != kind::parameter &&
            found->kind != kind::statement_parameter) {
            return false;
        }
        if (!found->type.is_wildcard()) return is_integer_type(found->type);
        // `x := 1` deduces its type from the initializer.
        const auto* statement = found->declaring_statement;
        if (found->type.constraint() != nullptr || statement == nullptr || depth_ >= 8 ||
            !statement->is<statement::declaration_form>()) {
            return false;
        }
        const auto& declaration = statement->as<statement::declaration_form>().declaration;
        if (!declaration || declaration->initializer.empty()) return false;
        const auto& initializer = declaration->initializer.tree();
        return integer_knowledge{initializer.tokens, scopes_, scopes_->at(*statement), {},
                                 depth_ + 1}
            .known(initializer.root);
    }
};

// The text of an ordinary-string interpolation in [begin, end): a backslash escapes
// the next character, except one beginning a character literal's escape sequence,
// as in `'\\n'`.
auto unescape_interpolation(std::string_view literal, std::size_t begin, std::size_t end)
    -> std::string {
    auto chunk = std::string{};
    for (auto index = begin; index < end; ++index) {
        if (literal[index] == '\\') {
            if (literal[index - 1] == '\'') chunk += '\\';
            if (index + 1 < end) ++index;
        }
        chunk += literal[index];
    }
    return chunk;
}

// Splits `expression:format` at the first top-level single colon outside literals.
auto split_interpolation(std::string_view expression)
    -> std::pair<std::string_view, std::optional<std::string_view>> {
    auto depths = delimiter_depths{};
    for (auto position = std::size_t{0}; position < expression.size(); ++position) {
        if (const auto found = detail::lexeme_at(expression, position);
            found.kind != detail::lexeme_kind::none && found.end > position) {
            position = found.end - 1;
            continue;
        }
        const auto value = expression[position];
        if (value == ':' && position + 1 < expression.size() && expression[position + 1] == ':') {
            ++position;
            continue;
        }
        if (value == ':' && depths.at_top_level() && position != 0) {
            return {expression.substr(0, position), expression.substr(position + 1)};
        }
        update_forward_depths(value, depths);
    }
    return {expression, std::nullopt};
}

// Whether `entry` is a template parameter or an alias of a type that names one
// or a `decltype`.
auto is_dependent_scope(const scope_model::entry* entry, const scope_model& scopes, int depth)
    -> bool {
    if (entry == nullptr || depth > 8) return false;
    if (entry->kind == scope_model::entry_kind::template_parameter) return true;
    if (entry->kind != scope_model::entry_kind::type || entry->alias.empty()) return false;
    if (mentions_decltype(parse_type(entry->alias))) return true;
    const auto parsed = parse_expression_text(entry->alias);
    const auto point = scope_point{entry->scope, entry->ordinal};
    return std::any_of(parsed.tokens.begin(), parsed.tokens.end(), [&](const auto& value) {
        return value.kind == expression_token_kind::identifier &&
            is_dependent_scope(scopes.lookup(value.text, point), scopes, depth + 1);
    });
}

class tree_renderer final {
public:
    explicit tree_renderer(const cpp1_expression_options& options) : options_{options} {}

    auto render_text(std::string_view text) -> std::string {
        return render_parsed(parse_expression_text(text, options_.experiments.enabled("try-operator"),
                                                   options_.experiments.enabled("comprehension")));
    }

    auto render_condition(const expression_text& parsed, std::string_view name)
        -> std::vector<cpp1_condition_operand> {
        const auto* saved = tokens_;
        tokens_ = &parsed.tokens;
        const auto& whole = parsed.root.children.front();
        const auto chain = whole.kind == expression_kind::binary &&
            token(whole.children.front().last) == "&&";
        auto operands = std::vector<const expression*>{};
        if (!chain) operands.push_back(&whole);
        for (const auto& operand : whole.children) {
            if (chain) operands.push_back(&operand);
        }
        auto result = std::vector<cpp1_condition_operand>{};
        const auto depth = bound_names_.size();
        for (const auto* operand : operands) {
            if (operand->kind != expression_kind::pattern_test) {
                result.push_back({{}, render(*operand), {}});
                continue;
            }
            const auto parts = inspect_alternative_of(*tokens_, operand->children[1]);
            const auto subject = std::string{name} + std::to_string(result.size());
            auto [test, bindings] = pattern_branch(parts, subject);
            while (!bindings.empty() && bindings.back() == ' ') bindings.pop_back();
            result.push_back({render(operand->children[0]), test.empty() ? "true" : test,
                              std::move(bindings)});
            const auto names = bound_names(parts);
            bound_names_.insert(bound_names_.end(), names.begin(), names.end());
        }
        bound_names_.resize(depth);
        tokens_ = saved;
        return result;
    }

    auto render_let_else(const expression_text& parsed, std::string_view name)
        -> cpp1_condition_operand {
        const auto* saved = tokens_;
        tokens_ = &parsed.tokens;
        const auto& cast = parsed.root.children.front();
        auto result = cpp1_condition_operand{render(cast.children[0]),
            render_is(std::string{name}, cast.children[1]),
            "cpp2::impl::as<" + render_cpp1_expression_type(type_of(cast.children[1])) + ">(" +
                std::string{name} + ")"};
        tokens_ = saved;
        return result;
    }

    auto render_parsed(const expression_text& parsed) -> std::string {
        const auto* saved = tokens_;
        tokens_ = &parsed.tokens;
        auto result = parsed.tokens.empty() ? std::string{}
                                            : leading(0) + render(parsed.root);
        result += parsed.trailing;
        tokens_ = saved;
        return result;
    }

    auto render_items(const expression_text& parsed) -> std::vector<std::string> {
        const auto* list = whole_list(parsed);
        if (list == nullptr) return {render_parsed(parsed)};
        const auto* saved = tokens_;
        tokens_ = &parsed.tokens;
        auto result = render_children(*list);
        tokens_ = saved;
        return result;
    }

    auto is_type_subject(const std::vector<expression_token>& tokens, const expression& node)
        -> bool {
        tokens_ = &tokens;
        return subject_type(node).has_value();
    }

private:
    const cpp1_expression_options& options_;
    const std::vector<cppfrontier::expression_token>* tokens_ = nullptr;
    // The path of the text being rendered within the top-level text; see `last_use`.
    std::vector<std::size_t> path_;
    // Positive while rendering an operand that must stay an lvalue.
    int lvalue_operands_ = 0;
    // Positive while rendering an inspect alternative's value, where `as` is `cpp2::impl::as`.
    int inspect_values_ = 0;
    // Positive while rendering a requires-expression, whose UFCS calls must
    // fail substitution rather than compilation.
    int requirements_ = 0;
    // The hoisted name of each `?` rendered, by path and operator token, so
    // that rendering an operand again hoists nothing more.
    std::map<std::pair<std::vector<std::size_t>, std::size_t>, std::string> try_names_;
    // The names bound by the for-comprehension generators and inspect
    // alternatives (experiment `patterns`) enclosing the text being rendered.
    std::vector<std::string> bound_names_;

    [[nodiscard]] auto token(std::size_t index) const -> std::string_view {
        return index < tokens_->size() ? std::string_view{(*tokens_)[index].text}
                                       : std::string_view{};
    }

    [[nodiscard]] auto leading(std::size_t index) const -> std::string {
        const auto& value = (*tokens_)[index].leading;
        const auto blank = std::all_of(value.begin(), value.end(),
            [](char c) { return std::isspace(static_cast<unsigned char>(c)); });
        if (blank && (token(index) == "::" || (index > 0 && token(index - 1) == "::"))) return {};
        return value;
    }

    [[nodiscard]] auto wrapped(std::string_view name) const
        -> const cpp1_expression_options::wrapped_parameter* {
        const auto found = std::find_if(options_.wrapped_parameters.begin(),
            options_.wrapped_parameters.end(), [&](const auto& value) { return value.name == name; });
        return found == options_.wrapped_parameters.end() ? nullptr : &*found;
    }

    [[nodiscard]] auto constructs(std::size_t index) const -> bool {
        return options_.constructions != nullptr && path_.empty() &&
            std::any_of(options_.constructions->begin(), options_.constructions->end(),
                [&](const construction& value) {
                    return value.position == index && value.name == token(index);
                });
    }

    [[nodiscard]] auto last_use_at(std::size_t index) const -> const last_use* {
        if (options_.last_uses == nullptr || lvalue_operands_ > 0) return nullptr;
        for (const auto& use : *options_.last_uses) {
            if (use.path.size() == path_.size() + 1 && use.path.back() == index &&
                std::equal(path_.begin(), path_.end(), use.path.begin()) &&
                use.name == token(index)) {
                return &use;
            }
        }
        return nullptr;
    }

    auto token_text(std::size_t index) -> std::string {
        const auto& value = (*tokens_)[index];
        if (value.kind == expression_token_kind::string) {
            path_.push_back(index);
            auto text = render_string(value.text);
            path_.pop_back();
            return text;
        }
        if (value.kind != expression_token_kind::identifier) return value.text;
        auto text = identifier_text(index);
        // A member name's last use applies only to a UFCS call; see `render_ufcs`.
        if (const auto* use = index > 0 && token(index - 1) == "." ? nullptr : last_use_at(index)) {
            switch (use->kind) {
            case last_use_kind::move: return "cpp2::move(" + text + ")";
            case last_use_kind::forward: return "CPP2_FORWARD(" + text + ")";
            case last_use_kind::this_member: return "cpp2::move(*this)." + text;
            }
        }
        return text;
    }

    // Whether identifier `index`, with the `A::` qualifiers before it, names a
    // lazy object (experiment `lazy`).
    auto names_lazy_object(std::size_t index) -> bool {
        const auto& value = (*tokens_)[index];
        const auto named = [&](const auto& object) { return object.name == value.text; };
        if (current_lazy_objects == nullptr) return false;
        const auto& objects = *current_lazy_objects;
        if (std::none_of(objects.begin(), objects.end(), named)) return false;
        if (options_.scopes == nullptr || !options_.point.valid()) return true;
        auto qualified = value.text;
        for (auto at = index; at >= 2 && token(at - 1) == "::" &&
             (*tokens_)[at - 2].kind == expression_token_kind::identifier; at -= 2) {
            qualified = (*tokens_)[at - 2].text + "::" + qualified;
        }
        const auto* found = options_.scopes->lookup(qualified, options_.point);
        return found != nullptr && std::any_of(objects.begin(), objects.end(),
            [&](const auto& object) { return named(object) && object.symbol == found->symbol; });
    }

    auto identifier_text(std::size_t index) -> std::string {
        const auto& value = (*tokens_)[index];
        // `__identifier__name` is an ordinary identifier spelled like a keyword.
        constexpr auto escape = std::string_view{"__identifier__"};
        if (value.text.starts_with(escape) && value.text.size() > escape.size()) {
            return cpp1_identifier(std::string_view{value.text}.substr(escape.size()));
        }
        const auto previous = index > 0 ? token(index - 1) : std::string_view{};
        const auto next = token(index + 1);
        const auto member = previous == "." || previous == ".." || previous == "->";
        // `type T::member` marks a dependent type.
        if (value.text == "type" && !member && previous != "::" &&
            (next == "::" || (index + 1 < tokens_->size() &&
                              (*tokens_)[index + 1].kind == expression_token_kind::identifier))) {
            return "typename";
        }
        if (value.text == "this" && !member) return "(*this)";
        // A name bound by an enclosing for-comprehension is the lambda's parameter.
        if (!member && previous != "::" && std::find(bound_names_.begin(), bound_names_.end(),
                                                     value.text) != bound_names_.end()) {
            return cpp1_identifier(value.text);
        }
        if (!member && names_lazy_object(index)) return cpp1_identifier(value.text) + ".value()";
        if (!member && previous != "::" && wrapped(value.text) && previous != "out" &&
            next != "=" &&
            !(next == "." && (token(index + 2) == "construct" || token(index + 2) == "value"))) {
            return cpp1_identifier(value.text) + ".value()";
        }
        if (!member && previous != "::" && next != "::" && is_support_name(value.text)) {
            return "cpp2::" + value.text;
        }
        // A fundamental alias in an expression is the runtime's alias, which
        // is one name, so `u8(5)` and `u8{5}` are functional casts.
        if (!member && previous != "::" && is_fundamental_type_keyword(value.text)) {
            return "cpp2::" + value.text;
        }
        // Unqualified `type_of(x)` is `CPP2_TYPEOF(x)`, as in a type.
        if (!member && previous != "::" && value.text == "type_of" && next == "(") {
            return "CPP2_TYPEOF";
        }
        return cpp1_identifier(value.text);
    }

    // `$R"d(text (expr)$ text)d"` concatenates raw pieces and `cpp2::to_string(expr)`
    // (or `cpp2::to_string(expr,"{:spec}")` for `(expr:spec)$`),
    // omitting empty pieces.
    auto render_raw_interpolation(std::string_view text) -> std::string {
        const auto open = text.find('(');
        const auto close = text.rfind(')');
        if (open == std::string_view::npos || close == std::string_view::npos || close < open) {
            return std::string{text.substr(1)};
        }
        const auto delimiter = text.substr(3, open - 3);
        auto pieces = std::vector<std::string>{};
        const auto add_raw = [&](std::string_view piece) {
            if (!piece.empty()) {
                pieces.push_back("R\"" + std::string{delimiter} + "(" + std::string{piece} + ")" +
                                 std::string{delimiter} + "\"");
            }
        };
        auto chunk_begin = open + 1;
        auto ordinal = std::size_t{0};
        for (const auto& [start, end] : detail::scan_interpolations(text).parts) {
            add_raw(text.substr(chunk_begin, start - chunk_begin));
            const auto [expression, format] =
                split_interpolation(text.substr(start + 1, end - start - 1));
            path_.push_back(ordinal++);
            auto piece = "cpp2::to_string(" + render_text(expression);
            path_.pop_back();
            if (format) piece += ",\"{:" + std::string{*format} + "}\"";
            pieces.push_back(piece + ")");
            chunk_begin = end + 2;
        }
        add_raw(text.substr(chunk_begin, close - chunk_begin));
        if (pieces.empty()) return "R\"" + std::string{delimiter} + "()" + std::string{delimiter} + "\"";
        auto result = std::string{};
        for (const auto& piece : pieces) {
            if (!result.empty()) result += " + ";
            result += piece;
        }
        return result;
    }

    auto render_string(std::string_view text) -> std::string {
        if (text.starts_with("$R\"")) return render_raw_interpolation(text);
        const auto quote = text.find('"');
        if (quote == std::string_view::npos || (quote > 0 && text[quote - 1] == 'R')) {
            return std::string{text};
        }
        const auto parts = detail::scan_interpolations(text).parts;
        if (parts.empty()) return std::string{text};
        auto pieces = std::vector<std::string>{};
        auto chunk_begin = quote + 1;
        auto ordinal = std::size_t{0};
        for (const auto& [open, close] : parts) {
            if (open != chunk_begin || pieces.empty()) {
                pieces.push_back("\"" + std::string{text.substr(chunk_begin, open - chunk_begin)} +
                                 "\"");
            }
            const auto chunk = unescape_interpolation(text, open + 1, close);
            const auto [expression, format] = split_interpolation(chunk);
            path_.push_back(ordinal++);
            auto piece = "cpp2::to_string(" + render_text(expression);
            path_.pop_back();
            if (format) piece += ",\"{:" + std::string{*format} + "}\"";
            pieces.push_back(piece + ")");
            chunk_begin = close + 2;
        }
        pieces.push_back("\"" +
                         std::string{text.substr(chunk_begin, text.size() - 1 - chunk_begin)} + "\"");
        auto result = std::string{text.substr(0, quote)};
        for (auto index = std::size_t{0}; index < pieces.size(); ++index) {
            if (index != 0) result += " + ";
            result += pieces[index];
        }
        return result;
    }

    [[nodiscard]] auto raw(const expression& node) const -> std::string {
        auto result = std::string{};
        for (auto index = node.first; index < node.last; ++index) {
            if (index != node.first) result += (*tokens_)[index].leading;
            result += (*tokens_)[index].text;
        }
        return result;
    }

    auto render_children(const expression& node) -> std::vector<std::string> {
        auto result = std::vector<std::string>{};
        result.reserve(node.children.size());
        for (const auto& child : node.children) result.push_back(render(child));
        return result;
    }

    // The tokens of [first, last) with `children` substituted by `texts`.
    // `templated` lists the tokens that take a `template` keyword before them.
    auto splice(std::size_t first, std::size_t last, const std::vector<expression>& children,
                const std::vector<std::string>& texts,
                const std::vector<std::size_t>& templated = {}) -> std::string {
        auto result = std::string{};
        auto child = std::size_t{0};
        for (auto index = first; index < last;) {
            while (child < children.size() && children[child].last <= index) ++child;
            if (index != first) result += leading(index);
            if (child < children.size() && children[child].first == index) {
                result += texts[child];
                index = children[child].last;
                ++child;
            }
            else {
                if (std::ranges::find(templated, index) != templated.end()) result += "template ";
                result += token_text(index++);
            }
        }
        return result;
    }

    auto verbatim(const expression& node, const std::vector<std::string>& texts) -> std::string {
        return splice(node.first, node.last, node.children, texts);
    }

    auto verbatim(const expression& node) -> std::string {
        return verbatim(node, render_children(node));
    }

    // Cpp2 postfix `*`, `&`, `~`, `++` and `--` render as Cpp1 prefix
    // operators, which need parentheses before further postfix operators.
    auto receiver(const expression& node) -> std::string {
        auto text = render(node);
        if (node.kind == expression_kind::postfix &&
            (token(node.op) == "*" || token(node.op) == "&" || token(node.op) == "~" ||
             token(node.op) == "++" || token(node.op) == "--")) {
            return "(" + text + ")";
        }
        return text;
    }

    // `e?` hoists `auto&& name = cpp2::try_hold(e); if (!name) return
    // cpp2::try_failure(name);` before the statement, after the hoists of any
    // `?` within `e`, and reads as `*name` forwarded. `try_hold` moves an
    // rvalue `e` into a temporary that the reference extends.
    auto render_try(const expression& node) -> std::string {
        auto& name = try_names_[{path_, node.op}];
        if (name.empty()) {
            const auto operand = render(node.children.front());
            auto& hoists = *options_.try_hoists;
            name = std::string{generated_name_prefix} + "try_" + std::to_string(hoists.count++);
            hoists.statements.push_back("auto&& " + name + " = cpp2::try_hold(" + operand +
                "); if (!" + name +
                ") return cpp2::try_failure(CPP2_FORWARD(" + name + "));");
        }
        return "(*CPP2_FORWARD(" + name + "))";
    }

    auto postfix_chain(const expression& node) -> std::string {
        auto texts = render_children(node);
        texts.front() = receiver(node.children.front());
        return verbatim(node, texts);
    }

    // The template arguments of `new<T>`, without whitespace.
    // Without spaces, except one between two words, as in `const int`.
    auto compact_template_arguments(const expression& name) -> std::string {
        const auto word = [](char value) {
            return std::isalnum(static_cast<unsigned char>(value)) || value == '_';
        };
        auto result = std::string{};
        auto space = false;
        for (const auto value : splice(name.first + 2, name.last - 1, name.children,
                                       name_texts(name))) {
            if (std::isspace(static_cast<unsigned char>(value))) {
                space = true;
                continue;
            }
            if (space && !result.empty() && word(result.back()) && word(value)) result += ' ';
            space = false;
            result += value;
        }
        return result;
    }

    // Whether the name `node` has a template argument list, as in `N::f<T>`.
    [[nodiscard]] auto has_template_arguments(const expression& node) const -> bool {
        for (auto index = node.first + 1; index < node.last; ++index) {
            if (token(index) == "<" &&
                (*tokens_)[index - 1].kind == expression_token_kind::identifier &&
                token(index - 1) != "operator") {
                return true;
            }
        }
        return false;
    }

    // The `::` before the last component of the qualified name `node`.
    [[nodiscard]] auto last_separator(const expression& node) const -> std::optional<std::size_t> {
        auto result = std::optional<std::size_t>{};
        auto child = node.children.begin();
        for (auto index = node.first + 1; index < node.last; ++index) {
            if (child != node.children.end() && index == child->first) {
                index = child++->last - 1;
            }
            else if (token(index) == "::") {
                result = index;
            }
        }
        return result;
    }

    [[nodiscard]] auto is_as_literal(const expression& node) const -> bool {
        auto index = node.first;
        if (node.kind == expression_kind::prefix && node.last == node.first + 2 &&
            (token(index) == "-" || token(index) == "+")) {
            ++index;
        }
        else if (node.last != node.first + 1) {
            return false;
        }
        const auto& value = (*tokens_)[index];
        if (value.kind == expression_token_kind::character) return true;
        if (value.kind != expression_token_kind::number) return false;
        const auto text = std::string_view{value.text};
        return is_integer_literal(text) ||
            (text.size() > 2 && text[0] == '0' && std::string_view{"xXbB"}.find(text[1]) !=
                std::string_view::npos && text.find('_') == std::string_view::npos &&
             text.find('.') == std::string_view::npos);
    }

    // A name marked `type` is a type: `type T::y<int>` is `typename T::template y<int>`.
    [[nodiscard]] auto is_marked_type(std::size_t index) const -> bool {
        return token(index) == "type" && index + 1 < tokens_->size() &&
            (token(index + 1) == "::" ||
             (*tokens_)[index + 1].kind == expression_token_kind::identifier);
    }

    // The member templates of a dependent scope in a qualified name, which take
    // `template`: `base<T>::f<int>` is `base<T>::template f<int>`.
    [[nodiscard]] auto dependent_member_templates(const expression& node) const
        -> std::vector<std::size_t> {
        auto result = std::vector<std::size_t>{};
        if (options_.scopes == nullptr) return result;
        auto dependent = false;
        for (auto index = node.first; index + 2 < node.last; ++index) {
            const auto& value = (*tokens_)[index];
            dependent = dependent || value.text == "decltype" ||
                (value.kind == expression_token_kind::identifier && is_dependent_scope(
                    options_.scopes->lookup(value.text, options_.point), *options_.scopes, 0));
            if (dependent && value.text == "::" &&
                (*tokens_)[index + 1].kind == expression_token_kind::identifier &&
                token(index + 2) == "<") {
                result.push_back(index + 1);
            }
        }
        return result;
    }

    auto render_name(const expression& node) -> std::string {
        if (is_marked_type(node.first)) return render_cpp1_expression_type(type_of(node));
        return splice(node.first, node.last, node.children, name_texts(node),
                      dependent_member_templates(node));
    }

    // The rendered template argument lists of a name. Template arguments
    // spelled with a leading `*`, `const` or `type` are types.
    auto name_texts(const expression& node) -> std::vector<std::string> {
        auto texts = render_children(node);
        for (std::size_t index = 0; index < node.children.size(); ++index) {
            const auto& child = node.children[index];
            auto arguments = std::vector<std::pair<std::size_t, std::size_t>>{};
            auto begin = child.first;
            auto depth = 0;
            for (auto position = child.first; position < child.last; ++position) {
                const auto value = token(position);
                if (value == "(" || value == "[" || value == "{" || value == "<") ++depth;
                else if (value == ")" || value == "]" || value == "}" || value == ">") --depth;
                else if (value == "," && depth == 0) {
                    arguments.emplace_back(begin, position);
                    begin = position + 1;
                }
            }
            arguments.emplace_back(begin, child.last);
            const auto is_type = [&](const auto& argument) {
                return argument.first < argument.second &&
                    (token(argument.first) == "*" || token(argument.first) == "const" ||
                     is_marked_type(argument.first));
            };
            if (std::none_of(arguments.begin(), arguments.end(), is_type)) continue;
            auto text = std::string{};
            for (const auto& argument : arguments) {
                auto spelling = std::string{};
                for (auto position = argument.first; position < argument.second; ++position) {
                    if (position != argument.first) spelling += (*tokens_)[position].leading;
                    spelling += token(position);
                }
                if (!text.empty()) text += ", ";
                path_.push_back(std::size_t(-1));
                text += is_type(argument)
                    ? render_cpp1_expression_type(parse_dependent_type(
                          *tokens_, argument.first, argument.second, options_.scopes,
                          options_.point, {}, type_options()))
                    : render_text(spelling);
                path_.pop_back();
            }
            texts[index] = std::move(text);
        }
        return texts;
    }

    [[nodiscard]] auto is_allocation(const expression& name) const -> bool {
        return name.kind == expression_kind::name && token(name.first) == "new" &&
            token(name.first + 1) == "<" && token(name.last - 1) == ">";
    }

    auto render(const expression& node) -> std::string {
        switch (node.kind) {
        case expression_kind::name:
            if (is_allocation(node)) return "cpp2_new<" + compact_template_arguments(node) + ">";
            return render_name(node);
        case expression_kind::group:
            return render_group(node);
        case expression_kind::typed_value: {
            // `:u8 = 3` is `cpp2::u8{3}`, as `u8{3}` is, and `:*const char = (p)` is
            // `std::type_identity_t<char const*>{p}`; a dependent name is
            // `typename C::value_type{}`.
            const auto type = type_of(node.children[0]);
            const auto spelling = render_cpp1_expression_type(type);
            const auto simple =
                type.kind == type_kind::name || is_simple_type_specifier(type, true);
            return (simple ? spelling : "std::type_identity_t<" + spelling + ">") +
                "{" + inner(node.children[1]) + "}";
        }
        case expression_kind::call:
            return render_call(node);
        case expression_kind::subscript:
            return render_subscript(node);
        case expression_kind::conditional:
            return verbatim(node);
        case expression_kind::member:
            return render_member(node);
        case expression_kind::postfix:
            if (token(node.op) == "?" && options_.try_hoists != nullptr) return render_try(node);
            if (token(node.op) == "*") {
                return options_.check_null_dereference
                    ? "*cpp2::impl::assert_not_null(" + render(node.children.front()) + ")"
                    : "*" + render(node.children.front());
            }
            if (token(node.op) == "&") {
                // `&T::member` is a pointer to member only without parentheses.
                const auto& operand = node.children.front();
                auto text = render_operand(operand);
                return operand.kind == expression_kind::name && text.find("::") != std::string::npos
                    ? "&" + text : "&(" + text + ")";
            }
            if (token(node.op) == "++" || token(node.op) == "--") {
                const auto& operand = node.children.front();
                if (options_.check_arithmetic && known_integer(operand)) {
                    return std::string{token(node.op) == "++" ? "cpp2::checked_add_assign("
                                                               : "cpp2::checked_sub_assign("} +
                        render_operand(operand) + ", 1)";
                }
                return std::string{token(node.op)} + render_operand(operand);
            }
            if (token(node.op) == "~") return "~" + render(node.children.front());
            return postfix_chain(node);
        case expression_kind::argument:
            if (token(node.op) == "move") return "std::move(" + render(node.children.front()) + ")";
            if (token(node.op) == "forward") {
                // As upstream, `forward x.member` forwards its first identifier.
                auto text = render_lvalue(node.children.front());
                auto end = std::size_t{0};
                while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) ||
                                             text[end] == '_')) {
                    ++end;
                }
                if (end == 0 || std::isdigit(static_cast<unsigned char>(text.front()))) return text;
                return "CPP2_FORWARD(" + text.substr(0, end) + ")" + text.substr(end);
            }
            if (token(node.op) == "out") {
                return "cpp2::impl::out(&" + render_lvalue(node.children.front()) + ")";
            }
            return verbatim(node);
        case expression_kind::prefix:
            // `-x` on a known integer, other than a literal, is `cpp2::checked_neg(x)`.
            if (options_.check_arithmetic && node.children.front().first == node.first + 1 &&
                token(node.first) == "-" &&
                node.children.front().kind != expression_kind::literal &&
                known_integer(node.children.front())) {
                return "cpp2::checked_neg(" + inner(node.children.front()) + ")";
            }
            return verbatim(node);
        case expression_kind::binary:
            return render_binary(node);
        case expression_kind::assignment:
            return render_assignment(node);
        case expression_kind::is_as:
            if (token(node.op) == "as") {
                const auto type = render_cpp1_expression_type(type_of(node.children[1]));
                const auto as = std::string{inspect_values_ > 0 ? "cpp2::impl::as<"
                                                                : "cpp2::impl::as_<"};
                // An integer or character literal is checked at compile time.
                if (is_as_literal(node.children[0])) {
                    return as + type + ", " + raw(node.children[0]) + ">()";
                }
                return as + type + ">(" + render(node.children[0]) + ")";
            }
            if (raw(node.children[1]) == "_") return "true";
            if (const auto subject = subject_type(node.children[0])) {
                return render_is(*subject, node.children[1], true);
            }
            return render_is(render(node.children[0]), node.children[1]);
        case expression_kind::inspect:
            return render_inspect(node);
        case expression_kind::comprehension:
            return render_comprehension(node);
        case expression_kind::range:
            return "cpp2::range(" + render(node.children[0]) + "," + render(node.children[1]) +
                (token(node.op) == "..=" ? ",true)" : ")");
        case expression_kind::parameter:
            return requires_parameter(node);
        // A type requirement `typename T::y<int>;` names a dependent type.
        case expression_kind::requirement:
            if (token(node.first) == "typename" && node.children.size() == 1 &&
                node.children.front().first == node.first + 1) {
                auto type = type_of(node.children.front());
                type.core().has_type_marker = true;
                return render_cpp1_expression_type(type) +
                    (token(node.last - 1) == ";" ? ";" : "");
            }
            return verbatim(node);
        case expression_kind::requires_expression: {
            ++requirements_;
            auto result = verbatim(node);
            --requirements_;
            return result;
        }
        default:
            return verbatim(node);
        }
    }

    // A requires-expression parameter as a Cpp1 declaration: `inout a: T` is `T& a`.
    auto requires_parameter(const expression& node) -> std::string {
        const auto passing = node.op == node.first ? std::string_view{} : token(node.first);
        const auto* qualifier = passing == "copy" ? "" : passing == "inout" || passing == "out"
            ? "& " : passing == "move" || passing == "forward" ? "&& " : "const& ";
        const auto& type_node = node.children.front();
        // A function type has no reference qualifier, and adjusts to a pointer.
        const auto type = parse_dependent_type(*tokens_, type_node.first, type_node.last,
                                               options_.scopes, options_.point, {},
                                               type_options());
        return render_cpp1_type_declaration(type,
            (type.core().is_function() ? "" : qualifier) + std::string{token(node.op)});
    }

    // `subject is pattern`, where `pattern` is a type-id or a value. A concept
    // declared in this translation unit tests the subject's type: `x is C<A>`
    // is `C<CPP2_TYPEOF(x), A>`. When `type_subject`, `subject` is a Cpp1 type
    // `T`, and `T is X` is whether `x is X` is statically true for a value `x`
    // of type `T`, which `cpp2::impl::is` tells by returning `std::true_type`.
    auto render_is(const std::string& subject, const expression& pattern,
                   bool type_subject = false) -> std::string {
        if (pattern.kind != expression_kind::sequence) {
            return "cpp2::impl::is(" + subject + ", " + render(pattern) + ")";
        }
        const auto type = type_of(pattern);
        if (names_concept(type)) {
            return render_cpp1_constraint(type,
                type_subject ? subject : "CPP2_TYPEOF(" + subject + ")");
        }
        const auto test = "cpp2::impl::is<" + render_cpp1_expression_type(type) + ">(";
        if (type_subject) {
            return "std::is_same_v<decltype(" + test + "std::declval<" + subject +
                " const&>())), std::true_type>";
        }
        return test + subject + ")";
    }

    // The declaration a type's name finds in this translation unit.
    [[nodiscard]] auto declaration_of(const type_id& type) const -> const scope_model::entry* {
        if (options_.scopes == nullptr || !options_.point.valid() ||
            type.kind != type_kind::name) {
            return nullptr;
        }
        auto name = std::string{type.is_global ? "::" : ""};
        for (const auto& component : type.components) {
            name += (&component == &type.components.front() ? "" : "::") + component.identifier;
        }
        const auto* found = options_.scopes->lookup(name, options_.point);
        return found != nullptr ? options_.scopes->resolve(*found) : nullptr;
    }

    // Whether `type` names a concept declared in this translation unit.
    [[nodiscard]] auto names_concept(const type_id& type) const -> bool {
        const auto* found = declaration_of(type);
        return found != nullptr && found->kind == scope_model::entry_kind::concept_declaration;
    }

    // Whether the name `node` finds a type this translation unit declares, or
    // an alias of one.
    [[nodiscard]] auto names_type(const expression& node) const -> bool {
        const auto* found = declaration_of(type_of(node));
        return found != nullptr && found->kind == scope_model::entry_kind::type;
    }

    // Whether the qualified name `node`, `N::f`, finds a function with a `this`
    // parameter of a type this translation unit declares.
    [[nodiscard]] auto names_member_function(const expression& node) const -> bool {
        const auto* found = declaration_of(type_of(node));
        return found != nullptr && found->kind == scope_model::entry_kind::function &&
            found->has_this;
    }

    // The Cpp1 type that the subject of an `is` or `inspect` names when it is
    // visibly a type: a fundamental type, a template type parameter, or a type
    // this translation unit declares or an alias of one, plainly or qualified.
    [[nodiscard]] auto subject_type(const expression& node) const -> std::optional<std::string> {
        if (node.kind != expression_kind::name) return std::nullopt;
        const auto type = type_of(node);
        const auto* found = declaration_of(type);
        if (type.kind == type_kind::fundamental || names_type(node) ||
            (found != nullptr && found->kind == scope_model::entry_kind::template_parameter &&
             found->type.empty())) {
            return render_cpp1_expression_type(type);
        }
        return std::nullopt;
    }

    // `T{items}`, or `std::type_identity_t<T>{items}` when `T` is not a simple
    // type specifier, such as `char const*` or `unsigned int`.
    static auto value_initialized(const type_id& type, const std::string& items = {})
        -> std::string {
        const auto spelling = render_cpp1_expression_type(type);
        return (is_simple_type_specifier(type, true) ? spelling
                                                     : "std::type_identity_t<" + spelling + ">") +
            "{" + items + "}";
    }

    // An inspect alternative's value initializes the result type, as `:T = value`
    // does, so a list `(a, b)`, `()` or `((a, b))` is `T{a, b}`, `T{}` or
    // `T{{a, b}}`.
    auto alternative_value(const expression& value, const type_id& type) -> std::string {
        const auto is_list = [](const auto& self, const expression& node) -> bool {
            return node.kind == expression_kind::group &&
                (node.children.size() != 1 || self(self, node.children.front()));
        };
        return is_list(is_list, value) ? value_initialized(type, inner(value)) : render(value);
    }

    [[nodiscard]] auto type_options() const -> type_parse_options {
        return {.type_operators = options_.experiments.enabled("type-operators")};
    }

    [[nodiscard]] auto type_of(const expression& node) const -> type_id {
        return with_declared_types(parse_dependent_type(*tokens_, node.first, node.last,
            options_.scopes, options_.point, {}, type_options()), options_);
    }

    // An immediately invoked lambda testing each alternative in order; an
    // alternative whose value is invalid or not convertible to the result
    // type yields a value-initialized result. With the `patterns` experiment
    // the alternatives are independent `if`s, since a failed guard falls
    // through, each with its bindings, and the lambda ends in
    // `std::terminate()` unless an alternative matches anything. The subject is
    // unused when every alternative is a wildcard. A subject that is visibly a
    // type is tested as the type, as `T is X` is.
    auto render_inspect(const expression& node) -> std::string {
        const auto patterns = options_.experiments.enabled("patterns");
        const auto qualifier = std::string{token(node.first + 1) == "constexpr" ? "constexpr " : ""};
        const auto result_type = type_of(node.children[1]);
        const auto type = render_cpp1_expression_type(result_type);
        const auto type_subject = subject_type(node.children[0]);
        const auto subject = type_subject.value_or("__cpp2_expr");
        auto result = "[&] () -> " + type + " {" + (type_subject ? std::string{} :
            " [[maybe_unused]] " + qualifier + "auto&& __cpp2_expr = " +
            render(node.children[0]) + ";");
        auto wildcard = false;
        for (auto index = std::size_t{2}; index < node.children.size(); ++index) {
            const auto& alternative = node.children[index];
            const auto& pattern = alternative.children[0];
            const auto parts = inspect_alternative_of(*tokens_, alternative);
            const auto names = patterns ? bound_names(parts) : std::vector<std::string>{};
            bound_names_.insert(bound_names_.end(), names.begin(), names.end());
            ++inspect_values_;
            const auto value = alternative_value(*parts.value, result_type);
            // The checks of the value are unevaluated, where a UFCS call cannot capture.
            ++requirements_;
            const auto checked = alternative_value(*parts.value, result_type);
            --requirements_;
            --inspect_values_;
            const auto guard = parts.guard != nullptr ? render(*parts.guard) : std::string{};
            bound_names_.resize(bound_names_.size() - names.size());
            result += leading(alternative.first) + (index == 2 || patterns ? "" : "else ");
            if (pattern.kind == expression_kind::sequence && raw(pattern) == "_" &&
                (!patterns || (parts.matches_anything() && names.empty()))) {
                result += "return " + value + ";";
                wildcard = true;
                break;
            }
            const auto fallback = "; else return " + value_initialized(result_type);
            // A requires-expression cannot hold a lambda that captures, so a
            // value holding a function or inspect expression is not checked
            // for validity.
            const auto valid = holds_lambda(*parts.value)
                ? std::string{}
                : "if constexpr( requires{" + checked + ";} ) ";
            const auto returns = valid + "if constexpr( std::is_convertible_v<CPP2_TYPEOF((" +
                checked + "))," + type + "> ) return " + value + fallback +
                (valid.empty() ? "" : fallback) + ";";
            if (!patterns) {
                result += "if " + qualifier + "(" +
                    render_is(subject, pattern, type_subject.has_value()) + ") { " +
                    returns + " }";
                continue;
            }
            const auto [condition, bindings] =
                pattern_branch(parts, subject, type_subject.has_value());
            result += condition.empty() ? "{ " : "if " + qualifier + "(" + condition + ") { ";
            // In a template the subject may not convert to the tested type (`as`
            // is invalid or yields `nonesuch`); the alternative, whose test then
            // fails, is left out.
            const auto matched = matched_value(parts, subject);
            const auto converts = !bindings.empty() && matched != subject;
            // A tuple pattern is left out where the subject is not a type that
            // a structured binding takes apart, so it is not the wildcard.
            const auto takes_apart = parts.destructuring != nullptr && !converts;
            if (converts) {
                result += "if constexpr( requires{" + matched + ";} ) if constexpr( "
                    "!std::is_same_v<CPP2_TYPEOF(" + matched + "), cpp2::nonesuch_> ) { ";
            }
            else if (takes_apart) {
                result += "if constexpr( cpp2::impl::destructurable<CPP2_TYPEOF(" + matched +
                    ")> ) { ";
            }
            result += bindings;
            result += guard.empty() ? returns : "if (" + guard + ") { " + returns + " }";
            result += converts || takes_apart ? " } }" : " }";
            if (parts.matches_anything() && !takes_apart) {
                wildcard = true;
                break;
            }
        }
        if (patterns && !wildcard) result += " std::terminate();";
        return result + leading(node.last - 1) + "}()";
    }

    // Whether `node` holds a function expression or an inspect expression,
    // which render as lambdas.
    [[nodiscard]] auto holds_lambda(const expression& node) const -> bool {
        for (auto index = node.first; index < node.last; ++index) {
            if (token(index) == "inspect" ||
                token(index).starts_with("__cpp2_function_expression_")) {
                return true;
            }
        }
        return false;
    }

    // The names an alternative binds, other than `_`.
    [[nodiscard]] auto bound_names(const inspect_alternative& parts) const
        -> std::vector<std::string> {
        auto names = std::vector<std::string>{};
        if (parts.destructuring != nullptr) {
            for (const auto& item : parts.destructuring->children) {
                if (token(item.first) != "_") names.emplace_back(token(item.first));
            }
        }
        if (parts.binding != nullptr) names.emplace_back(token(parts.binding->first));
        return names;
    }

    // The value an alternative's bindings name: the one type's `as`, or the
    // subject itself.
    auto matched_value(const inspect_alternative& parts, const std::string& subject)
        -> std::string {
        const auto& patterns = parts.patterns;
        return patterns.size() == 1 && !parts.wildcard &&
                patterns.front()->kind == expression_kind::sequence
            ? "cpp2::impl::as<" + render_cpp1_expression_type(type_of(*patterns.front())) +
                  ">(" + subject + ")"
            : subject;
    }

    // The condition and the bindings of an `is` alternative under the
    // `patterns` experiment, `is T(a, _) | U as c`, testing the value that
    // `subject` names, or the type when `type_subject`; the condition is empty
    // for `_` and a tuple pattern.
    auto pattern_branch(const inspect_alternative& parts, const std::string& subject,
                        bool type_subject = false)
        -> std::pair<std::string, std::string> {
        auto condition = std::string{};
        if (!parts.wildcard) {
            for (const auto* pattern : parts.patterns) {
                condition += (condition.empty() ? "" : " || ") +
                    render_is(subject, *pattern, type_subject);
            }
        }
        const auto matched = matched_value(parts, subject);
        auto bindings = std::string{};
        if (parts.destructuring != nullptr) {
            auto names = std::string{};
            auto unused = 0;
            for (const auto& item : parts.destructuring->children) {
                const auto name = token(item.first);
                names += (names.empty() ? "" : ", ") +
                    (name == "_" ? "__cpp2_unused_" + std::to_string(unused++)
                                 : cpp1_identifier(name));
            }
            bindings += "[[maybe_unused]] auto&& [" + names + "] = " + matched + "; ";
        }
        if (parts.binding != nullptr) {
            bindings += "[[maybe_unused]] auto&& " + cpp1_identifier(token(parts.binding->first)) +
                " = " + matched + "; ";
        }
        return {condition, bindings};
    }

    // `cpp2::comprehend(range, [guard,] value)` with the guard and value as
    // lambdas over the bound name, or `cpp2::comprehend_bind` when the value is
    // an inner comprehension (experiment `comprehension`). The lambdas capture
    // by reference (nothing outside a function), except the names bound by
    // enclosing generators that their body names, which they copy: the
    // enclosing lambda has returned when a lazy view calls them. A name in a
    // comprehension is not moved from, since the view may run later.
    auto render_comprehension(const expression& node) -> std::string {
        const auto& value = node.children.back();
        const auto name = cpp1_identifier(token(node.children[1].first));
        const auto capture = [&, enclosing = bound_names_](const std::string& body) {
            auto result = std::string{options_.nonlocal ? "[" : "[&"};
            for (const auto& bound : enclosing) {
                if (detail::names_word(body, bound)) {
                    result += (result.size() > 1 ? ", " : "") + bound;
                }
            }
            return result + "]";
        };
        // Line breaks and comments before a keyword are kept; plain spaces are not.
        const auto spacing = [&](std::size_t index) {
            const auto text = leading(index);
            const auto begin = text.find_first_not_of(' ');
            return begin == std::string::npos ? std::string{} : text.substr(begin);
        };
        const auto lambda = [&](std::size_t keyword, const std::string& body) {
            return spacing(keyword) + capture(body) + "(auto const& " + name + ") { return " +
                body + "; }";
        };
        ++lvalue_operands_;
        auto result = std::string{value.kind == expression_kind::comprehension
                                      ? "cpp2::comprehend_bind(" : "cpp2::comprehend("} +
            spacing(node.children[0].first) + render(node.children[0]);
        bound_names_.push_back(name);
        if (node.children.size() == 4) {
            result += ", " + lambda(node.children[2].first - 1, render(node.children[2]));
        }
        result += ", " + lambda(node.op, render(value)) + ")";
        bound_names_.pop_back();
        --lvalue_operands_;
        return result;
    }

    // An operand that is not moved from on a last use.
    auto render_lvalue(const expression& node) -> std::string {
        ++lvalue_operands_;
        auto text = render(node);
        --lvalue_operands_;
        return text;
    }

    // The operand of a postfix `&`, `++` or `--`: as upstream, a name is not
    // moved from, while a call's callee may be.
    auto render_operand(const expression& node) -> std::string {
        return node.kind == expression_kind::name || node.kind == expression_kind::member
            ? render_lvalue(node) : render(node);
    }

    // A group's contents, or the whole expression for anything else.
    auto inner(const expression& node) -> std::string {
        auto text = render(node);
        if (node.kind == expression_kind::group && text.size() >= 2) {
            return text.substr(1, text.size() - 2);
        }
        return text;
    }

    auto render_group(const expression& node) -> std::string {
        const auto& open = (*tokens_)[node.first];
        if (node.children.empty() && node.first > 0 && (*tokens_)[node.first + 1].leading.empty() &&
            std::all_of(open.leading.begin(), open.leading.end(),
                [](char c) { return std::isspace(static_cast<unsigned char>(c)); })) {
            const auto previous = token(node.first - 1).back();
            if (previous == '(' || previous == ',' || previous == ':' || previous == '=') {
                return "{}";
            }
        }
        // Cpp2 has no comma operator, so a list inside a list is a nested
        // initializer: `(("a", 1), ("b", 2))` is `{{"a", 1}, {"b", 2}}`.
        auto texts = render_children(node);
        // A trailing comma is ignored, so `(x,)` is the grouping `(x)`.
        if (node.children.size() == 1 && token(node.last - 2) == ",") {
            return splice(node.first, node.last - 2, node.children, texts) + ")";
        }
        for (std::size_t index = 0; index < node.children.size(); ++index) {
            const auto& child = node.children[index];
            auto& text = texts[index];
            const auto open = text.find('(');
            if (child.kind == expression_kind::group && child.children.size() > 1 &&
                open != std::string::npos && text.back() == ')') {
                text[open] = '{';
                text.back() = '}';
            }
        }
        return verbatim(node, texts);
    }

    [[nodiscard]] auto has_trailing_comma(const expression& node) const -> bool {
        return token(node.op) == "(" && node.children.size() > 1 && node.last >= node.first + 3 &&
            token(node.last - 2) == "," && node.children.back().last == node.last - 2;
    }

    enum class call_kind : std::uint8_t { ordinary, ufcs, member, member_template };

    // `x.f(args)` calls a member `f` if there is one, else `f(x, args)`. Only
    // the member can be meant when `f` finds an object whose initializer this
    // is (#550, #863, #864), or when `f<T>` names an object, which is not a
    // template (#889).
    [[nodiscard]] auto call_kind_of(const expression& node) const -> call_kind {
        const auto& callee = node.children.front();
        if (callee.kind != expression_kind::member || token(callee.op) != "." ||
            token(node.op) != "(") {
            return call_kind::ordinary;
        }
        if (options_.member_calls) return call_kind::member;
        const auto& member = callee.children[1];
        if (options_.scopes == nullptr || member.kind != expression_kind::name ||
            last_separator(member)) {
            return call_kind::ufcs;
        }
        const auto templated = has_template_arguments(member);
        if (member.last != member.first + 1 && !templated) return call_kind::ufcs;
        const auto* found = options_.scopes->lookup(token(member.first), options_.point);
        if (found == nullptr) return call_kind::ufcs;
        if (options_.scopes->under_initialization(*found, options_.point)) return call_kind::member;
        found = options_.scopes->resolve(*found);
        using kind = scope_model::entry_kind;
        if (templated && !found->is_template &&
            (found->kind == kind::object || found->kind == kind::parameter ||
             found->kind == kind::statement_parameter)) {
            return call_kind::member_template;
        }
        return call_kind::ufcs;
    }

    // `CPP2_UFCS(f)(x, args)`, with `_TEMPLATE` for template arguments,
    // `_MOVE` or `_FORWARD` when `f` names a local at its last use, and
    // `_NONLOCAL` outside function bodies and in requires-expressions, where
    // it is constrained. A qualified name names a non-member, `x.N::f(args)`
    // is `N::f(x, args)`, unless `N::f` is a function with a `this` parameter
    // of a type this translation unit declares: then it is the member call
    // `x.N::f(args)`, as `this.Base::f()`.
    auto render_ufcs(const expression& node) -> std::string {
        const auto& callee = node.children.front();
        const auto& member = callee.children[1];
        auto macro = std::string{"CPP2_UFCS"};
        auto name = std::string{};
        auto object = std::string{};
        if (is_allocation(member)) {
            name = "cpp2_new<" + compact_template_arguments(member) + ">";
            object = render(callee.children[0]);
            if (object == "unique" || object == "shared") object = "cpp2::" + object;
        }
        else {
            name = render_lvalue(member);
            // A support name or fundamental alias is never a member:
            // `x.unchecked_div(y)` is `cpp2::unchecked_div(x, y)`,
            // `x.unchecked_narrow<u8>()` is `cpp2::unchecked_narrow<std::uint8_t>(x)`,
            // and `123.u8()` is `cpp2::u8(123)`.
            object = receiver(callee.children[0]);
            const auto alias = is_fundamental_type_keyword(token(member.first)) &&
                member.last == member.first + 1;
            const auto support = alias || (is_support_name(token(member.first)) &&
                (member.last == member.first + 1 || token(member.first + 1) == "<"));
            const auto separator = last_separator(member);
            if (separator && token(member.first) != "::" && names_member_function(member)) {
                return object + "." + name + "(" + call_arguments(node, false) + ")";
            }
            if (support || token(member.first) == "::" || separator) {
                return (support ? "cpp2::" : "") + name + "(" + object + call_arguments(node, true) + ")";
            }
        }
        if (is_allocation(member) || has_template_arguments(member)) macro += "_TEMPLATE";
        if (options_.nonlocal || requirements_ > 0) macro += "_NONLOCAL";
        else if (member.last == member.first + 1) {
            if (const auto* use = last_use_at(member.first)) {
                macro += use->kind == last_use_kind::forward ? "_FORWARD" : "_MOVE";
            }
        }
        return macro + "(" + name + ")(" + object + call_arguments(node, true) + ")";
    }

    // The rendered arguments of call `node`, each after a comma if `leading`.
    auto call_arguments(const expression& node, bool leading) -> std::string {
        auto texts = render_children(node);
        const auto end = has_trailing_comma(node) ? node.last - 2 : node.last - 1;
        if (node.children.size() <= 1) return {};
        return (leading ? ", " : "") + splice(node.op + 1, end, node.children, texts);
    }

    auto render_call(const expression& node) -> std::string {
        const auto kind = call_kind_of(node);
        if (kind == call_kind::ufcs) return render_ufcs(node);
        // `decltype(x)` of a deferred local or `out` parameter is its declared type.
        if (const auto& callee = node.children.front(); node.children.size() == 2 &&
            callee.kind == expression_kind::name && callee.last == callee.first + 1 &&
            token(callee.first) == "decltype") {
            const auto& argument = node.children[1];
            if (argument.kind == expression_kind::name && argument.last == argument.first + 1) {
                auto type = type_id{};
                type.kind = type_kind::decltype_of;
                type.text = "decltype(" + std::string{token(argument.first)} + ")";
                if (auto declared = with_declared_types(type, options_);
                    declared.kind != type_kind::decltype_of) {
                    return declared.text;
                }
            }
        }
        auto texts = render_children(node);
        texts.front() = receiver(node.children.front());
        // The type-id of `sizeof(*int)`, `alignof(const T)` or `typeid(*T)`.
        if (node.children.size() == 2 && node.children[1].kind == expression_kind::sequence &&
            (token(node.first) == "sizeof" || token(node.first) == "alignof" ||
             token(node.first) == "typeid")) {
            texts[1] = render_cpp1_expression_type(type_of(node.children[1]));
        }
        // `(type T::x)(args)` constructs a `T::x`, written `typename T::x(args)`.
        if (const auto& callee = node.children.front();
            callee.kind == expression_kind::group && callee.children.size() == 1 &&
            callee.children.front().kind == expression_kind::name &&
            token(callee.children.front().first) == "type") {
            texts.front() = render(callee.children.front());
        }
        // `x.template f<T>()`, since lookup from the call finds an object `f`.
        if (kind == call_kind::member_template) {
            const auto& callee = node.children.front();
            texts.front() = receiver(callee.children[0]) + ".template " + render(callee.children[1]);
        }
        // Cpp1 calls do not allow a trailing comma.
        if (has_trailing_comma(node)) {
            return splice(node.first, node.last - 2, node.children, texts) + ")";
        }
        return verbatim(node, texts);
    }

    // A single-index subscript is bounds checked, as upstream.
    auto render_subscript(const expression& node) -> std::string {
        if (!options_.check_subscripts || node.children.size() != 2) return postfix_chain(node);
        // `xs...[0]` indexes a pack (C++26), which is not a subscript.
        if (token(node.op - 1) == "...") return postfix_chain(node);
        const auto& index = node.children[1];
        const auto literal = index.kind == expression_kind::literal &&
            index.last == index.first + 1 && is_integer_literal(token(index.first));
        // A macro argument cannot hold a comma outside parentheses, as in a lambda's captures.
        const auto argument = [](std::string text) {
            auto depth = 0;
            auto comma = text.find("__cpp2_function_expression_") != std::string::npos;
            for (const auto value : text) {
                if (value == '(') ++depth;
                else if (value == ')') --depth;
                else if (value == ',' && depth == 0) comma = true;
            }
            return comma ? "(" + text + ")" : text;
        };
        return std::string{literal ? "CPP2_ASSERT_IN_BOUNDS_LITERAL(" : "CPP2_ASSERT_IN_BOUNDS("} +
            argument(receiver(node.children.front())) + ", " + argument(render(index)) + ")";
    }

    auto render_member(const expression& node) -> std::string {
        const auto& member = node.children[1];
        if (token(node.op) == "." && is_allocation(member)) {
            auto object = render(node.children[0]);
            if (object == "unique" || object == "shared") object = "cpp2::" + object;
            return object + ".cpp2_new<" + compact_template_arguments(member) + ">";
        }
        // `x..f()` names a member, never a UFCS call; `x.template f<T>()` in
        // case `x` is dependent.
        if (token(node.op) == "..") {
            return receiver(node.children[0]) +
                (has_template_arguments(member) ? ".template " : ".") + render(member);
        }
        // A member template of a `decltype(e)` scope takes `template`.
        if (token(node.op) == "::" && has_template_arguments(member)) {
            return render(node.children[0]) + "::template " + render(member);
        }
        return postfix_chain(node);
    }

    auto is_support_name(std::string_view name) const -> bool {
        return is_cpp2_support_name(name, options_.experiments);
    }

    auto checked_denominator(std::string_view numerator, const expression& denominator,
                             std::string_view text) -> std::string {
        const auto literal = denominator.kind == expression_kind::literal &&
            denominator.last == denominator.first + 1 && is_integer_literal(token(denominator.first));
        return std::string{literal ? "CPP2_ASSERT_NOT_ZERO_LITERAL" : "CPP2_ASSERT_NOT_ZERO"} +
            "(CPP2_TYPEOF(" + std::string{numerator} + ")," + std::string{text} + ")";
    }

    // Whether `node`, with experiment `nonnegative-literal-comparisons`, is a decimal literal
    // without a `u` suffix, which converts to either signedness without changing its value.
    [[nodiscard]] auto either_sign_literal(const expression& node) const -> bool {
        if (!options_.experiments.enabled("nonnegative-literal-comparisons") ||
            node.kind != expression_kind::literal || node.last != node.first + 1) {
            return false;
        }
        const auto digits = token(node.first);
        return is_integer_literal(digits) && digits.find_first_of("uU") == std::string_view::npos &&
            (digits.size() == 1 || digits.front() != '0');
    }

    // `a < b` is `cpp2::impl::cmp_less(a,b)`, which rejects mixed-sign integers (safety.md).
    // A comparison with an `either_sign_literal` is `cmp_less_literal(a,b)`, which
    // rejects only `bool` operands.
    // `a < b <= c` evaluates each operand once, as upstream:
    // `[_0 = a, _1 = b, _2 = c]{ return cmp_less(_0,_1) && cmp_less_eq(_1,_2); }()`.
    // Chains mixing `<` with `>`, or containing `!=`, are rendered unchanged.
    auto render_comparison_chain(const expression& node, const std::vector<std::string>& texts)
        -> std::optional<std::string> {
        if (node.children.size() < 2) return std::nullopt;
        const auto operator_text = [&](std::size_t index) {
            return binary_operator(*tokens_, node, index);
        };
        auto kinds = std::string{};
        for (auto index = std::size_t{1}; index < node.children.size(); ++index) {
            const auto kind = comparison_kind(operator_text(index));
            if (kind == '\0' || kind == 'n' || (!kinds.empty() && kinds.front() != kind)) {
                return std::nullopt;
            }
            kinds += kind;
        }
        const auto checked = [&](std::size_t index, const std::string& lhs,
                                 const std::string& rhs) {
            const auto op = operator_text(index);
            if (op == "==" || !options_.check_comparisons) return lhs + op + rhs;
            const auto literal = either_sign_literal(node.children[index - 1]) ||
                either_sign_literal(node.children[index]);
            return std::string{op == "<" ? "cpp2::impl::cmp_less" : op == "<=" ? "cpp2::impl::cmp_less_eq"
                : op == ">" ? "cpp2::impl::cmp_greater" : "cpp2::impl::cmp_greater_eq"} +
                (literal ? "_literal(" : "(") + lhs + "," + rhs + ")";
        };
        if (node.children.size() == 2) {
            if (kinds == "e" || !options_.check_comparisons) return std::nullopt;
            return checked(1, texts[0], texts[1]);
        }
        // A literal operand is compared as itself, a constant the compiler knows is non-negative.
        const auto literal = [&](std::size_t index) {
            return either_sign_literal(node.children[index]);
        };
        auto captures = std::string{};
        for (auto index = std::size_t{0}; index < node.children.size(); ++index) {
            if (literal(index)) continue;
            if (!captures.empty()) captures += ", ";
            captures += "_" + std::to_string(index) + " = " + texts[index];
        }
        const auto operand = [&](std::size_t index) {
            return literal(index) ? texts[index] : "_" + std::to_string(index);
        };
        auto body = std::string{};
        for (auto index = std::size_t{1}; index < node.children.size(); ++index) {
            if (!body.empty()) body += " && ";
            body += checked(index, operand(index - 1), operand(index));
        }
        return "[" + captures + "]{ return " + body + "; }()";
    }

    [[nodiscard]] auto known_integer(const expression& node) const -> bool {
        return integer_knowledge{*tokens_, options_.scopes, options_.point, bound_names_}
            .known(node);
    }

    // `a + b - c` on known integers is `cpp2::checked_sub(cpp2::checked_add(a, b), c)`,
    // folding from the left. An operand that is not a known integer, and the
    // operators after it, keep the plain operator; `/` and `%` keep their
    // division check and leave the result a known integer. A chain of
    // literals alone is a constant and is left unchanged.
    auto render_checked_arithmetic(const expression& node, const std::vector<std::string>& texts)
        -> std::optional<std::string> {
        const auto level = binary_operator(*tokens_, node, 1);
        if (level != "+" && level != "-" && level != "*" && level != "/" && level != "%") {
            return std::nullopt;
        }
        if (std::all_of(node.children.begin(), node.children.end(), [](const expression& child) {
                return child.kind == expression_kind::literal;
            })) {
            return std::nullopt;
        }
        auto known = std::vector<bool>{};
        for (const auto& child : node.children) known.push_back(known_integer(child));
        auto result = texts.front();
        auto result_known = known.front();
        auto checked = false;
        for (auto index = std::size_t{1}; index < node.children.size(); ++index) {
            const auto op = binary_operator(*tokens_, node, index);
            const auto& operand = node.children[index];
            if (result_known && known[index] && (op == "+" || op == "-" || op == "*")) {
                result = std::string{op == "+" ? "cpp2::checked_add(" : op == "-"
                    ? "cpp2::checked_sub(" : "cpp2::checked_mul("} + result + ", " + texts[index] +
                    ")";
                checked = true;
                continue;
            }
            auto text = texts[index];
            if ((op == "/" || op == "%") && options_.check_division_by_zero) {
                text = checked_denominator(result, operand, text);
            }
            result += leading(node.children[index - 1].last) + op + leading(operand.first) + text;
            result_known = result_known && known[index];
        }
        if (!checked) return std::nullopt;
        return result;
    }

    auto render_binary(const expression& node) -> std::string {
        auto texts = render_children(node);
        if (auto chain = render_comparison_chain(node, texts)) return *std::move(chain);
        if (options_.check_arithmetic) {
            if (auto chain = render_checked_arithmetic(node, texts)) return *std::move(chain);
        }
        if (options_.check_division_by_zero) {
            const auto rendered = texts;
            for (auto index = std::size_t{1}; index < node.children.size(); ++index) {
                const auto op = token(node.children[index - 1].last);
                if (op == "/" || op == "%") {
                    texts[index] = checked_denominator(rendered[index - 1], node.children[index],
                                                       rendered[index]);
                }
            }
        }
        return verbatim(node, texts);
    }

    auto render_assignment(const expression& node) -> std::string {
        const auto& target = node.children[0];
        const auto& value = node.children[1];
        const auto op = token(node.op);
        if (op == "=" && target.kind == expression_kind::name && target.last == target.first + 1) {
            if (const auto* parameter = wrapped(token(target.first))) {
                const auto name = std::string{token(target.first)};
                auto text = render(value);
                const auto reassigned = !parameter->out && !constructs(target.first);
                // `x = ()` default-constructs rather than copying a temporary.
                if (!reassigned && value.kind == expression_kind::group && value.children.empty()) {
                    return cpp1_identifier(name) + ".construct()";
                }
                // A list, and a literal of arithmetic type, is converted before it
                // is forwarded, so a constant such as `42` does not narrow and a
                // list initializes an aggregate; a list constructs the object from
                // a returned prvalue and another single value initializes it in
                // place, either of which needs no copy or move constructor.
                const auto has_literal = std::any_of(
                    tokens_->begin() + static_cast<std::ptrdiff_t>(value.first),
                    tokens_->begin() + static_cast<std::ptrdiff_t>(value.last),
                    [](const expression_token& token) {
                        return token.kind == expression_token_kind::number ||
                            token.kind == expression_token_kind::character ||
                            token.kind == expression_token_kind::string;
                    });
                if (!parameter->type.empty() && (value.kind == expression_kind::group ||
                        (has_literal && (is_arithmetic_type(parameter->type) || reassigned)))) {
                    text = "std::type_identity_t<" +
                        parameter->type + ">{" +
                        (value.kind == expression_kind::group ? text.substr(1, text.size() - 2)
                                                              : text) + "}";
                }
                if (!reassigned && !parameter->type.empty() &&
                    value.kind == expression_kind::group) {
                    return cpp1_identifier(name) + ".construct_from([&]{ return " + text + "; })";
                }
                return cpp1_identifier(name) +
                    (reassigned ? ".value() = " + text : ".construct(" + text + ")");
            }
        }
        auto texts = std::vector<std::string>{render_lvalue(target), render(value)};
        if (options_.check_arithmetic && (op == "+=" || op == "-=" || op == "*=") &&
            known_integer(target) && known_integer(value)) {
            return std::string{op == "+=" ? "cpp2::checked_add_assign(" : op == "-="
                ? "cpp2::checked_sub_assign(" : "cpp2::checked_mul_assign("} + texts[0] + ", " +
                texts[1] + ")";
        }
        if (options_.check_division_by_zero && (op == "/=" || op == "%=")) {
            texts[1] = checked_denominator(texts[0], value, texts[1]);
        }
        // `v = (1, 2)` assigns a braced initializer list; `v: T = (1, 2)` is a declaration.
        if (op == "=" && value.kind == expression_kind::group && !value.children.empty() &&
            (target.first == 0 || token(target.first - 1) != ":") &&
            texts[1].size() >= 2 && texts[1].front() == '(' && texts[1].back() == ')') {
            texts[1] = "{ " + texts[1].substr(1, texts[1].size() - 2) + " }";
        }
        return verbatim(node, texts);
    }
};

}

lazy_objects_scope::lazy_objects_scope(std::vector<lazy_object> objects)
    : objects_{std::move(objects)}, previous_{current_lazy_objects} {
    current_lazy_objects = &objects_;
}

lazy_objects_scope::~lazy_objects_scope() { current_lazy_objects = previous_; }

auto render_cpp1_expression(std::string_view text, cpp1_expression_options options) -> std::string {
    auto parsed = parsed_expression{std::string{text}, options.experiments.enabled("try-operator")};
    return render_cpp1_expression(parsed, std::move(options));
}

namespace {

// `options` without the wrapped parameters that do not apply to `expression`.
auto applicable(const parsed_expression& expression, cpp1_expression_options options)
    -> cpp1_expression_options {
    // A wrapped declaration the text does not name, or hidden by another of the
    // same name, does not apply. A string literal or a function expression may
    // name any through an interpolation or a capture.
    const auto& tokens = expression.tree().tokens;
    auto named = std::unordered_set<std::string_view>{};
    auto opaque = false;
    for (const auto& token : tokens) {
        opaque = opaque || token.kind == expression_token_kind::string ||
            token.text.starts_with("__cpp2_function_expression_");
        if (token.kind == expression_token_kind::identifier) named.insert(token.text);
    }
    std::erase_if(options.wrapped_parameters, [&](const auto& parameter) {
        if (!opaque && !named.contains(parameter.name)) return true;
        return options.scopes != nullptr && parameter.entry != nullptr && options.point.valid() &&
            options.scopes->lookup(parameter.name, options.point) != parameter.entry;
    });
    return options;
}

}

auto render_cpp1_expression(const parsed_expression& expression, cpp1_expression_options options)
    -> std::string {
    const auto applied = applicable(expression, std::move(options));
    return tree_renderer{applied}.render_parsed(expression.tree());
}

auto render_cpp1_expression_items(const parsed_expression& expression,
                                  cpp1_expression_options options) -> std::vector<std::string> {
    const auto applied = applicable(expression, std::move(options));
    return tree_renderer{applied}.render_items(expression.tree());
}

auto render_cpp1_condition(const parsed_expression& condition, cpp1_expression_options options,
                           std::string_view name) -> std::vector<cpp1_condition_operand> {
    const auto applied = applicable(condition, std::move(options));
    return tree_renderer{applied}.render_condition(condition.tree(), name);
}

auto render_cpp1_let_else(const parsed_expression& initializer, cpp1_expression_options options,
                          std::string_view name) -> cpp1_condition_operand {
    const auto applied = applicable(initializer, std::move(options));
    return tree_renderer{applied}.render_let_else(initializer.tree(), name);
}


auto parse_dependent_type(const std::vector<expression_token>& tokens, std::size_t first,
                          std::size_t last, const scope_model* scopes, scope_point point,
                          std::vector<std::string> parameters, type_parse_options type_options)
    -> type_id {
    auto type = parse_type(tokens, first, last, type_options);
    for (auto index = first; scopes != nullptr && index < last; ++index) {
        const auto& value = tokens[index];
        if (value.kind != expression_token_kind::identifier) continue;
        if (is_dependent_scope(scopes->lookup(value.text, point), *scopes, 0)) {
            parameters.push_back(value.text);
        }
    }
    mark_dependent_names(type, parameters);
    return type;
}

auto parse_dependent_type(std::string_view text, const scope_model* scopes, scope_point point,
                          std::vector<std::string> parameters, type_parse_options type_options)
    -> type_id {
    const auto parsed = parse_expression_text(text);
    return parse_dependent_type(parsed.tokens, 0, parsed.tokens.size(), scopes, point,
                                std::move(parameters), type_options);
}

namespace {

// The declared type of the deferred local or `out` parameter `found`, if it is one.
auto wrapped_type(const scope_model::entry& found) -> const type_id* {
    const auto* parameter = found.declared_parameter;
    const auto out = found.kind == scope_model::entry_kind::parameter && parameter != nullptr &&
        parameter->passing == passing_style::out && parameter->name != "this";
    const auto* statement = found.declaring_statement;
    const auto deferred = found.kind == scope_model::entry_kind::object && statement != nullptr &&
        statement->is<statement::declaration_form>() &&
        statement->as<statement::declaration_form>().declaration &&
        !statement->as<statement::declaration_form>().declaration->is_alias &&
        statement->as<statement::declaration_form>().declaration->initializer.empty();
    return (out || deferred) && !found.type.is_wildcard() ? &found.type : nullptr;
}

// `decltype(name)` and the rest of `text` after it, such as `::value_type`.
auto decltype_operand(std::string_view text) -> std::optional<std::pair<std::string, std::string>> {
    const auto parsed = parse_expression_text(text);
    const auto& tokens = parsed.tokens;
    if (tokens.size() < 4 || (tokens[0].text != "decltype" && tokens[0].text != "type_of") ||
        tokens[1].text != "(" || tokens[2].kind != expression_token_kind::identifier ||
        tokens[3].text != ")" || (tokens.size() > 4 && tokens[4].text != "::")) {
        return std::nullopt;
    }
    const auto close = text.find(')');
    return std::pair{tokens[2].text, std::string{text.substr(close + 1)}};
}

auto declared_types(type_id type,
                    const std::vector<cpp1_expression_options::wrapped_parameter>& wrapped,
                    const scope_model* scopes, scope_point point, int depth) -> type_id {
    const auto operand = type.kind == type_kind::decltype_of || type.kind == type_kind::cpp1
        ? decltype_operand(type.text) : std::nullopt;
    if (operand) {
        const auto& [name, rest] = *operand;
        auto declared = std::string{};
        if (scopes != nullptr && point.valid()) {
            // A name `x` finds where the type is written.
            const auto* found = scopes->lookup(name, point);
            const auto* declared_type = found == nullptr || depth > 8 ? nullptr : wrapped_type(*found);
            if (declared_type != nullptr) {
                declared = render_cpp1_type(declared_types(*declared_type, {}, scopes,
                                                           {found->scope, found->ordinal},
                                                           depth + 1));
            }
        }
        else {
            const auto found = std::find_if(wrapped.begin(), wrapped.end(),
                [&](const auto& value) { return value.name == name; });
            if (found != wrapped.end() && found->type != "auto") declared = found->type;
        }
        if (declared.empty()) return type;
        auto result = type_id{};
        result.kind = type_kind::cpp1;
        result.text = rest.empty() ? declared : "typename " + declared + rest;
        result.span = type.span;
        return result;
    }
    for (auto& child : type.children) {
        child = declared_types(std::move(child), wrapped, scopes, point, depth);
    }
    for (auto& component : type.components) {
        for (auto& argument : component.arguments) {
            argument = declared_types(std::move(argument), wrapped, scopes, point, depth);
        }
    }
    return type;
}

}

auto with_declared_types(
    type_id type, const std::vector<cpp1_expression_options::wrapped_parameter>& wrapped)
    -> type_id {
    return declared_types(std::move(type), wrapped, nullptr, {}, 0);
}

auto with_declared_types(type_id type, const cpp1_expression_options& options) -> type_id {
    return declared_types(std::move(type), options.wrapped_parameters, options.scopes,
                          options.point, 0);
}

auto interpolated_expressions(std::string_view literal) -> std::vector<std::string> {
    auto result = std::vector<std::string>{};
    const auto raw = literal.starts_with("$R\"");
    for (const auto& [open, close] : detail::scan_interpolations(literal).parts) {
        result.emplace_back(split_interpolation(
            raw ? std::string{literal.substr(open + 1, close - open - 1)}
                : unescape_interpolation(literal, open + 1, close)).first);
    }
    return result;
}

auto interpolated_offset(std::string_view literal, std::size_t ordinal, std::size_t offset)
    -> std::size_t {
    const auto [open, close] = detail::scan_interpolations(literal).parts.at(ordinal);
    if (literal.starts_with("$R\"")) return open + 1 + offset;
    // As `unescape_interpolation` reads it.
    auto read = std::size_t{0};
    for (auto index = open + 1; index < close; ++index) {
        const auto at = index;
        if (literal[index] == '\\') {
            if (literal[index - 1] == '\'') ++read;
            if (index + 1 < close) ++index;
        }
        if (++read > offset) return at;
    }
    return close;
}

auto is_type_subject(const std::vector<expression_token>& tokens, const expression& node,
                     const scope_model& scopes, scope_point point) -> bool {
    const auto options = cpp1_expression_options{.scopes = &scopes, .point = point};
    return tree_renderer{options}.is_type_subject(tokens, node);
}

auto is_cpp2_support_name(std::string_view name) -> bool {
    return std::find(support_names.begin(), support_names.end(), name) != support_names.end();
}

auto is_cpp2_support_name(std::string_view name, const experiment_set& experiments) -> bool {
    return is_cpp2_support_name(name) ||
        (experiments.enabled("checked-arithmetic") &&
         std::find(wrapping_names.begin(), wrapping_names.end(), name) != wrapping_names.end());
}

auto binary_operator(const std::vector<expression_token>& tokens, const expression& node,
                     std::size_t index) -> std::string {
    auto result = std::string{};
    for (auto at = node.children[index - 1].last; at < node.children[index].first; ++at) {
        result += tokens[at].text;
    }
    return result;
}

auto comparison_kind(std::string_view op) noexcept -> char {
    return op == "<" || op == "<=" ? 'l' : op == ">" || op == ">=" ? 'g'
        : op == "==" ? 'e' : op == "!=" ? 'n' : '\0';
}

}
