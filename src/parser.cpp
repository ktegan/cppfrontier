#include <cppfrontier/pipeline.hpp>

#include "parser_context.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cppfrontier {

namespace {

// The struct a function with named return values returns, named as upstream
// does. Empty for an operator that has no such name.
auto named_return_struct_name(std::string_view function) -> std::string {
    if (!function.starts_with("operator")) return std::string{function} + "_ret";
    if (function == "operator()") return "operator_call_ret";
    if (function == "operator[]") return "operator_subscript_ret";
    if (function == "operator*") return "operator_dereference_ret";
    return {};
}

auto is_keyword(const token& value, std::string_view text) -> bool {
    return value.kind == token_kind::keyword && value.text == text;
}

class declaration_parser final {
public:
    declaration_parser(const source_text& source, token_list& tokens,
                       std::vector<diagnostic>& diagnostics, const parse_options& options)
        : context_{source, tokens, diagnostics, options.trace, &options.replacements,
                   &options.experiments},
          tokens_{tokens} {}

    auto parse() -> ast {
        auto tree = ast{};
        while (!at_eof()) {
            const auto before = index_;
            if (!consume_declaration(tree)) {
                if (token_at(index_).text == ";") diagnose_empty_declaration();
                else {
                    add_diagnostic("unexpected-token", token_at(index_).span,
                                   "token does not begin a declaration");
                }
                ++index_;
            }
            if (index_ == before) ++index_;
        }
        return tree;
    }

    [[nodiscard]] auto binding_operators() const -> const std::vector<source_span>& {
        return context_.binding_operators();
    }

private:
    auto token_at(std::size_t position) const -> const token& {
        return context_.token_at(position);
    }

    auto at_eof() const -> bool {
        return index_ >= context_.token_count() - 1;
    }

    auto add_diagnostic(std::string_view code, source_span span, std::string_view message) -> void {
        context_.add_diagnostic(code, diagnostic_severity::error, span, message);
    }

    auto diagnose_cpp1_auto(const token& value, std::string_view name,
                            std::string_view replacement) -> void {
        add_diagnostic("cpp1-auto-type", value.span,
            "Cpp2 writes deduced types as '" + std::string{replacement} +
            "'; remove the explicit 'auto' on '" + std::string{name} + "'");
    }

    auto syntax_text(std::size_t begin, std::size_t end) const -> std::string {
        return context_.syntax_text(begin, end);
    }

    auto matching_token(std::size_t open) const -> std::optional<std::size_t> {
        return parser_detail::bracket_close(tokens_, open, context_.token_count());
    }

    // Reports and skips a destructuring declaration, a `defer` statement or an
    // `unsafe` block where a declaration or function body is expected
    // (experiments `destructuring`, `defer`, `unsafe`).
    auto skip_misplaced_statement() -> bool {
        if (at_eof()) return false;
        const auto& current = token_at(index_);
        if (context_.experiment_enabled("destructuring") &&
            parser_detail::destructuring_names_end(tokens_, index_)) {
            add_diagnostic("destructuring-scope", current.span,
                           "a destructuring declaration must be in a function body");
            skip_declaration();
            return true;
        }
        if (current.kind != token_kind::identifier) return false;
        const auto& next = token_at(index_ + 1);
        if (current.text == "defer" && context_.experiment_enabled("defer") &&
            next.text != ":" && next.text != ":=") {
            add_diagnostic("defer-scope", current.span,
                           "a defer statement must be inside a function body");
        }
        else if (current.text == "unsafe" && context_.experiment_enabled("unsafe") &&
                 next.text == "{") {
            add_diagnostic("unsafe-scope", current.span,
                           "an unsafe block must be inside a function body");
        }
        else {
            return false;
        }
        if (next.text == "{") {
            const auto close = matching_token(index_ + 1);
            index_ = close ? *close + 1 : context_.token_count() - 1;
        }
        else {
            skip_declaration();
        }
        return true;
    }

    auto skip_declaration() -> void { skip_member(index_); }

    // An operator Cpp2 cannot declare is diagnosed and skipped, through its
    // body: a Cpp1 conversion function `operator T`, `operator new`,
    // `operator delete`, `operator co_await`, and a literal operator `operator "" _x`.
    auto skip_unsupported_operator() -> bool {
        const auto& name = token_at(index_);
        if (at_eof() || name.escaped_identifier) return false;
        const auto& next = token_at(index_ + 1);
        if (name.text.starts_with("operator\"\"") || (name.text == "operator" && next.text == "\"\"")) {
            add_diagnostic("unsupported-operator", name.span,
                           "a literal operator 'operator\"\" _x' cannot be declared in Cpp2 code; "
                           "declare it in Cpp1 code");
        }
        else if (name.text != "operator") {
            return false;
        }
        else if (next.text == "new" || next.text == "delete" || next.text == "co_await") {
            add_diagnostic("unsupported-operator", name.span,
                           "'operator " + next.text + "' cannot be declared in Cpp2 code; "
                           "declare it in Cpp1 code");
        }
        else if (next.kind == token_kind::identifier || next.kind == token_kind::keyword) {
            add_diagnostic("conversion-function", name.span,
                           "conversion functions such as 'operator " + next.text +
                           "' are not supported; use a named function, or a converting "
                           "constructor 'operator=: (implicit out this, value: From)' in the "
                           "target type");
        }
        else {
            return false;
        }
        const auto end = context_.token_count() - 1;
        index_ = parser_detail::find_top_level(tokens_, index_, end,
            parser_detail::angle_rule::expression, [&](std::size_t position) {
                return token_at(position).text == ";" || token_at(position).text == "{";
            });
        if (index_ < end && token_at(index_).text == "{") {
            index_ = matching_token(index_).value_or(end - 1) + 1;
        }
        else if (index_ < end && token_at(index_).text == ";") {
            ++index_;
        }
        return true;
    }

