// The `try-operator` experiment: where the postfix `?` may appear.

#include <cppfrontier/pipeline.hpp>

#include "cpp1_expression_renderer.hpp"
#include "ir_walk.hpp"

#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace cppfrontier {
namespace {

constexpr auto experiment = std::string_view{"try-operator"};

auto before(source_position left, source_position right) -> bool {
    return std::tie(left.line, left.column) < std::tie(right.line, right.column);
}

// The span of the first `?` at or after `span`, which begins the statement or
// declaration that holds it, or `span` itself. A statement's span is its first token.
// A function expression that begins after `span` holds its own `?`s; its tokens
// follow the program's in `tokens`, so the search goes by position.
auto question_span(const token_list& tokens, source_span span) -> source_span {
    auto nested = std::vector<source_span>{};
    for (const auto& value : tokens) {
        if (value.kind == token_kind::identifier && is_function_expression_name(value.text) &&
            before(span.begin, value.span.begin)) {
            nested.push_back(value.span);
        }
    }
    auto result = std::optional<source_span>{};
    for (const auto& value : tokens) {
        if (value.kind == token_kind::punctuation && value.text == "?" &&
            !before(value.span.begin, span.begin) &&
            (!result || before(value.span.begin, result->begin)) &&
            !ir_detail::inside_any(nested, value.span)) {
            result = value.span;
        }
    }
    return result.value_or(span);
}

auto has_question(const parsed_expression& text) -> bool {
    for (const auto& value : text.tree().tokens) {
        if (value.kind == expression_token_kind::punctuation && value.text == "?") return true;
        if (value.kind == expression_token_kind::string) {
            for (const auto& nested : interpolated_expressions(value.text)) {
                if (has_question(parsed_expression{nested, true})) return true;
            }
        }
    }
    return false;
}

auto has_question(const contract_clause& contract) -> bool {
    return has_question(contract.condition) || has_question(contract.message) ||
        std::any_of(contract.flags.begin(), contract.flags.end(),
                    [](const parsed_expression& flag) { return has_question(flag); });
}

// `std::expected<...>` or `std::optional<...>`, which `try_failure` can build.
auto is_propagable_type(std::string_view text) -> bool {
    if (text.starts_with("std::")) text.remove_prefix(5);
    return text.starts_with("expected<") || text.starts_with("optional<");
}

struct text_uses final {
    bool question = false;
    // A `?` on the right of `&&` or `||`.
    bool short_circuit = false;
    // A `?` in an inspect expression's alternatives.
    bool inspect = false;
    // A `?` in a comprehension's guard or value, which run in a lambda.
    bool comprehension = false;
    // A `?` that follows no operand.
    bool stray = false;
};

auto collect_uses(const expression_text& text, const expression& node, bool conditional,
                  std::set<std::size_t>& operators, text_uses& uses) -> void {
    const auto& tokens = text.tokens;
    if (node.kind == expression_kind::postfix && tokens[node.op].text == "?") {
        operators.insert(node.op);
        uses.question = true;
        if (conditional) uses.short_circuit = true;
    }
    for (auto index = std::size_t{0}; index < node.children.size(); ++index) {
        auto nested = conditional;
        if (node.kind == expression_kind::binary && index > 0) {
            const auto op = binary_operator(tokens, node, index);
            nested = nested || op == "&&" || op == "||";
        }
        if ((node.kind == expression_kind::inspect || node.kind == expression_kind::comprehension) &&
            index > 0) {
            auto deferred = text_uses{};
            collect_uses(text, node.children[index], nested, operators, deferred);
            uses.question = uses.question || deferred.question;
            auto& flag = node.kind == expression_kind::inspect ? uses.inspect : uses.comprehension;
            flag = flag || deferred.question;
            continue;
        }
        collect_uses(text, node.children[index], nested, operators, uses);
    }
}

auto uses_of(const parsed_expression& text) -> text_uses {
    auto uses = text_uses{};
    auto operators = std::set<std::size_t>{};
    collect_uses(text.tree(), text.tree().root, false, operators, uses);
    const auto& tokens = text.tree().tokens;
    for (auto index = std::size_t{0}; index < tokens.size(); ++index) {
        if (tokens[index].kind == expression_token_kind::punctuation && tokens[index].text == "?" &&
            !operators.contains(index)) {
            uses.question = true;
            uses.stray = true;
        }
        if (tokens[index].kind == expression_token_kind::string) {
            for (const auto& nested : interpolated_expressions(tokens[index].text)) {
                const auto inner = uses_of(parsed_expression{nested, true});
                uses.question = uses.question || inner.question;
                uses.short_circuit = uses.short_circuit || inner.short_circuit;
                uses.inspect = uses.inspect || inner.inspect;
                uses.comprehension = uses.comprehension || inner.comprehension;
                uses.stray = uses.stray || inner.stray;
            }
        }
    }
    return uses;
}

class checker final {
public:
    checker(const token_list& tokens, std::vector<diagnostic>& diagnostics)
        : tokens_{tokens}, diagnostics_{diagnostics} {}

    auto check(const ir_declaration& declaration) -> void {
        check_signature(declaration);
        if (declaration.kind != declaration_kind::function) {
            if (has_question(declaration.body)) {
                context_error(declaration.span, "not in an initializer outside a function");
            }
            return;
        }
        check_text(declaration.body, declaration, declaration.span, false);
        const auto* body = declaration.function_statements.get();
        if (body == nullptr || !checked_.insert(body).second) return;
        check_statements(body->statements, declaration, false);
    }

private:
    const token_list& tokens_;
    std::vector<diagnostic>& diagnostics_;
    std::set<const function_body*> checked_;

