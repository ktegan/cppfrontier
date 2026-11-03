#include <cppfrontier/pipeline.hpp>

#include "brackets.hpp"

#include <algorithm>
#include <array>

namespace cppfrontier {
namespace {

struct print_failure final {};

constexpr std::array<std::string_view, 8> passing_words{{
    "in", "copy", "inout", "out", "move", "forward", "in_ref", "forward_ref",
}};

constexpr std::array<std::array<std::string_view, 5>, 11> binary_levels{{
    {"..<", "..="}, {"||"}, {"&&"}, {"|"}, {"^"}, {"&"}, {"==", "!="}, {"<", ">", "<=", ">=", "<=>"},
    {"<<", ">>"}, {"+", "-"}, {"*", "/", "%"},
}};

constexpr std::array<std::string_view, 11> assignment_operators{{
    "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=",
}};

template<std::size_t N>
auto contains(const std::array<std::string_view, N>& values, std::string_view value) -> bool {
    return !value.empty() && std::find(values.begin(), values.end(), value) != values.end();
}

auto pre(int indent) -> std::string { return std::string(static_cast<std::size_t>(indent) * 4, ' '); }

// Upstream's lexer expansion of an interpolated string literal:
// `"a(x)$b"` reads as `"a" + cpp2::to_string(x) + "b"`.
auto expand_interpolation(const std::string& text) -> std::string {
    const auto quote = text.find('"');
    if (quote == std::string::npos || text.back() != '"' || text.starts_with('$') ||
        (quote > 0 && text[quote - 1] == 'R')) {
        return text;
    }
    const auto prefix = text.substr(0, quote + 1);
    struct part final {
        bool is_code;
        std::string text;
    };
    auto parts = std::vector<part>{};
    auto current = quote + 1;
    auto escape = false;
    for (auto position = current; position + 1 < text.size(); ++position) {
        escape = text[position] == '\\' && !escape;
        if (text[position] != '$' || text[position - 1] != ')') continue;
        auto depth = 1;
        auto open = position - 2;
        for (; open > current; --open) {
            if (text[open] == ')') ++depth;
            else if (text[open] == '(' && --depth == 0) break;
        }
        if (text[open] != '(') return text;
        if (open != current || parts.empty()) parts.push_back({false, text.substr(current, open - current)});
        auto chunk = text.substr(open, position - open);
        auto unescaped = std::string{};
        auto previous = ' ';
        auto escaping = false;
        for (const auto character : chunk) {
            escaping = !escaping && previous != '\'' && character == '\\';
            previous = character;
            if (!escaping) unescaped += character;
        }
        chunk = unescaped;
        if (const auto colon = chunk.find_last_of(':');
            colon != std::string::npos && colon > 0 && chunk[colon - 1] != ':') {
            chunk.insert(colon, ", \"{");
            chunk.insert(chunk.size() - 1, "}\"");
        }
        parts.push_back({true, "cpp2::to_string" + chunk});
        current = position + 1;
    }
    if (std::none_of(parts.begin(), parts.end(), [](const part& value) { return value.is_code; })) {
        return text;
    }
    parts.push_back({false, text.substr(current, text.size() - current - 1)});
    auto result = parts.front().is_code ? parts.front().text : prefix + parts.front().text;
    for (auto index = std::size_t{1}; index < parts.size(); ++index) {
        const auto& left = parts[index - 1];
        const auto& right = parts[index];
        if (!left.is_code && right.is_code) result += "\" + " + right.text;
        else if (left.is_code && !right.is_code) result += " + " + prefix + right.text;
        else if (!left.is_code) result += right.text;
        else result += " + " + right.text;
    }
    return result + (parts.back().is_code ? "" : "\"");
}

// Upstream's `pretty_print_visualize`, reading the declaration's tokens.
class printer final {
public:
    explicit printer(const std::vector<expression_token>& tokens) : tokens_{tokens} {}

    auto top_level() -> std::string {
        auto result = declaration(0, parent_kind::none, 0);
        if (position_ != tokens_.size()) throw print_failure{};
        return result;
    }

private:
    enum class parent_kind { none, type, function, object, parameter, template_parameter };

    const std::vector<expression_token>& tokens_;
    std::size_t position_ = 0;
    int types_ = 0;
    // Upstream's `last_parent_type`: a type's first member is not preceded by a blank line.
    int last_parent_ = -1;
    // The unnamed declaration being read is a template argument `:T = value`.
    bool template_value_ = false;