    auto consume_name(declaration& output) -> bool {
        if (at_eof() || !is_declaration_name(token_at(index_))) return false;
        output.name = token_at(index_).text;
        ++index_;
        return true;
    }

    auto consume_type(declaration& output) -> void {
        if (at_eof() || !parser_detail::is_type_start(tokens_, index_)) return;
        const auto begin = index_;
        index_ = parser_detail::type_end(tokens_, begin, context_.token_count(),
                                         context_.experiment_enabled("type-operators"));
        output.type = context_.syntax_type(begin, index_);
    }

    // `name: final type` declares a type that cannot be derived from.
    auto consume_final(declaration& output) -> void {
        if (!at_eof() && token_at(index_).text == "final" &&
            index_ + 1 < context_.token_count() && is_keyword(token_at(index_ + 1), "type")) {
            output.is_final = true;
            ++index_;
        }
    }

    // `name: lazy type = init` declares an object initialized on first use
    // (experiment `lazy`); `lazy` followed by anything but a type is a type name.
    auto consume_lazy(declaration& output) -> void {
        if (!at_eof() && context_.experiment_enabled("lazy") &&
            token_at(index_).kind == token_kind::identifier && token_at(index_).text == "lazy" &&
            parser_detail::is_type_start(tokens_, index_ + 1)) {
            output.is_lazy = true;
            ++index_;
        }
    }

    auto diagnose_lazy(const declaration& output) -> void {
        if (!output.is_lazy) return;
        if (output.type.is_wildcard()) {
            add_diagnostic("lazy-type", output.span, "a lazy object must have a declared type");
        }
        else if (!output.body || !output.alias.empty()) {
            add_diagnostic("lazy-initializer", output.span,
                           "a lazy object must have an '=' initializer");
        }
    }

    auto consume_template_parameters(declaration& output) -> bool {
        return context_.template_parameters(index_, output.template_parameters);
    }

    auto is_contract_start(std::size_t position) const -> bool {
        const auto& current = token_at(position);
        return current.kind == token_kind::identifier &&
            (current.text == "pre" || current.text == "post") &&
            position + 1 < context_.token_count() &&
            (token_at(position + 1).text == "(" || token_at(position + 1).text == "<");
    }

    // `pre<group, flags...>(condition, message)` or `post(...)` after a signature.
    auto consume_contract(declaration& output) -> bool {
        auto contract = function_contract{};
        contract.is_post = token_at(index_).text == "post";
        if (!context_.contract(index_, contract, contract.is_post ? &contract.captures : nullptr)) {
            return false;
        }
        output.contracts.push_back(std::move(contract));
        return true;
    }

    auto consume_requires(declaration& output) -> bool {
        if (at_eof() || !is_keyword(token_at(index_), "requires")) return false;
        ++index_;
        const auto begin = index_;
        index_ = parser_detail::find_top_level(tokens_, begin, context_.token_count() - 1,
            parser_detail::angle_rule::expression, [&](std::size_t position) {
                const auto& text = token_at(position).text;
                return text == "=" || text == "==" || text == ";" || text == "}" ||
                    (text == "{" && !parser_detail::is_requires_body(tokens_, position)) ||
                    is_contract_start(position);
            });
        if (index_ == begin) {
            add_diagnostic("missing-requires-clause", token_at(index_).span,
                           "expected a constraint after 'requires'");
            return false;
        }
        output.requires_clause = parsed_expression{syntax_text(begin, index_)};
        context_.diagnose_juxtaposition(output.requires_clause, begin, index_);
        // The constraint ends the declaration's kind.
        if (is_contract_start(index_)) {
            add_diagnostic("contract-after-requires", token_at(index_).span,
                           "'" + token_at(index_).text + "' must come before the 'requires' "
                           "constraint, which ends the declaration's kind");
            return false;
        }
        return true;
    }

    auto diagnose_empty_declaration() -> void {
        add_diagnostic("empty-declaration", token_at(index_).span,
                       "empty declaration is not allowed - remove extra semicolon");
    }

    // A declaration that runs into the `}` closing its scope, the next
    // declaration or the file's end is missing its `;`.
    auto diagnose_missing_equal() -> void {
        const auto text = context_.quoted(token_at(index_));
        const auto& next = token_at(index_ + 1).text;
        if (text == "}" || token_at(index_).kind == token_kind::end_of_file ||
            (is_declaration_name(token_at(index_)) && (next == ":" || next == ":="))) {
            add_diagnostic("missing-semicolon", token_at(index_ - 1).span,
                           "expected ';' at the end of the declaration");
            return;
        }
        add_diagnostic("missing-equal-before-body", token_at(index_).span,
                       text == "{" ? std::string{"expected '=' before '{' - did you mean '= {' ?"}
                                   : "expected '=' before the function body '" + text + "'");
    }

    auto consume_using(declaration& output, std::size_t& index) -> parser_detail::using_parse_result {
        const auto start = index;
        const auto result = context_.using_declaration(index, output.using_name);
        if (result == parser_detail::using_parse_result::valid) {
            output.kind = declaration_kind::using_declaration;
            output.name = output.using_name;
        }
        else if (result == parser_detail::using_parse_result::invalid) {
            context_.diagnose_invalid_using(start);
        }
        return result;
    }

    auto consume_parameter_range(std::size_t begin, std::size_t end, parameter& output) -> bool {
        return context_.parse_parameter_range(begin, end, output);
    }

    auto push_parameter(declaration& output, parameter value) -> void {
        context_.check_in_parameter_type(value);
        output.parameters.push_back(std::move(value));
    }

