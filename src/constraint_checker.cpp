#include <cppfrontier/pipeline.hpp>

#include <string>
#include <string_view>
#include <unordered_map>

#include "ir_walk.hpp"
#include "scope_model.hpp"

namespace cppfrontier {
namespace {

// The name of `type` without template arguments, such as `n::C` for `n::C<int>`.
auto unqualified_spelling(const type_id& type) -> std::string {
    auto result = std::string{type.is_global ? "::" : ""};
    for (const auto& component : type.components) {
        if (&component != &type.components.front()) result += "::";
        result += component.identifier;
    }
    return result;
}

class constraint_checker final {
public:
    constraint_checker(const ir_program& program, const scope_model& scopes) : scopes_{scopes} {
        ir_detail::visit_declarations(program.declarations, [&](const ir_declaration& value) {
            if (value.symbol != invalid_symbol) declarations_.emplace(value.symbol, &value);
        });
    }

    // Checks the constraint of `type`, written at `point`.
    auto check(const type_id& type, scope_point point) -> void {
        if (const auto* constraint = type.constraint()) check_constraint(*constraint, point, "_");
    }

    // Checks `constraint` of `subject`, written at `point`; each operand of
    // `A & B` or `A | B` is a constraint of its own.
    auto check_constraint(const type_id& constraint, scope_point point, std::string_view subject)
        -> void {
        if (constraint.is_operator()) {
            for (const auto& operand : constraint.children) {
                check_constraint(operand, point, subject);
            }
            return;
        }
        if (constraint.span.begin.line == 0) return;
        const auto is_type_keyword = constraint.kind == type_kind::name &&
            !constraint.is_global && constraint.components.size() == 1 &&
            constraint.components.front().identifier == "type";
        if (constraint.kind == type_kind::name && !is_type_keyword) {
            if (point.valid()) check_concept(constraint, point, subject);
            return;
        }
        // Only a name can be a concept: `type`, a fundamental, pointer, `const`,
        // function or `decltype` type, or a deduced one, cannot.
        if (constraint.kind != type_kind::cpp1 && constraint.kind != type_kind::none) {
            report("constraint-not-concept", constraint,
                   "'" + constraint.text + "' is not a concept, so it cannot constrain '" +
                       std::string{subject} + "'");
        }
    }

    [[nodiscard]] auto diagnostics() && -> std::vector<diagnostic> {
        return std::move(diagnostics_);
    }

private:
    auto check_concept(const type_id& constraint, scope_point point, std::string_view subject)
        -> void {
        const auto* found = scopes_.lookup(unqualified_spelling(constraint), point);
        if (found != nullptr) found = scopes_.resolve(*found);
        // Names declared outside this translation unit are left to the Cpp1 compiler.
        if (found == nullptr) return;
        using kind = scope_model::entry_kind;
        switch (found->kind) {
        case kind::concept_declaration: break;
        case kind::type:
        case kind::object:
        case kind::function:
        case kind::namespace_node:
        case kind::parameter:
        case kind::statement_parameter:
            report("constraint-not-concept", constraint,
                   "'" + constraint.text + "' is not a concept, so it cannot constrain '" +
                       std::string{subject} + "'");
            return;
        default: return;
        }
        const auto declaration = declarations_.find(found->symbol);
        if (declaration == declarations_.end()) return;
        check_arguments(constraint, *declaration->second, subject);
    }

    // `_ is C<args...>` checks `C<T, args...>`, so the concept takes one more
    // argument than is written, and its first parameter must take a type.
    auto check_arguments(const type_id& constraint, const ir_declaration& concept_declaration,
                         std::string_view subject) -> void {
        const auto& parameters = concept_declaration.template_parameters;
        if (parameters.empty()) return;
        const auto& first = parameters.front();
        if (!first.is_type_parameter()) {
            report("constraint-not-type-concept", constraint,
                   "'" + concept_declaration.name + "' does not take a type as its first "
                   "template argument, so it cannot constrain '" + std::string{subject} + "'");
            return;
        }
        auto required = std::size_t{0};
        auto variadic = false;
        for (const auto& parameter : parameters) {
            variadic = variadic || parameter.is_variadic;
            if (!parameter.is_variadic && !parameter.has_default()) ++required;
        }
        const auto supplied = constraint.components.back().arguments.size() + 1;
        if (supplied >= required && (variadic || supplied <= parameters.size())) return;
        // Too few arguments, or (only when nothing is variadic) too many.
        const auto too_many = supplied > parameters.size();
        const auto count = (too_many ? parameters.size() : required) - 1;
        const auto expected = (variadic || required == parameters.size() ? ""
                               : too_many ? "at most " : "at least ") +
            std::to_string(count);
        report("constraint-argument-count", constraint,
               "'" + concept_declaration.name + "' as a type constraint takes " + expected +
               (variadic ? " or more" : "") + " template argument" +
               (count == 1 && !variadic ? "" : "s") + " after the constrained type, but " +
               std::to_string(supplied - 1) + " " + (supplied == 2 ? "is" : "are") +
               " written");
    }

    auto report(std::string_view code, const type_id& type, std::string message) -> void {
        diagnostics_.push_back({std::string{code}, diagnostic_severity::error, type.span,
                                std::move(message)});
    }

    const scope_model& scopes_;
    std::unordered_map<symbol_id, const ir_declaration*> declarations_;
    std::vector<diagnostic> diagnostics_;
};

}

auto check_constraints(const ir_program& program, const scope_model& scopes)
    -> std::vector<diagnostic> {
    auto checker = constraint_checker{program, scopes};
    ir_detail::visit_declarations(program.declarations,
        [&](const ir_declaration& declaration) {
            const auto inside = scopes.inside(declaration.symbol);
            checker.check(declaration.type, inside);
            for (const auto& value : declaration.template_parameters) {
                checker.check_constraint(value.constraint, inside, value.name);
            }
            for (const auto& value : declaration.parameters) checker.check(value.type, inside);
            for (const auto& value : declaration.named_returns) {
                checker.check(value.value.type, inside);
            }
        },
        [&](const statement& value) {
            const auto point = scopes.at(value);
            if (const auto* form = std::get_if<statement::declaration_form>(&value.form);
                form != nullptr && form->declaration) {
                checker.check(form->declaration->type, point);
            }
            else if (const auto* block = std::get_if<statement::block_form>(&value.form)) {
                for (const auto& parameter : block->parameters) {
                    checker.check(parameter.value.type, scopes.at(parameter));
                }
                for (const auto& handler : block->handlers) checker.check(handler.caught.type, point);
            }
            else if (const auto* loop = std::get_if<statement::loop_form>(&value.form);
                     loop != nullptr && loop->range_parameter) {
                checker.check(loop->range_parameter->type, point);
            }
        });
    return std::move(checker).diagnostics();
}

}