    [[nodiscard]] auto text(std::size_t offset = 0) const -> std::string_view {
        const auto index = position_ + offset;
        return index < tokens_.size() ? std::string_view{tokens_[index].text} : std::string_view{};
    }
    [[nodiscard]] auto at(std::string_view value) const -> bool { return text() == value; }
    [[nodiscard]] auto kind(std::size_t offset = 0) const -> expression_token_kind {
        const auto index = position_ + offset;
        if (index >= tokens_.size()) return expression_token_kind::punctuation;
        return tokens_[index].kind;
    }
    [[nodiscard]] auto joined(std::size_t offset) const -> bool {
        const auto index = position_ + offset;
        return index < tokens_.size() && tokens_[index].leading.empty();
    }
    [[nodiscard]] auto is_identifier(std::size_t offset = 0) const -> bool {
        return kind(offset) == expression_token_kind::identifier;
    }
    auto take() -> std::string {
        if (position_ >= tokens_.size()) throw print_failure{};
        return tokens_[position_++].text;
    }
    auto expect(std::string_view value) -> void {
        if (!at(value)) throw print_failure{};
        ++position_;
    }
    auto accept(std::string_view value) -> bool {
        if (!at(value)) return false;
        ++position_;
        return true;
    }

    // The operator at the cursor, joining `>` with following `>` and `=`.
    [[nodiscard]] auto operator_at() const -> std::pair<std::string, std::size_t> {
        if (text() != ">") return {std::string{text()}, 1};
        if (text(1) == ">" && joined(1)) {
            if (text(2) == "=" && joined(2)) return {">>=", 3};
            return {">>", 2};
        }
        if (text(1) == "=" && joined(1)) return {">=", 2};
        return {">", 1};
    }

    // ---- declarations