    auto consume_parameters(declaration& output) -> bool {
        if (at_eof() || token_at(index_).text != "(") return false;
        const auto close = matching_token(index_);
        if (!close) {
            add_diagnostic("unterminated-parameters", token_at(index_).span, "expected ')'");
            return false;
        }
        output.kind = declaration_kind::function;
        ++index_;
        auto valid = true;
        for (auto begin = index_; begin < *close;) {
            const auto comma = parser_detail::find_top_level(tokens_, begin, *close,
                parser_detail::angle_rule::declaration,
                [&](std::size_t position) { return token_at(position).text == ","; });
            auto parsed_parameter = parameter{};
            const auto reported = context_.diagnostic_count();
            if (!consume_parameter_range(begin, comma, parsed_parameter)) {
                if (context_.diagnostic_count() == reported) {
                    add_diagnostic("invalid-parameter-declaration",
                                   token_at(begin < comma ? begin : comma).span,
                                   "expected a parameter name with an optional type");
                }
                valid = false;
            }
            else {
                push_parameter(output, std::move(parsed_parameter));
            }
            begin = comma + 1;
        }
        for (auto position = std::size_t{0}; position < output.parameters.size(); ++position) {
            const auto& value = output.parameters[position];
            if (value.name != "this") continue;
            if (output.scope != declaration_scope::type_scope) {
                add_diagnostic("this-parameter-scope", value.name_span,
                               "'this' must be the first parameter of a type-scope function");
            }
            else if (position != 0) {
                add_diagnostic("this-parameter-position", value.name_span,
                               "'this' must be the first parameter");
            }
        }
        index_ = *close + 1;
        return valid;
    }

    // `name := value` declares `name: = value`; split the token so both forms parse alike.
    auto split_colon_equals() -> void {
        if (at_eof() || token_at(index_).text != ":=") return;
        auto equals = tokens_[index_];
        equals.text = "=";
        equals.span.begin.column += 1;
        tokens_[index_].text = ":";
        tokens_[index_].span.end = equals.span.begin;
        // `name :== value` lexes as `:=` `=`.
        if (index_ + 1 < context_.token_count() && tokens_[index_ + 1].text == "=") {
            tokens_[index_ + 1].text = "==";
            tokens_[index_ + 1].span.begin = equals.span.begin;
            return;
        }
        tokens_.insert(tokens_.begin() + static_cast<std::ptrdiff_t>(index_) + 1, equals);
    }

    // The comma-separated arguments between the `<` at `open` and the `>` or
    // `>>` at `close`; the `>`s of a `>>` other than the last close lists in
    // the last argument. An empty argument is an error.
    auto consume_metaclass_arguments(std::size_t open, std::size_t close,
                                     std::vector<std::string>& output) -> void {
        auto next = open + 1;
        auto empty = std::optional<std::size_t>{};
        for (auto [begin, end] : parser_detail::top_level_items(
                 tokens_, open + 1, close + 1, parser_detail::angle_rule::type)) {
            if (begin >= close) break;
            end = std::min(end, close);
            if (begin != next && !empty) empty = next;
            output.push_back(syntax_text(begin, end) +
                             std::string(end == close ? token_at(close).text.size() - 1 : 0, '>'));
            next = end + 1;
        }
        if (next <= close && close > open + 1 && !empty) empty = next;
        if (empty) {
            add_diagnostic("empty-metaclass-argument", token_at(*empty).span,
                           "empty argument in the argument list of '@" +
                               token_at(open - 1).text + "'");
        }
    }

    auto consume_metaclass(declaration& output) -> bool {
        if (at_eof() || token_at(index_).text != "@") return false;
        if (!output.template_parameters.empty()) {
            add_diagnostic("metaclass-after-template-parameters", token_at(index_).span,
                           "metafunctions come before the template parameters, as in "
                           "'name: @meta <T> type'");
            return false;
        }
        ++index_;
        if (at_eof() || !is_declaration_name(token_at(index_))) {
            add_diagnostic("missing-metaclass", token_at(index_).span, "expected metaclass name after '@'");
            return false;
        }
        const auto& metaclass = token_at(index_).text;
        output.metaclasses.push_back(metaclass);
        output.metaclass_arguments.emplace_back();
        ++index_;
        // Metaclass arguments, such as `@enum<u8>`, `@struct<noforward>` and
        // `@autodiff<"order=2">`, follow the name directly. As upstream, a list
        // after a space, or containing ':', is the type's template parameters.
        const auto& name_end = token_at(index_ - 1).span.end;
        if (!at_eof() && token_at(index_).text == "<" &&
            token_at(index_).span.begin.line == name_end.line &&
            token_at(index_).span.begin.column == name_end.column) {
            const auto open = index_;
            const auto close = parser_detail::angle_close(tokens_, open, context_.token_count());
            if (close && std::none_of(tokens_.begin() + static_cast<std::ptrdiff_t>(open),
                                      tokens_.begin() + static_cast<std::ptrdiff_t>(*close),
                                      [](const token& value) { return value.text == ":"; })) {
                consume_metaclass_arguments(open, *close, output.metaclass_arguments.back());
                index_ = *close + 1;
            }
        }
        // `name: @struct <T: type> type`: template parameters may follow the metaclasses.
        if (!at_eof() && token_at(index_).text == "<" && output.template_parameters.empty() &&
            !consume_template_parameters(output)) {
            return false;
        }
        consume_final(output);
        if (!at_eof() && is_keyword(token_at(index_), "type") &&
            !parser_detail::is_type_marker(tokens_, index_)) {
            output.kind = declaration_kind::type;
            output.type = {};
            ++index_;
        }
        else if (!at_eof() && is_keyword(token_at(index_), "namespace")) {
            add_diagnostic("namespace-metafunction", token_at(index_).span,
                           "(temporary alpha limitation) metafunctions are currently not "
                           "supported on namespaces, only on types");
            output.kind = declaration_kind::namespace_node;
            output.metaclasses.clear();
            output.metaclass_arguments.clear();
            ++index_;
        }
        // `x: @m int = 0` is an object, which reflection reports.
        else if (!at_eof() && token_at(index_).text != "(" && token_at(index_).text != "@" &&
                 parser_detail::is_type_start(tokens_, index_)) {
            consume_type(output);
        }
        return true;
    }