    auto error(std::string_view code, source_span span, std::string message) -> void {
        diagnostics_.push_back({std::string{code}, diagnostic_severity::error,
                                question_span(tokens_, span), std::move(message)});
    }

    auto context_error(source_span span, std::string_view where) -> void {
        error("try-operator-context", span,
              "`?` is allowed only in a function body statement that can return early; " +
              std::string{where});
    }

    // The parts of a declaration outside its body, which return from no function;
    // the first that holds a `?` is reported.
    auto check_signature(const ir_declaration& declaration) -> void {
        const auto any = [](const auto& values, auto text) {
            return std::any_of(values.begin(), values.end(),
                               [&](const auto& value) { return has_question(text(value)); });
        };
        if (any(declaration.template_parameters,
                [](const template_parameter& value) -> auto& { return value.default_value; }) ||
            any(declaration.parameters,
                [](const parameter& value) -> auto& { return value.default_value; })) {
            context_error(declaration.span, "not in a parameter default");
        }
        else if (has_question(declaration.requires_clause)) {
            context_error(declaration.span, "not in a requires clause");
        }
        else if (any(declaration.contracts,
                     [](const function_contract& value) -> auto& { return value; })) {
            context_error(declaration.span, "not in a contract");
        }
    }

    // A defer body runs when its block exits, where `?` cannot return.
    auto check_statements(const statement_list& statements, const ir_declaration& function,
                          bool in_defer) -> void {
        for (const auto& value : statements) {
            check_statement(*value, function, in_defer);
            for_each_nested_list(*value, [&](const statement_list& nested) {
                check_statements(nested, function,
                                 in_defer || value->kind == statement_kind::defer_statement);
            });
        }
    }

    auto check_statement(const statement& value, const ir_declaration& function, bool in_defer)
        -> void {
        switch (value.kind) {
        case statement_kind::expression:
        case statement_kind::discard_statement:
        case statement_kind::return_statement:
            check_text(value.as<statement::expression_form>().text, function, value.span,
                       in_defer);
            return;
        case statement_kind::declaration_statement: {
            const auto& form = value.as<statement::declaration_form>();
            if (form.declaration && form.declaration->is_named_return) {
                if (has_question(form.text)) {
                    context_error(value.span, "not in a named return value");
                }
            }
            else if (form.declaration && form.declaration->is_static) {
                if (has_question(form.text)) {
                    context_error(value.span, "not in a static local's initializer");
                }
            }
            else if (!form.declaration || form.declaration->kind == declaration_kind::object) {
                check_text(form.text, function, value.span, in_defer);
            }
            else if (has_question(form.text)) {
                context_error(value.span, "not in a local declaration");
            }
            return;
        }
        case statement_kind::if_statement:
            if (has_question(value.as<statement::if_form>().condition)) {
                context_error(value.span, "not in a condition");
            }
            return;
        case statement_kind::while_statement:
        case statement_kind::do_statement:
        case statement_kind::for_statement: {
            const auto& form = value.as<statement::loop_form>();
            if (has_question(form.condition)) {
                context_error(value.span, "not in a loop condition or range");
            }
            else if (has_question(form.next)) {
                context_error(value.span, "not in a loop's next clause");
            }
            return;
        }
        case statement_kind::contract_statement:
            if (has_question(value.as<statement::contract_form>())) {
                context_error(value.span, "not in a contract");
            }
            return;
        case statement_kind::block:
        case statement_kind::try_statement:
            for (const auto& parameter : value.as<statement::block_form>().parameters) {
                if (has_question(parameter.initializer)) {
                    context_error(value.span, "not in a statement parameter");
                }
            }
            return;
        case statement_kind::destructuring_statement:
            check_text(value.as<statement::destructuring_form>().initializer, function, value.span,
                       in_defer);
            return;
        case statement_kind::break_statement:
        case statement_kind::continue_statement:
        case statement_kind::using_declaration:
        case statement_kind::defer_statement:
        case statement_kind::unsafe_block:
            return;
        }
    }

    // A statement text in which `?` returns early from `function`.
    auto check_text(const parsed_expression& text, const ir_declaration& function,
                    source_span span, bool in_defer) -> void {
        const auto uses = uses_of(text);
        if (!uses.question) return;
        if (uses.stray) {
            error("try-operator-context", span, "`?` must follow the expression it tests");
            return;
        }
        if (in_defer) {
            context_error(span, "not in a defer body");
            return;
        }
        if (uses.inspect) {
            context_error(span, "not in an inspect alternative");
            return;
        }
        if (uses.comprehension) {
            context_error(span, "not in a comprehension's guard or value");
            return;
        }
        if (uses.short_circuit) {
            error("try-operator-short-circuit", span,
                  "`?` on the right of `&&` or `||` would be evaluated unconditionally");
            return;
        }
        if (function.type.is_wildcard() || !function.named_returns.empty() ||
            !is_propagable_type(function.type.text)) {
            error("try-operator-return-type", span,
                  "`?` returns early from a function whose declared return type is "
                  "std::expected or std::optional" +
                  (function.type.is_wildcard() && function.named_returns.empty()
                       ? std::string{", not a deduced one"}
                       : ", not `" + (function.named_returns.empty()
                                          ? function.type.text : std::string{"(...)"}) + "`"));
        }
    }
};

}

auto check_try_operator(const ir_program& program, const token_list& tokens)
    -> std::vector<diagnostic> {
    auto diagnostics = std::vector<diagnostic>{};
    auto check = checker{tokens, diagnostics};
    ir_detail::visit_declarations(program.declarations,
                                  [&](const ir_declaration& declaration) { check.check(declaration); });
    return diagnostics;
}

}