    auto declaration(int indent, parent_kind parent, int parent_type) -> std::string {
        const auto is_parameter =
            parent == parent_kind::parameter || parent == parent_kind::template_parameter;
        auto access = std::string{};
        if ((at("public") || at("protected") || at("private")) && text(1) != ":") {
            access = take() + " ";
        }
        auto name = std::string{};
        if (at("operator")) {
            while (!at(":") && position_ < tokens_.size()) name += take();
        }
        else if (!at(":")) {
            if (!is_identifier()) throw print_failure{};
            name = take();
        }
        const auto variadic = accept("...");
        if (is_parameter && (name == "this" || name == "that")) {
            return access + name;
        }

        auto metafunctions = std::vector<std::string>{};
        auto template_parameters = std::string{};
        auto requires_clause = std::string{};
        auto is_final = false;
        enum class form { object, function, type, namespace_node, type_alias, namespace_alias,
                          object_alias };
        auto shape = form::object;
        auto type = std::string{};
        auto function_type = std::string{};
        auto alias = std::string{};
        auto named_returns = false;

        const auto has_colon = accept(":");
        if (!has_colon) {
            if (!is_parameter && parent != parent_kind::type) throw print_failure{};
            if (parent == parent_kind::template_parameter) shape = form::type;
        }
        else {
            for (;;) {
                if (accept("@")) {
                    metafunctions.push_back(metafunction());
                }
                else if (at("<") && template_parameters.empty() && shape == form::object &&
                         type.empty() && function_type.empty()) {
                    template_parameters = " " + parameter_list(indent + 1, "<", ">", list_kind::templates);
                }
                else if (at("final") && (text(1) == "type" || text(1) == "@")) {
                    ++position_;
                    is_final = true;
                }
                else {
                    break;
                }
            }
            if (accept("type")) {
                shape = form::type;
                if (accept("==")) {
                    shape = form::type_alias;
                    if (at("requires")) requires_clause = requires_expression(indent);
                    alias = type_id(indent);
                }
                else if (at("requires")) {
                    requires_clause = requires_expression(indent);
                }
            }
            else if (accept("namespace")) {
                shape = form::namespace_node;
                if (accept("==")) {
                    shape = form::namespace_alias;
                    alias = id_expression(indent, true);
                }
            }
            else if (at("(")) {
                shape = form::function;
                function_type = parameter_list(indent, "(", ")", list_kind::function);
                auto has_return = false;
                for (;;) {
                    if (accept("throws")) {
                        function_type += " throws";
                    }
                    else if (accept("->")) {
                        has_return = true;
                        if (at("(")) {
                            named_returns = true;
                            function_type += " -> " +
                                parameter_list(indent + 1, "(", ")", list_kind::returns);
                        }
                        else {
                            auto passing = std::string{"move"};
                            if ((at("forward") || at("forward_ref")) && text(1) != "::") {
                                passing = take();
                            }
                            const auto returned = type_id(indent + 1);
                            if (returned != "void") function_type += " -> " + passing + " " + returned;
                        }
                    }
                    else if ((at("pre") || at("post")) && (text(1) == "(" || text(1) == "<")) {
                        function_type += contract(indent + 1);
                    }
                    else if (at("requires")) {
                        requires_clause = requires_expression(indent);
                    }
                    else {
                        break;
                    }
                }
                // As upstream, a function with an expression body returns `forward _`,
                // except `operator=` and `main`.
                if (!has_return && (at("=") || at("==")) && text(1) != "{" && text(1) != ";" &&
                    name != "operator=" && (name != "main" || parent != parent_kind::none)) {
                    function_type += " -> forward _";
                }
            }
            else if (at("==") || at("requires")) {
                if (at("requires")) requires_clause = requires_expression(indent);
                shape = form::object_alias;
            }
            else if (!at("=") && !at(";") && !at(",") && !at(")") && !at(">")) {
                type = type_id(indent);
                if (at("requires")) requires_clause = requires_expression(indent);
            }
            if (shape == form::object && accept("==")) shape = form::object_alias;
            else if (shape == form::object_alias) expect("==");
            if (shape == form::object_alias) alias = expression(indent);
        }

        const auto is_alias = shape == form::type_alias || shape == form::namespace_alias ||
            shape == form::object_alias;
        auto initializer = std::string{};
        if (is_alias) {
            accept(";");
        }
        else if (at("=") || (at("==") && shape == form::function)) {
            const auto constexpr_function = take() == "==";
            const auto adjusted = name.empty() ? indent + 1 : indent;
            initializer = constexpr_function ? " == " : " = ";
            if (is_parameter) {
                initializer += parent == parent_kind::template_parameter && shape == form::type
                    ? type_id(adjusted)
                    : expression(adjusted, parent == parent_kind::template_parameter);
            }
            else if (shape == form::type || shape == form::namespace_node) {
                const auto id = types_++;
                initializer += members(adjusted, id);
            }
            else if (name.empty() && std::exchange(template_value_, false)) {
                initializer += expression(adjusted, true);
            }
            else {
                initializer += statement(adjusted, false,
                    shape == form::function ? parent_kind::function : parent_kind::object,
                    named_returns).text;
            }
            if (initializer.ends_with(";;")) initializer.pop_back();
        }
        else if (!is_parameter) {
            expect(";");
            initializer = ";";
        }

        auto result = std::string{};
        if (parent != parent_kind::function && parent != parent_kind::object && !is_parameter) {
            if (parent == parent_kind::type) {
                if (last_parent_ != parent_type) last_parent_ = parent_type;
                else result += "\n";
            }
            else {
                result += "\n";
            }
        }
        if (!is_parameter && !name.empty()) result += "\n" + pre(indent);
        result += access + name + (variadic ? "..." : "") + ":";

        auto meta = std::string{};
        if (!metafunctions.empty()) {
            meta = "/*";
            for (const auto& value : metafunctions) meta += " @" + value;
            meta += " */";
        }
        switch (shape) {
        case form::function:
            result += meta + template_parameters + function_type + requires_clause + initializer;
            break;
        case form::object:
            result += meta + template_parameters + " " + (type.empty() ? "_" : type) +
                requires_clause + initializer;
            break;
        case form::type:
            result += meta + template_parameters + " " + (is_final ? "final type" : "type") +
                requires_clause + initializer;
            break;
        case form::namespace_node:
            result += "namespace = " + initializer;
            break;
        case form::type_alias:
            result += template_parameters + " type" + requires_clause + " == " + alias + ";";
            break;
        case form::namespace_alias:
            result += template_parameters + " namespace == " + alias + ";";
            break;
        case form::object_alias:
            result += template_parameters + (type.empty() ? "" : " " + type) + requires_clause +
                " == " + alias;
            if (!result.ends_with(';')) result += ";";
            break;
        }
        return result;
    }