    // `(name: type [= value], ...)` after `->`. The function returns `f_ret`:
    // an alias of the type for a single value, else a struct with one member
    // per value.
    auto consume_named_returns(declaration& output) -> bool {
        const auto open = index_;
        const auto close = matching_token(open);
        if (!close) {
            add_diagnostic("unterminated-parameters", token_at(open).span, "expected ')'");
            return false;
        }
        for (const auto& [begin, end] : parser_detail::top_level_items(
                 tokens_, open + 1, *close, parser_detail::angle_rule::declaration)) {
            const auto equals = parser_detail::find_top_level(tokens_, begin, end,
                parser_detail::angle_rule::type,
                [&](std::size_t position) { return token_at(position).text == "="; });
            auto value = initialized_parameter{};
            // `out` (the default) and `forward` are the only return passing styles.
            if (!consume_parameter_range(begin, equals, value.value) ||
                value.value.type.is_wildcard() || token_at(begin).text == "in" ||
                (value.value.passing != passing_style::in &&
                 value.value.passing != passing_style::out &&
                 value.value.passing != passing_style::forward)) {
                add_diagnostic("invalid-named-return", token_at(begin).span,
                               "a named return value needs a name and a type");
                return false;
            }
            if (equals < end) value.initializer = parsed_expression{syntax_text(equals + 1, end)};
            output.named_returns.push_back(std::move(value));
        }
        if (output.named_returns.empty()) {
            add_diagnostic("invalid-named-return", token_at(open).span,
                           "expected a named return value");
            return false;
        }
        // A function expression has no struct to return, so it returns a single
        // named value as its type.
        if (is_function_expression_name(output.name)) {
            if (output.named_returns.size() > 1) {
                add_diagnostic("unsupported-named-return", token_at(open).span,
                               "a function expression cannot return multiple values");
            }
            output.type = output.named_returns.front().value.type;
            index_ = *close + 1;
            return true;
        }
        output.type = parse_type(named_return_struct_name(output.name));
        if (output.type.empty()) {
            add_diagnostic("unsupported-named-return", token_at(open).span,
                           "named return values are not supported on " + output.name);
            return false;
        }
        index_ = *close + 1;
        return true;
    }

    auto consume_return_type(declaration& output) -> bool {
        if (at_eof() || token_at(index_).text != "->") return false;
        ++index_;
        if (!at_eof() && token_at(index_).text == "(") return consume_named_returns(output);
        // `move` (the default) and `forward` are the only return passing styles.
        if (const auto passing = context_.parameter_passing_style(token_at(index_).text);
            passing && *passing != passing_style::forward &&
            (parser_detail::is_type_start(tokens_, index_ + 1) ||
             token_at(index_ + 1).text == "_")) {
            if (*passing != passing_style::move) {
                add_diagnostic("invalid-return-passing", token_at(index_).span,
                               "only 'move' and 'forward' return passing style are allowed "
                               "from functions");
                return false;
            }
            ++index_;
        }
        if (at_eof() || (!parser_detail::is_type_start(tokens_, index_) &&
                         !is_keyword(token_at(index_), "forward") &&
                         token_at(index_).text != "_")) {
            add_diagnostic("missing-return-type", token_at(index_).span, "expected return type after '->'");
            return false;
        }
        if (token_at(index_).text == "forward" && !at_eof() &&
            index_ + 1 < context_.token_count()) {
            if (token_at(index_ + 1).text == "_") {
                output.deduction = return_deduction::by_forwarding;
                output.type = {};
                index_ += 2;
                return true;
            }
            output.deduction = return_deduction::by_forwarding_typed;
            ++index_;
        }
        else if (token_at(index_).text == "forward_ref" && !at_eof() &&
            index_ + 1 < context_.token_count() && token_at(index_ + 1).text == "_") {
            output.deduction = return_deduction::by_forwarding_ref;
            output.type = {};
            index_ += 2;
            return true;
        }
        else if (token_at(index_).text == "_") {
            output.deduction = return_deduction::by_value;
            output.type = {};
            ++index_;
            return true;
        }
        if (token_at(index_).text == "auto") {
            diagnose_cpp1_auto(token_at(index_), output.name, "-> _");
        }
        auto return_type = declaration{};
        consume_type(return_type);
        output.type = std::move(return_type.type);
        return true;
    }

    // Moves past the malformed declaration that begins at `begin`: through its
    // `;` or its body's `}`, or to the `}` that ends the enclosing body.
    auto skip_member(std::size_t begin) -> void {
        index_ = begin;
        while (!at_eof() && token_at(index_).text != "}") {
            const auto& text = token_at(index_).text;
            if (detail::is_open_bracket(text)) {
                const auto close = matching_token(index_);
                if (!close) {
                    index_ = context_.token_count() - 1;
                    return;
                }
                index_ = *close + 1;
                if (text == "{") break;
                continue;
            }
            ++index_;
            if (text == ";") break;
        }
        if (!at_eof() && token_at(index_).text == ";") ++index_;
    }

    auto parse_members(declaration& output, std::size_t open) -> bool {
        if (output.scope == declaration_scope::file) {
            output.scope = output.kind == declaration_kind::namespace_node
                ? declaration_scope::namespace_scope
                : declaration_scope::type_scope;
        }
        ++index_;
        while (!at_eof() && token_at(index_).text != "}") {
            const auto before = index_;
            auto member = std::make_unique<declaration>();
            member->span.begin = token_at(index_).span.begin;
            member->scope = output.kind == declaration_kind::namespace_node
                ? declaration_scope::namespace_scope : declaration_scope::type_scope;
            const auto using_result = consume_using(*member, index_);
            if (using_result == parser_detail::using_parse_result::valid) {
                member->span.end = index_ > before ? token_at(index_ - 1).span.end : member->span.begin;
                output.members.push_back(std::move(member));
                continue;
            }
            if (using_result == parser_detail::using_parse_result::invalid) return false;
            if (token_at(index_).kind == token_kind::keyword &&
                (token_at(index_).text == "public" || token_at(index_).text == "protected" ||
                 token_at(index_).text == "private")) {
                member->access = token_at(index_).text == "public" ? access_kind::public_access :
                    (token_at(index_).text == "protected" ? access_kind::protected_access :
                     access_kind::private_access);
                ++index_;
                if (skip_unsupported_operator()) continue;
                if (at_eof() || (!is_declaration_name(token_at(index_)) &&
                                 token_at(index_).text != "this")) {
                    add_diagnostic("invalid-member-start", token_at(index_).span,
                                   "member declaration must begin with a name");
                    return false;
                }
            }
            if (skip_unsupported_operator()) continue;
            if (skip_misplaced_statement()) continue;
            if (token_at(index_).text == ";") {
                diagnose_empty_declaration();
                ++index_;
                continue;
            }
            // `this: Base` declares a base subobject.
            const auto is_base = token_at(index_).text == "this" &&
                index_ + 1 < context_.token_count() && token_at(index_ + 1).text == ":";
            if (!is_base && !is_declaration_name(token_at(index_))) {
                add_diagnostic("invalid-member-start", token_at(index_).span,
                               "member declaration must begin with a name");
                return false;
            }
            const auto name = index_;
            member->name = token_at(index_).text;
            ++index_;
            if (token_at(index_).text == ",") {
                context_.diagnose_top_level_comma(index_ - 1, index_ + 2);
                skip_member(before);
                continue;
            }
            split_colon_equals();
            if (!at_eof() && token_at(index_).text == ":") {
                ++index_;
                if (!at_eof() && token_at(index_).text == "<" &&
                    !consume_template_parameters(*member)) {
                    return false;
                }
                consume_final(*member);
                consume_lazy(*member);
                if (!at_eof() && is_keyword(token_at(index_), "type") &&
                    !parser_detail::is_type_marker(tokens_, index_)) {
                    member->kind = declaration_kind::type;
                    ++index_;
                }
                else if (!at_eof() && is_keyword(token_at(index_), "concept")) {
                    member->kind = declaration_kind::concept_declaration;
                    ++index_;
                }
                else if (!at_eof() && is_keyword(token_at(index_), "namespace")) {
                    member->kind = declaration_kind::namespace_node;
                    if (output.kind == declaration_kind::type) {
                        add_diagnostic("namespace-in-type", token_at(index_).span,
                                       "types cannot contain namespaces");
                    }
                    ++index_;
                }
                else {
                    consume_type(*member);
                    context_.check_cpp1_auto(member->type, member->name, ":= value");
                }
            }
            auto complete = true;
            while (!at_eof() && (token_at(index_).kind == token_kind::punctuation ||
                                 is_keyword(token_at(index_), "requires") ||
                                 token_at(index_).text == "throws" || is_contract_start(index_) ||
                                 member->kind == declaration_kind::function)) {
                if (token_at(index_).text == "throws") ++index_;
                else if (is_contract_start(index_)) complete = consume_contract(*member);
                else if (token_at(index_).text == "@") complete = consume_metaclass(*member);
                else if (token_at(index_).text == "(" &&
                         member->kind != declaration_kind::function) {
                    complete = consume_parameters(*member);
                }
                else if (token_at(index_).text == "->") complete = consume_return_type(*member);
                else if (is_keyword(token_at(index_), "requires")) complete = consume_requires(*member);
                else if (token_at(index_).text == "==") {
                    if (member->kind == declaration_kind::function) {
                        complete = consume_body(*member);
                        break;
                    }
                    ++index_;
                    const auto alias_begin = index_;
                    while (!at_eof() && token_at(index_).text != ";") ++index_;
                    if (index_ == alias_begin) {
                        add_diagnostic("missing-alias-value", token_at(index_).span,
                                       "expected a type or namespace after '=='");
                        complete = false;
                    }
                    else {
                        member->alias = syntax_text(alias_begin, index_);
                        if (member->kind == declaration_kind::type) {
                            context_.check_alias_type(alias_begin, index_);
                        }
                    }
                    break;
                }
                else if (token_at(index_).text == "=") {
                    if (block_initializer(*member, name)) {
                        complete = false;
                        break;
                    }
                    if (member->kind == declaration_kind::type &&
                        index_ + 1 < context_.token_count() &&
                        token_at(index_ + 1).text == "{") {
                        ++index_;
                        complete = parse_members(*member, index_);
                        break;
                    }
                    if (member->kind == declaration_kind::concept_declaration) {
                        if (index_ + 1 < context_.token_count() &&
                            token_at(index_ + 1).text == "{") {
                            add_diagnostic("concept-requires-expression",
                                           token_at(index_ + 1).span,
                                           "a concept definition requires an expression after '='");
                            complete = false;
                            break;
                        }
                        ++index_;
                        complete = consume_expression_body(*member);
                        break;
                    }
                    if (member->kind == declaration_kind::object &&
                        !member->parameters.size() && member->name != "operator=") {
                        if (parser_detail::empty_initializer(
                                tokens_, index_, std::min(index_ + 2, context_.token_count()))) {
                            add_diagnostic("missing-initializer", token_at(index_).span,
                                           "expected an initializer after '='");
                            complete = false;
                            break;
                        }
                        ++index_;
                        const auto initializer_begin = index_;
                        index_ = parser_detail::find_top_level(tokens_, index_,
                            context_.token_count() - 1, parser_detail::angle_rule::expression,
                            [&](std::size_t position) {
                                return token_at(position).text == ";" ||
                                    (position != initializer_begin &&
                                     diagnose_declaration_start(position));
                            });
                        context_.diagnose_top_level_comma(initializer_begin, index_);
                        if (index_ != initializer_begin) expect_semicolon();
                        if (index_ != initializer_begin) {
                            member->body = std::make_unique<expression_statement>();
                            member->body->span.begin = token_at(initializer_begin).span.begin;
                            member->body->span.end = token_at(index_ - 1).span.end;
                            member->body->text = context_.expression(
                                syntax_text(initializer_begin, index_), member->body->span.begin);
                            context_.diagnose_juxtaposition(
                                member->body->text, initializer_begin, index_, false, true);
                        }
                    }
                    else {
                        complete = consume_body(*member);
                    }
                    break;
                }
                else if (token_at(index_).text == ";") {
                    ++index_;
                    break;
                }
                else if (token_at(index_).text == "{" ||
                         member->kind == declaration_kind::function) {
                    diagnose_missing_equal();
                    complete = false;
                    break;
                }
                else break;
                if (!complete) break;
            }
            if (!complete) {
                skip_member(before);
                continue;
            }
            if (!at_eof() && token_at(index_).text == ";") ++index_;
            member->span.end = index_ > before ? token_at(index_ - 1).span.end : member->span.begin;
            diagnose_anonymous_object(*member, name);
            diagnose_uninitialized_deduced(*member);
            diagnose_lazy(*member);
            if (index_ == before) {
                add_diagnostic("member-progress-stalled", token_at(before).span,
                               "member parsing made no progress");
                return false;
            }
            output.members.push_back(std::move(member));
        }
        if (at_eof() || token_at(index_).text != "}") {
            add_diagnostic("unterminated-block", token_at(open).span, "expected '}'");
            return false;
        }
        ++index_;
        return true;
    }