    // `@name`, with `<arguments>` for the enum metafunctions or string arguments,
    // as in `@enum<u8>` and `@autodiff<"reverse">`.
    auto metafunction() -> std::string {
        auto result = take();
        while (at("::")) result += take() + take();
        // As the parser reads them, arguments follow the name directly and
        // contain no ':'; otherwise the list is the type's template parameters.
        if (!at("<") || !joined(0)) return result;
        const auto close = detail::angle_close(tokens_, position_, tokens_.size());
        if (!close || std::any_of(tokens_.begin() + static_cast<std::ptrdiff_t>(position_),
                                  tokens_.begin() + static_cast<std::ptrdiff_t>(*close),
                                  [](const expression_token& value) { return value.text == ":"; })) {
            return result;
        }
        while (position_ <= *close) result += take();
        return result;
    }

    // `requires` and its constraint, which ends before `=` or `==`.
    auto requires_expression(int indent) -> std::string {
        expect("requires");
        return " requires (" + binary(indent, 0, true) + ")";
    }

    // A type or namespace body.
    auto members(int indent, int id) -> std::string {
        expect("{");
        auto result = "\n" + pre(indent) + "{";
        while (!at("}")) {
            if (position_ >= tokens_.size()) throw print_failure{};
            result += at("using") ? using_statement(indent + 1)
                                  : declaration(indent + 1, parent_kind::type, id);
        }
        ++position_;
        return result + "\n" + pre(indent) + "}";
    }

    enum class list_kind { function, templates, returns };

    auto parameter_list(int indent, std::string_view open, std::string_view close,
                        list_kind list) -> std::string {
        expect(open);
        auto parameters = std::vector<std::string>{};
        while (!at(close)) {
            parameters.push_back(parameter(indent + 1, list));
            if (!accept(",") && !at(close)) throw print_failure{};
        }
        ++position_;
        const auto space = parameters.size() > 1 ? "\n" + pre(indent + 1) : std::string{};
        auto result = std::string{open};
        for (const auto& value : parameters) result += space + value + ", ";
        if (parameters.size() > 1) result += "\n" + pre(indent);
        return result + std::string{close};
    }

    auto parameter(int indent, list_kind list) -> std::string {
        if (list == list_kind::templates) {
            return declaration(indent, parent_kind::template_parameter, 0);
        }
        auto result = std::string{};
        if ((at("implicit") || at("virtual") || at("override") || at("final")) &&
            is_identifier(1)) {
            result += take() + " ";
        }
        auto passing = std::string{list == list_kind::returns ? "out" : "in"};
        if (contains(passing_words, text()) && (is_identifier(1) || text(1) == "...")) {
            passing = take();
        }
        return result + passing + " " + declaration(indent, parent_kind::parameter, 0);
    }

    auto contract(int indent) -> std::string {
        auto result = "\n" + pre(indent) + take();
        if (accept("<")) {
            result += "<" + id_expression(indent, false);
            while (accept(",")) result += "," + expression(indent, true);
            expect(">");
            result += ">";
        }
        expect("(");
        result += "( " + expression(indent);
        if (accept(",") && !at(")")) result += ", " + expression(indent);
        accept(",");
        expect(")");
        return result + " )";
    }

    // ---- statements

    struct printed_statement final {
        std::string text;
        bool is_return = false;
    };

    auto compound(int indent, bool add_return = false) -> std::string {
        auto try_block = accept("try");
        expect("{");
        auto result = "\n" + pre(indent) + (try_block ? "try " : "") + "{";
        auto last_is_return = false;
        while (!at("}")) {
            if (position_ >= tokens_.size()) throw print_failure{};
            const auto value = statement(indent + 1, true, parent_kind::function, false);
            result += value.text;
            last_is_return = value.is_return;
        }
        ++position_;
        if (add_return && !last_is_return) result += "\n" + pre(indent + 1) + "return;";
        result += "\n" + pre(indent) + "}";
        while (try_block && accept("catch")) {
            expect("(");
            result += "\n" + pre(indent) + "catch (" + parameter(indent + 1, list_kind::function) +
                " ) ";
            expect(")");
            result += compound(indent);
        }
        return result;
    }

    [[nodiscard]] auto statement_parameters_follow() const -> bool {
        auto depth = 0;
        for (auto index = position_; index < tokens_.size(); ++index) {
            const auto& value = tokens_[index].text;
            if (value == "(") ++depth;
            else if (value == ")" && --depth == 0) {
                if (index + 1 >= tokens_.size()) return false;
                const auto& next = tokens_[index + 1];
                return next.text == "{" ||
                    (next.kind == expression_token_kind::identifier && next.text != "is" &&
                     next.text != "as");
            }
        }
        return false;
    }