    auto consume_block_body(declaration& output, std::size_t open) -> bool {
        const auto close = matching_token(open);
        if (!close) {
            add_diagnostic("unterminated-block", token_at(open).span, "expected '}'");
            return false;
        }
        output.block_body = std::make_unique<function_body>();
        output.span.end = token_at(*close).span.end;
        index_ = open + 1;
        const auto reported = context_.diagnostic_count();
        parser_detail::statement_parser{context_, tokens_}.parse_block(
            output.block_body->statements, index_, token_at(*close), 0);
        if (index_ != *close && context_.diagnostic_count() == reported) {
            add_diagnostic("statement-recovery-failed", token_at(open).span,
                           "statement parser did not reach the closing brace");
        }
        if (index_ != *close) return false;
        index_ = *close + 1;
        return true;
    }

    // `name:` or `name :=` begins a declaration, so ends a preceding
    // initializer that is missing its `;`.
    auto diagnose_declaration_start(std::size_t position) -> bool {
        if (!is_declaration_name(token_at(position)) ||
            (token_at(position + 1).text != ":" && token_at(position + 1).text != ":=")) {
            return false;
        }
        add_diagnostic("missing-semicolon", token_at(position - 1).span,
                       "expected ';' at the end of the declaration");
        return true;
    }

    // Reports an object initialized with a block after the `=` at the cursor;
    // the declaration is then skipped.
    auto block_initializer(const declaration& output, std::size_t name) -> bool {
        return output.kind == declaration_kind::object && token_at(index_).text == "=" &&
            context_.diagnose_block_initializer(index_ + 1, token_at(name).span);
    }

    auto diagnose_anonymous_object(const declaration& output, std::size_t name) -> void {
        if (output.kind == declaration_kind::object && output.alias.empty() &&
            (output.body || output.block_body)) {
            context_.check_anonymous_object(output.name, output.type, token_at(name).span);
        }
    }

    auto diagnose_uninitialized_deduced(const declaration& output) -> void {
        if (output.kind == declaration_kind::object && output.type.kind == type_kind::deduced &&
            !output.body && !output.block_body && output.alias.empty()) {
            add_diagnostic("deduced-object-initializer", output.span,
                           "an object with a deduced type must have an = initializer");
        }
    }

    auto consume_expression_body(declaration& output) -> bool {
        if (at_eof()) return false;
        const auto begin = index_;
        index_ = parser_detail::find_top_level(tokens_, begin, context_.token_count() - 1,
            parser_detail::angle_rule::expression, [&](std::size_t position) {
                return token_at(position).text == ";" ||
                    (position != begin && diagnose_declaration_start(position));
            });
        if (const auto& stray = token_at(index_); stray.text == ")" || stray.text == "]") {
            add_diagnostic(stray.text == ")" ? "unmatched-close-parenthesis"
                                             : "unmatched-close-bracket",
                           stray.span, stray.text == ")" ? "')' has no matching '('"
                                                         : "']' has no matching '['");
        }
        if (index_ == begin) return false;
        context_.diagnose_top_level_comma(begin, index_);
        if (const auto unsupported = context_.unsupported_expression(begin, index_)) {
            add_diagnostic("unsupported-function-expression", *unsupported,
                           "unnamed function expressions are not supported");
            if (!at_eof() && token_at(index_).text == ";") ++index_;
            return true;
        }
        expect_semicolon();
        output.body = std::make_unique<expression_statement>();
        output.body->span.begin = token_at(begin).span.begin;
        output.body->span.end = token_at(index_ - 1).span.end;
        output.body->text = context_.expression(syntax_text(begin, index_), output.body->span.begin);
        context_.diagnose_juxtaposition(output.body->text, begin, index_,
                                        output.kind == declaration_kind::function, true);
        output.body_span = output.body->span;
        if (!at_eof() && token_at(index_).text == ";") ++index_;
        return true;
    }