    // `(a, b) :=`, a destructuring declaration (experiment `destructuring`);
    // `:=` is the two tokens `:` `=`.
    [[nodiscard]] auto destructuring_follows() const -> bool {
        const auto is_name = [](const expression_token& value) {
            return value.kind == expression_token_kind::identifier;
        };
        return detail::destructuring_names_end(tokens_, position_, is_name).has_value();
    }

    auto statement(int indent, bool compound_parent, parent_kind parent, bool add_return)
        -> printed_statement {
        if (at("{") || (at("try") && text(1) == "{")) return {compound(indent, add_return)};
        if (at("(") && destructuring_follows()) {
            auto result = "\n" + pre(indent) + take();
            while (!accept(")")) {
                result += take();
                if (accept(",")) result += ", ";
            }
            expect(":");
            expect("=");
            result += ") := " + expression(indent);
            accept(";");
            return {result + ";"};
        }
        if (at("(") && statement_parameters_follow()) {
            auto result = "\n" + pre(indent) + parameter_list(indent, "(", ")", list_kind::function);
            return {result + statement(indent, compound_parent, parent, false).text};
        }
        if (at("if")) return {selection(indent, false)};
        if (is_identifier() && text(1) == ":" &&
            (text(2) == "while" || text(2) == "do" || text(2) == "for")) {
            const auto label = take();
            ++position_;
            return {iteration(indent, label + ": ")};
        }
        if (at("while") || at("do") || at("for")) return {iteration(indent, {})};
        if (accept("return")) {
            auto result = "\n" + pre(indent) + "return";
            if (!at(";")) result += " " + expression(indent);
            accept(";");
            if (!result.ends_with(';')) result += ";";
            return {result, true};
        }
        if (at("break") || at("continue")) {
            auto result = "\n" + pre(indent) + take();
            if (!at(";")) result += " " + take();
            expect(";");
            return {result + ";"};
        }
        if (at("using")) return {using_statement(indent)};
        if (at("assert") && (text(1) == "(" || text(1) == "<")) {
            auto result = contract(indent);
            accept(";");
            return {result + ";"};
        }
        if ((is_identifier() && text(1) == ":") ||
            ((at("public") || at("protected") || at("private")) && text(2) == ":")) {
            return {declaration(indent, parent, 0)};
        }
        auto result = compound_parent ? "\n" + pre(indent) : std::string{};
        result += expression(indent);
        if (accept(";") && !result.ends_with(';')) result += ";";
        return {result};
    }

    auto using_statement(int indent) -> std::string {
        expect("using");
        auto result = "\n" + pre(indent) + "using ";
        auto id = std::string{};
        while (!at(";")) id += take();
        ++position_;
        if (id.ends_with("::_")) {
            id.resize(id.size() - 3);
            result += "namespace ";
        }
        return result + id + ";";
    }

    auto selection(int indent, bool no_indent) -> std::string {
        expect("if");
        auto result = (no_indent ? std::string{} : "\n" + pre(indent)) + "if ";
        if (accept("constexpr")) result += "constexpr ";
        result += expression(indent);
        result += compound(indent);
        if (accept("else")) {
            result += "\n" + pre(indent) + "else ";
            result += at("if") ? selection(indent, true) : else_branch(indent);
        }
        return result;
    }

    // An else block holding just an if statement prints as `else if`.
    auto else_branch(int indent) -> std::string {
        if (at("{") && text(1) == "if") {
            const auto start = position_++;
            auto result = selection(indent, true);
            if (accept("}")) return result;
            position_ = start;
        }
        return compound(indent);
    }

    auto iteration(int indent, const std::string& label) -> std::string {
        const auto next = [&] {
            if (!accept("next")) return std::string{};
            auto result = "\n" + pre(indent) + "next " + expression(indent);
            accept(";");
            return result;
        };
        auto result = "\n" + pre(indent) + label;
        if (accept("while")) {
            const auto condition = expression(indent);
            const auto next_expression = next();
            return result + "while " + condition + next_expression + compound(indent + 1);
        }
        if (accept("do")) {
            const auto body = compound(indent + 1);
            const auto next_expression = next();
            expect("while");
            result += "do " + body + next_expression + "\n" + pre(indent) + "while " +
                expression(indent);
            accept(";");
            if (!result.ends_with(';')) result += ";";
            return result;
        }
        expect("for");
        const auto range = expression(indent);
        const auto next_expression = next();
        expect("do");
        expect("(");
        const auto parameter_text = parameter(indent + 1, list_kind::function);
        expect(")");
        return result + "for " + range + next_expression + "\n" + pre(indent) + "do (" +
            parameter_text + ")" +
            statement(indent + 1, true, parent_kind::function, false).text;
    }

    // ---- expressions

    auto expression(int indent, bool in_template = false) -> std::string {
        auto lhs = binary(indent, 0, false, in_template);
        const auto [op, length] = operator_at();
        if (!contains(assignment_operators, op) || in_template) return lhs;
        position_ += length;
        return lhs + " " + op + " " + expression(indent);
    }

    // `constraint` stops before `==`, which begins an alias value, and
    // `in_template` before `>`.
    auto binary(int indent, std::size_t level, bool constraint, bool in_template = false)
        -> std::string {
        if (level == binary_levels.size()) return is_as(indent);
        auto result = binary(indent, level + 1, constraint, in_template);
        for (;;) {
            const auto [op, length] = operator_at();
            if (!contains(binary_levels[level], op) || (constraint && op == "==") ||
                (in_template && op.starts_with('>'))) {
                break;
            }
            position_ += length;
            // As upstream, a range has no spaces: `1..<3`.
            const auto space = op.starts_with("..") ? "" : " ";
            result += space + op + space + binary(indent, level + 1, constraint, in_template);
        }
        return result;
    }

    auto is_as(int indent) -> std::string {
        auto result = prefix(indent);
        while (at("is") || at("as")) {
            const auto op = take();
            const auto value = op == "is" &&
                (at("(") || at("-") || at("+") ||
                 (kind() != expression_token_kind::identifier &&
                  kind() != expression_token_kind::punctuation));
            result += " " + op + " " + (value ? prefix(indent) : type_id(indent));
        }
        return result;
    }

    auto prefix(int indent) -> std::string {
        auto result = std::string{};
        while (at("!") || at("-") || at("+") || at("~") || at("++") || at("--") || at("&") ||
               at("*")) {
            result += take();
        }
        return result + postfix(indent);
    }

    // `*` and `&` are binary when followed by an operand, `!` or `:`.
    [[nodiscard]] auto is_postfix_dereference() const -> bool {
        const auto next = text(1);
        if (next == "(") return text(2) == ")";
        return (kind(1) == expression_token_kind::punctuation && next != "::" && next != "!" &&
                next != ":") ||
            next == "is" || next == "as";
    }

    auto postfix(int indent) -> std::string {
        auto result = primary(indent);
        for (;;) {
            if (at("(")) result += expression_list(indent, "(", ")");
            else if (at("[")) result += expression_list(indent, "[", "]");
            else if (at(".") || at("..")) {
                result += take();
                result += id_expression(indent, false);
            }
            else if (at("++") || at("--") || at("$") || at("...") || at("~")) {
                result += take();
            }
            else if ((at("*") || at("&")) && is_postfix_dereference()) result += take();
            else break;
        }
        return result;
    }

    auto expression_list(int indent, std::string_view open, std::string_view close)
        -> std::string {
        expect(open);
        auto result = std::string{open};
        auto first = true;
        while (!at(close)) {
            if (!first) result += ", ";
            first = false;
            if (contains(passing_words, text()) && text() != "in" && is_identifier(1)) {
                const auto passing = take();
                if (passing == "out" || passing == "move" || passing == "forward") {
                    result += passing + " ";
                }
            }
            result += expression(indent);
            if (!accept(",") && !at(close)) throw print_failure{};
        }
        ++position_;
        return result + std::string{close};
    }

    auto primary(int indent) -> std::string {
        if (at("(")) return expression_list(indent, "(", ")");
        if (at(":")) return declaration(indent, parent_kind::object, 0);
        if (at("inspect")) return inspect(indent);
        if (at("...")) return take();
        if (kind() == expression_token_kind::string) {
            auto result = expand_interpolation(take());
            while (kind() == expression_token_kind::string) {
                result += " " + expand_interpolation(take());
            }
            return result;
        }
        if (kind() == expression_token_kind::number || kind() == expression_token_kind::character) {
            return take();
        }
        if (at("sizeof") && text(1) == "...") {
            position_ += 2;
            return "sizeof...";
        }
        if (is_identifier() || at("::")) return id_expression(indent, false);
        throw print_failure{};
    }