    // A declaration that does not end in a block ends in `;`. A missing one
    // is reported after the last token, which a declaration start or `}` follows.
    auto expect_semicolon() -> void {
        const auto& found = token_at(index_);
        if (found.text == ";" || found.text == ")" || found.text == "]") return;
        if (found.text == "}" || found.kind == token_kind::end_of_file) {
            add_diagnostic("missing-semicolon", token_at(index_ - 1).span,
                           "expected ';' at the end of the declaration");
        }
    }

    // Named return values become the first locals of the body, so the
    // initialization analysis checks them like any deferred local.
    auto consume_body(declaration& output) -> bool {
        if (!consume_definition(output)) return false;
        if (output.kind == declaration_kind::function && output.block_body &&
            output.named_returns.empty()) {
            const auto returns_value = output.deduction != return_deduction::none ||
                (!output.type.empty() && output.type.text != "void");
            check_returns(output.block_body->statements, returns_value);
        }
        if (output.named_returns.empty()) return true;
        if (!output.block_body) {
            add_diagnostic("named-return-body", output.span,
                           "a function with named return values requires a '{ }' body");
            return false;
        }
        auto locals = statement_list{};
        for (const auto& named : output.named_returns) {
            auto local = std::make_unique<statement>(statement_kind::declaration_statement);
            local->span = named.value.name_span;
            auto data = statement::declaration_data{};
            data.name = named.value.name;
            data.type = named.value.type;
            data.initializer = named.initializer;
            data.is_named_return = true;
            local->as<statement::declaration_form>() = {
                context_.expression(named.initializer.empty() ? data.name + " ;"
                                      : data.name + " = " + named.initializer.source() + ";"),
                std::move(data)};
            locals.push_back(std::move(local));
        }
        auto& statements = output.block_body->statements;
        reject_valued_returns(statements);
        statements.insert(statements.begin(), std::make_move_iterator(locals.begin()),
                          std::make_move_iterator(locals.end()));
        return true;
    }

    auto reject_valued_returns(const statement_list& statements) -> void {
        for_each_return(statements, [&](const statement& current, bool valued) {
            if (valued) {
                add_diagnostic("named-return-value", current.span,
                               "a function with named return values uses a bare 'return;'");
            }
        });
    }

    // A function with a single anonymous return value returns it from every
    // `return`; any other function returns no value.
    auto check_returns(const statement_list& statements, bool returns_value) -> void {
        for_each_return(statements, [&](const statement& current, bool valued) {
            if (valued == returns_value) return;
            add_diagnostic("return-value-mismatch", current.span, returns_value
                ? "return statement must have an expression in a function with a single "
                  "anonymous return value"
                : "return statement with expression must be in a function with a single "
                  "anonymous return value");
        });
    }

    template <typename Visit>
    auto for_each_return(const statement_list& statements, Visit&& visit) -> void {
        for (const auto& current : statements) {
            if (current->kind == statement_kind::return_statement) {
                visit(*current, !current->as<statement::expression_form>().text.empty());
            }
            for_each_nested_list(*current, [&](const auto& nested) {
                for_each_return(nested, visit);
            });
        }
    }

    auto consume_definition(declaration& output) -> bool {
        if (at_eof()) return false;
        if (token_at(index_).text == "==") {
            ++index_;
            if (output.kind == declaration_kind::function) {
                output.is_constexpr = true;
                if (!at_eof() && token_at(index_).text == "{") {
                    return consume_block_body(output, index_);
                }
                if (consume_expression_body(output)) return true;
            }
            const auto begin = index_;
            while (!at_eof() && token_at(index_).text != ";") ++index_;
            if (index_ == begin) {
                add_diagnostic("missing-alias-value", token_at(index_).span,
                               "expected a type or namespace after '=='");
                return false;
            }
            output.alias = syntax_text(begin, index_);
            if (output.kind == declaration_kind::type) context_.check_alias_type(begin, index_);
            if (!at_eof() && token_at(index_).text == ";") ++index_;
            return true;
        }
        if (token_at(index_).text != "=") return false;
        ++index_;
        if (!at_eof() && token_at(index_).text == ";") {
            if (output.kind == declaration_kind::object) {
                add_diagnostic("missing-initializer", token_at(index_ - 1).span,
                               "expected an initializer after '='");
                return false;
            }
            ++index_;
            return true;
        }
        if (!at_eof() && token_at(index_).text == "{") {
            const auto open = index_;
            if (output.kind == declaration_kind::type ||
                output.kind == declaration_kind::namespace_node) {
                return parse_members(output, open);
            }
            if (output.kind == declaration_kind::concept_declaration) {
                add_diagnostic("concept-requires-expression", token_at(index_).span,
                               "a concept definition requires an expression after '='");
                return false;
            }
            // An object's braced list is an expression, reported as Cpp1.
            if (output.kind != declaration_kind::object) return consume_block_body(output, open);
        }
        if (skip_misplaced_statement()) return true;
        if (consume_expression_body(output)) return true;
        add_diagnostic("expected-function-body", token_at(index_).span,
                       "expected function body after '='");
        return false;
    }

    auto consume_declaration(ast& tree) -> bool {
        const auto first = index_;
        auto output = std::make_unique<declaration>();
        output->span.begin = token_at(first).span.begin;

        const auto using_result = consume_using(*output, index_);
        if (using_result == parser_detail::using_parse_result::valid) {
            output->span.end = token_at(index_ - 1).span.end;
            tree.declarations.push_back(std::move(output));
            return true;
        }
        if (using_result == parser_detail::using_parse_result::invalid) {
            skip_declaration();
            return true;
        }

        if (skip_unsupported_operator()) return true;
        if (skip_misplaced_statement()) return true;
        const auto name = index_;
        if (!consume_name(*output)) return false;
        split_colon_equals();
        if (at_eof() || token_at(index_).text != ":") {
            add_diagnostic("missing-declaration-separator", output->span,
                           "expected ':' after declaration name");
            skip_declaration();
            return true;
        }
        ++index_;
        if (!at_eof() && token_at(index_).text == "<" &&
            !consume_template_parameters(*output)) {
            skip_declaration();
            return true;
        }
        consume_final(*output);
        consume_lazy(*output);
        if (!at_eof() && is_keyword(token_at(index_), "type") &&
            !parser_detail::is_type_marker(tokens_, index_)) {
            output->kind = declaration_kind::type;
            ++index_;
        }
        else if (!at_eof() && is_keyword(token_at(index_), "concept")) {
            output->kind = declaration_kind::concept_declaration;
            ++index_;
        }
        else if (!at_eof() && is_keyword(token_at(index_), "namespace")) {
            output->kind = declaration_kind::namespace_node;
            ++index_;
        }
        else {
            consume_type(*output);
            context_.check_cpp1_auto(output->type, output->name, ":= value");
        }

        auto complete = true;
        while (!at_eof() && (token_at(index_).kind == token_kind::punctuation ||
                             is_keyword(token_at(index_), "requires") ||
                             token_at(index_).text == "throws" || is_contract_start(index_) ||
                             output->kind == declaration_kind::function)) {
            if (token_at(index_).text == "throws") ++index_;
            else if (is_contract_start(index_)) complete = consume_contract(*output);
            else if (token_at(index_).text == "@") complete = consume_metaclass(*output);
            else if (token_at(index_).text == "(" &&
                     output->kind != declaration_kind::function) {
                complete = consume_parameters(*output);
            }
            else if (token_at(index_).text == "->") complete = consume_return_type(*output);
            else if (is_keyword(token_at(index_), "requires")) complete = consume_requires(*output);
            else if (token_at(index_).text == "=" || token_at(index_).text == "==") {
                complete = !block_initializer(*output, name) && consume_body(*output);
                break;
            }
            else if (token_at(index_).text == ";") {
                ++index_;
                break;
            }
            else if (token_at(index_).text == "{" ||
                     output->kind == declaration_kind::function) {
                diagnose_missing_equal();
                complete = false;
                break;
            }
            else break;
            if (!complete) break;
        }
        output->span.end = index_ > first ? token_at(index_ - 1).span.end : output->span.begin;
        if (!complete) {
            skip_member(first);
            return true;
        }
        diagnose_anonymous_object(*output, name);
        diagnose_uninitialized_deduced(*output);
        diagnose_lazy(*output);
        tree.declarations.push_back(std::move(output));
        return true;
    }

    parser_detail::token_cursor context_;
    token_list& tokens_;
    std::size_t index_ = 0;
};

}

auto parse_cpp2(const source_text& source, const token_list& tokens,
                const parse_options& options) -> parse_result {
    auto result = parse_result{};
    // A trailing comma before the `>` closing a template list is ignored; Cpp2
    // has no comma operator, so a `,` before `>` ends a list.
    auto adjusted = options;
    auto parser_tokens = token_list{};
    parser_tokens.reserve(tokens.size() + 1);
    for (auto index = std::size_t{0}; index < tokens.size(); ++index) {
        if (tokens[index].text == "," && index + 1 < tokens.size() &&
            detail::angle_closes(tokens[index + 1].text) != 0) {
            adjusted.replacements.push_back({tokens[index].span, {}});
            continue;
        }
        parser_tokens.push_back(tokens[index]);
    }
    std::stable_sort(adjusted.replacements.begin(), adjusted.replacements.end(),
                     [](const auto& left, const auto& right) {
                         return std::pair{left.span.begin.line, left.span.begin.column} <
                             std::pair{right.span.begin.line, right.span.begin.column};
                     });
    auto eof = token{};
    eof.kind = token_kind::end_of_file;
    eof.span.begin = source.data().empty()
        ? source_position{}
        : source_position{static_cast<std::uint32_t>(source.lines().size()) + 1, 1};
    eof.span.end = eof.span.begin;
    parser_tokens.push_back(eof);
    if (auto too_deep = validate_nesting_depth(parser_tokens)) {
        result.diagnostics.push_back(*std::move(too_deep));
        return result;
    }

    const auto parser_trace = parser_detail::trace{"enter", options.trace};
    if (parser_trace.enabled()) {
        for (const auto& value : parser_tokens) {
            parser_trace.event(std::to_string(static_cast<int>(value.kind)) + ' ' + value.text + '@' +
                std::to_string(value.span.begin.line) + ':' + std::to_string(value.span.begin.column));
        }
    }

    auto parser = declaration_parser{source, parser_tokens, result.diagnostics, adjusted};
    result.tree = parser.parse();
    result.binding_operators = parser.binding_operators();
    return result;
}

}