    auto inspect(int indent) -> std::string {
        expect("inspect");
        auto result = std::string{"inspect"};
        if (accept("constexpr")) result += " constexpr";
        result += " " + expression(indent);
        if (accept("->")) result += " -> " + type_id(indent);
        expect("{");
        result += " {";
        while (!accept("}")) {
            const auto op = take();
            if (op != "is" && op != "as") throw print_failure{};
            result += "\n" + pre(indent + 1) + op;
            const auto value = op == "is" &&
                (at("(") || at("-") || at("+") ||
                 (kind() != expression_token_kind::identifier &&
                  kind() != expression_token_kind::punctuation));
            result += " " + (value ? prefix(indent + 1) : type_id(indent + 1));
            expect("=");
            result += " = " + statement(indent + 2, false, parent_kind::function, false).text;
        }
        return result + "\n" + pre(indent) + "}";
    }

    // Whether the `<` at the cursor opens a template argument list.
    [[nodiscard]] auto opens_template_arguments() const -> bool {
        auto angles = 0;
        auto groups = 0;
        for (auto index = position_; index < tokens_.size(); ++index) {
            const auto& value = tokens_[index].text;
            if (value == "(" || value == "[") ++groups;
            else if (value == ")" || value == "]") {
                if (--groups < 0) return false;
            }
            else if (value == ";" || value == "{" || value == "}") return false;
            else if (groups != 0) continue;
            else if (value == "&&" || value == "||" || value == "=" || value == "==") return false;
            else if (value == "<") ++angles;
            else if (value == ">" && --angles == 0) {
                return index + 1 >= tokens_.size() ||
                    tokens_[index + 1].kind == expression_token_kind::punctuation;
            }
        }
        return false;
    }

    // `in_type` reads every `<` after a name as template arguments.
    auto id_expression(int indent, bool in_type) -> std::string {
        auto result = std::string{};
        if (accept("::")) result += "::";
        for (;;) {
            if (!is_identifier()) throw print_failure{};
            result += take();
            if (at("<") && (in_type || opens_template_arguments())) {
                ++position_;
                result += "<";
                auto first = true;
                while (!at(">")) {
                    if (!first) result += ", ";
                    first = false;
                    result += template_argument(indent);
                    if (!accept(",") && !at(">")) throw print_failure{};
                }
                ++position_;
                result += ">";
            }
            if (!at("::")) break;
            result += take();
        }
        return result;
    }

    auto template_argument(int indent) -> std::string {
        if (at("const") || at("*") || at("_") || (at("(") && function_type_follows())) {
            return type_id(indent);
        }
        template_value_ = at(":");
        return expression(indent, true);
    }

    // Whether the parenthesized list at the cursor is followed by `->`.
    [[nodiscard]] auto function_type_follows() const -> bool {
        auto depth = 0;
        for (auto index = position_; index < tokens_.size(); ++index) {
            const auto& value = tokens_[index].text;
            if (value == "(") ++depth;
            else if (value == ")" && --depth == 0) {
                return index + 1 < tokens_.size() && tokens_[index + 1].text == "->";
            }
        }
        return false;
    }

    auto type_id(int indent) -> std::string {
        auto result = std::string{};
        while (at("const") || at("*") || at("&")) result += take() + " ";
        if (accept("_")) {
            result += "_";
        }
        else if (at("(")) {
            result += parameter_list(indent, "(", ")", list_kind::function);
            if (accept("->")) {
                auto passing = std::string{"move"};
                if (at("forward")) passing = take();
                const auto returned = type_id(indent + 1);
                if (returned != "void") result += " -> " + passing + " " + returned;
            }
        }
        else {
            result += id_expression(indent, true);
            if (at("(")) result += expression_list(indent, "(", ")");
        }
        if (at("...")) result += take();
        if (accept("is")) result += " is " + id_expression(indent, true);
        return result;
    }
};

}

auto pretty_print_declaration(std::string_view text) -> std::optional<std::string> {
    const auto parsed = parse_expression_text(text);
    try {
        return printer{parsed.tokens}.top_level();
    }
    catch (const print_failure&) {
        return std::nullopt;
    }
}

}
