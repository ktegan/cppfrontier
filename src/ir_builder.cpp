#include <cppfrontier/pipeline.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <utility>

#include "ir_walk.hpp"
#include "regex_metafunction.hpp"

namespace cppfrontier {

namespace {

auto has_valid_main_parameters(const std::vector<parameter>& parameters) -> bool {
    if (parameters.empty()) return true;
    if (parameters.size() != 1) return false;
    return is_main_args_parameter(parameters.front());
}

// common.md "main": a `void` `main` has no `return` statement. Another return
// type, like a template `main`, is left to the Cpp1 compiler.
auto check_main(const ir_declaration& main, std::vector<diagnostic>& diagnostics) -> void {
    const auto form = return_form_of(main);
    const auto is_void = form == return_form::implicit_void ||
        form == return_form::implicit_deduced ||
        (form == return_form::declared && main.type.is_void());
    if (!is_void || !main.function_statements) return;
    ir_detail::visit_statements(main.function_statements->statements, [&](const statement& value) {
        if (value.kind == statement_kind::return_statement) {
            diagnostics.push_back({"void-main-return", diagnostic_severity::error, value.span,
                                   "a 'void' 'main' cannot have a 'return' statement - "
                                   "declare 'main: () -> int' to return a value"});
        }
    });
}

auto build_statement(const statement& input, ir_program& program, symbol_id parent)
    -> std::unique_ptr<statement>;

auto build_statements(const std::vector<std::unique_ptr<statement>>& input, ir_program& program,
                      symbol_id parent) -> std::vector<std::unique_ptr<statement>> {
    auto output = std::vector<std::unique_ptr<statement>>{};
    output.reserve(input.size());
    for (const auto& value : input) {
        output.push_back(build_statement(*value, program, parent));
    }
    return output;
}

auto build_statement(const statement& input, ir_program& program, symbol_id parent)
    -> std::unique_ptr<statement> {
    auto output = std::make_unique<statement>(input.kind);
    output->span = input.span;
    auto scope = parent;
    if (input.kind == statement_kind::block || input.kind == statement_kind::try_statement ||
        input.kind == statement_kind::defer_statement ||
        input.kind == statement_kind::unsafe_block) {
        output->symbol = add_symbol(program, {
            invalid_symbol, {}, declaration_kind::object, parent, 0, {}, input.span});
        scope = output->symbol;
    }
    const auto add_bindings = [&](std::vector<statement::binding>& bindings) {
        for (auto& binding : bindings) {
            if (binding.name == "_") continue;
            binding.symbol = add_symbol(program, {
                invalid_symbol, binding.name, declaration_kind::object, scope, 0,
                parse_type("auto"), binding.span});
        }
    };
    const auto add_parameter = [&](const parameter& value) {
        if (value.name.empty() || value.name == "_") return;
        static_cast<void>(add_symbol(program, {
            invalid_symbol, value.name, declaration_kind::object, scope, 0, value.type,
            value.name_span}));
    };
    std::visit([&](const auto& form) {
        using form_type = std::remove_cvref_t<decltype(form)>;
        auto& copy = output->as<form_type>();
        if constexpr (std::is_same_v<form_type, statement::block_form>) {
            copy.parameters = form.parameters;
            for (const auto& parameter : form.parameters) add_parameter(parameter.value);
            copy.body = build_statements(form.body, program, scope);
            copy.handlers.reserve(form.handlers.size());
            for (const auto& handler : form.handlers) {
                add_parameter(handler.caught);
                copy.handlers.push_back(statement::handler{
                    handler.caught, build_statements(handler.body, program, scope)});
            }
        }
        else if constexpr (std::is_same_v<form_type, statement::if_form>) {
            copy.condition = form.condition;
            copy.bindings = form.bindings;
            add_bindings(copy.bindings);
            copy.body = build_statements(form.body, program, scope);
            copy.else_body = build_statements(form.else_body, program, scope);
            copy.has_else = form.has_else;
            copy.is_else_if = form.is_else_if;
            copy.is_constexpr = form.is_constexpr;
        }
        else if constexpr (std::is_same_v<form_type, statement::loop_form>) {
            copy.label = form.label;
            copy.condition = form.condition;
            copy.next = form.next;
            copy.range_parameter = form.range_parameter;
            if (form.range_parameter) add_parameter(*form.range_parameter);
            copy.bindings = form.bindings;
            add_bindings(copy.bindings);
            copy.body = build_statements(form.body, program, scope);
        }
        else if constexpr (std::is_same_v<form_type, statement::declaration_form>) {
            copy.text = form.text;
            copy.declaration = form.declaration;
            copy.else_body = build_statements(form.else_body, program, scope);
            copy.is_let_else = form.is_let_else;
        }
        else if constexpr (std::is_same_v<form_type, statement::destructuring_form>) {
            copy.bindings = form.bindings;
            copy.initializer = form.initializer;
            copy.else_body = build_statements(form.else_body, program, scope);
            copy.is_let_else = form.is_let_else;
        }
        else {
            copy = form;
        }
    }, input.form);
    if (output->is<statement::declaration_form>()) {
        if (auto& declaration = output->as<statement::declaration_form>().declaration) {
            declaration->symbol = add_symbol(program, {
                invalid_symbol, declaration->name, declaration->kind, scope, 0,
                declaration->type, input.span});
            output->symbol = declaration->symbol;
        }
    }
    else if (auto* form = std::get_if<statement::destructuring_form>(&output->form)) {
        add_bindings(form->bindings);
    }
    return output;
}

// Whether the type alias `alias` names a type that depends on `names`.
auto is_dependent_alias(const std::string& alias, const std::vector<std::string>& names) -> bool {
    if (alias.empty()) return false;
    const auto type = parse_type(alias);
    return mentions_decltype(type) || std::any_of(names.begin(), names.end(),
        [&](const std::string& name) { return mentions_name(type, name); });
}

// Marks the dependent names in the types of local declarations and statement,
// range, and catch parameters; see `mark_dependent_names`. A local alias of a
// dependent type is itself a dependent scope.
auto mark_statement_types(statement_list& statements, std::vector<std::string> names) -> void {
    for (auto& value : statements) {
        if (auto* form = std::get_if<statement::declaration_form>(&value->form);
            form != nullptr && form->declaration) {
            const auto& data = *form->declaration;
            mark_dependent_names(form->declaration->type, names);
            if (data.kind == declaration_kind::type && is_dependent_alias(data.alias, names)) {
                names.push_back(data.name);
            }
        }
        else if (auto* block = std::get_if<statement::block_form>(&value->form)) {
            for (auto& parameter : block->parameters) {
                mark_dependent_names(parameter.value.type, names);
            }
            for (auto& handler : block->handlers) mark_dependent_names(handler.caught.type, names);
        }
        else if (auto* loop = std::get_if<statement::loop_form>(&value->form);
                 loop != nullptr && loop->range_parameter) {
            mark_dependent_names(loop->range_parameter->type, names);
        }
        for_each_nested_list(*value, [&](statement_list& nested) {
            mark_statement_types(nested, names);
        });
    }
}

// The template parameters of `declaration`.
// Replaces each `decltype(name)` in `type` with `declared`.
auto replace_decltype(type_id& type, std::string_view name, const type_id& declared) -> void {
    if (type.kind == type_kind::decltype_of) {
        const auto parsed = parse_expression_text(type.text);
        const auto& tokens = parsed.tokens;
        if (tokens.size() == 4 && tokens[1].text == "(" && tokens[2].text == name &&
            tokens[3].text == ")") {
            type = declared;
        }
        return;
    }
    for (auto& child : type.children) replace_decltype(child, name, declared);
    for (auto& component : type.components) {
        for (auto& argument : component.arguments) replace_decltype(argument, name, declared);
    }
}

auto template_parameter_names(const declaration_signature& declaration)
    -> std::vector<std::string> {
    auto result = std::vector<std::string>{};
    for (const auto& parameter : declaration.template_parameters) {
        if (!parameter.name.empty()) result.push_back(parameter.name);
    }
    return result;
}

}

auto add_symbol(ir_program& program, symbol value) -> symbol_id {
    value.id = program.symbols.empty() ? 1 : program.symbols.back().id + 1;
    value.declaration_order = static_cast<std::uint32_t>(program.symbols.size());
    program.symbols.push_back(std::move(value));
    return program.symbols.back().id;
}

auto build_ir(const ast& tree, ir_program program) -> build_result {
    auto result = build_result{std::move(program), {}};
    // The template parameters of the declarations being built.
    auto template_names = std::vector<std::string>{};
    // The template parameters of each written declaration. A function
    // expression is built at file scope, but its types may name those of the
    // declarations it is written in.
    auto enclosing_templates = std::vector<std::pair<source_span, std::vector<std::string>>>{};
    const auto collect_templates = [&](const auto& self, const declaration& input) -> void {
        if (auto names = template_parameter_names(input); !names.empty()) {
            enclosing_templates.emplace_back(input.span, std::move(names));
        }
        for (const auto& member : input.members) self(self, *member);
    };
    for (const auto& input : tree.declarations) collect_templates(collect_templates, *input);
    const auto key = [](source_position position) { return std::pair{position.line, position.column}; };

    auto build_declaration = [&](const auto& self, const declaration& input, symbol_id parent,
                                 declaration_scope fallback_scope) -> ir_declaration {
        auto output = ir_declaration{};
        static_cast<declaration_signature&>(output) =
            static_cast<const declaration_signature&>(input);
        if (input.scope == declaration_scope::file && fallback_scope != declaration_scope::file) {
            output.scope = fallback_scope;
        }
        output.is_type_member = fallback_scope == declaration_scope::type_scope;
        output.span = input.span;
        output.alias = input.alias;
        output.using_name = input.using_name;
        output.special_member = ir_declaration::special_member_kind::none;
        const auto enclosing_names = template_names.size();
        if (is_function_expression_name(input.name)) {
            for (const auto& [span, names] : enclosing_templates) {
                if (key(span.begin) <= key(input.span.begin) &&
                    key(input.span.end) <= key(span.end) &&
                    std::pair{key(span.begin), key(span.end)} !=
                        std::pair{key(input.span.begin), key(input.span.end)}) {
                    template_names.insert(template_names.end(), names.begin(), names.end());
                }
            }
        }
        for (auto& name : template_parameter_names(output)) {
            template_names.push_back(std::move(name));
        }
        // A base is not a dependent name: Cpp1 base lists do not take `typename`.
        if (output.name != "this") mark_dependent_names(output.type, template_names);
        for (auto& parameter : output.parameters) {
            mark_dependent_names(parameter.type, template_names);
        }
        for (auto& named : output.named_returns) {
            mark_dependent_names(named.value.type, template_names);
        }
        // `decltype(x)` of an `out` parameter in the signature is its declared type.
        for (std::size_t index = 0; index < output.parameters.size(); ++index) {
            const auto out = output.parameters[index];
            if (out.passing != passing_style::out || out.name == "this" ||
                out.type.is_wildcard()) {
                continue;
            }
            for (auto& parameter : output.parameters) {
                replace_decltype(parameter.type, out.name, out.type);
            }
            for (auto& named : output.named_returns) {
                replace_decltype(named.value.type, out.name, out.type);
            }
            replace_decltype(output.type, out.name, out.type);
        }
        const auto& parameters = output.template_parameters;
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            const auto& parameter = parameters[index];
            if (!parameter.name.empty() &&
                std::any_of(parameters.begin(), parameters.begin() + static_cast<std::ptrdiff_t>(index),
                    [&](const template_parameter& other) { return other.name == parameter.name; })) {
                result.diagnostics.push_back({
                    "duplicate-template-parameter", diagnostic_severity::error, output.span,
                    "template parameter '" + parameter.name + "' is declared twice"});
            }
            // Cpp1 allows a pack before other parameters only in a function template.
            if (parameter.is_variadic && index + 1 < parameters.size() &&
                output.kind != declaration_kind::function) {
                result.diagnostics.push_back({
                    "pack-not-last", diagnostic_severity::error, output.span,
                    "template parameter pack '" + parameter.name + "' must be the last "
                    "template parameter"});
            }
        }
        if (output.kind == declaration_kind::concept_declaration) {
            if (output.scope != declaration_scope::file &&
                output.scope != declaration_scope::namespace_scope) {
                result.diagnostics.push_back({
                    "concept-declaration-scope", diagnostic_severity::error, output.span,
                    "a concept can only be declared at namespace scope"});
            }
            if (output.template_parameters.empty()) {
                result.diagnostics.push_back({
                    "concept-requires-template-parameters", diagnostic_severity::error,
                    output.span, "a concept requires a non-empty template parameter list"});
            }
            if (input.body == nullptr) {
                result.diagnostics.push_back({
                    "concept-requires-expression", diagnostic_severity::error, output.span,
                    "a concept definition requires an expression after '='"});
            }
        }
        // A deduced type cannot be declared before its definition, so it would
        // keep the object from being used before it, as upstream (`==` may deduce).
        if (output.kind == declaration_kind::object && output.alias.empty() &&
            output.type.core().is_wildcard() &&
            (output.scope == declaration_scope::file ||
             output.scope == declaration_scope::namespace_scope)) {
            result.diagnostics.push_back({
                "namespace-scope-object-requires-type", diagnostic_severity::error,
                output.span, "a namespace-scope object must have a declared type, not a "
                "deduced type (or be a '==' constant)"});
        }
        // `x: requires C == value` names neither a type nor `type`/`namespace`.
        if (output.kind == declaration_kind::object && !output.alias.empty() &&
            output.type.empty() && !output.requires_clause.empty()) {
            result.diagnostics.push_back({
                "invalid-alias", diagnostic_severity::error, output.span,
                "invalid alias declaration - expected 'type', 'namespace', or a type-id after ':'"});
        }
        output.is_entry_point = input.name == "main" && parent == invalid_symbol;
        if (output.is_entry_point && !has_valid_main_parameters(input.parameters)) {
            result.diagnostics.push_back({
                "invalid-main-parameters", diagnostic_severity::error, input.span,
                "'main' must be declared as 'main: ()' or 'main: (args)' with one deduced "
                "'in' parameter named 'args'"});
        }
        output.symbol = add_symbol(result.program, {
            invalid_symbol, input.name, input.kind, parent, 0, input.type, input.span});
        // An object named `_` gets a name unique by its position, which generated
        // memberwise code can name.
        if (output.kind == declaration_kind::object && output.name == "_") {
            output.name = anonymous_object_name(output.span.begin);
        }
        for (const auto& input_parameter : input.parameters) {
            static_cast<void>(add_symbol(result.program, {
                invalid_symbol, input_parameter.name, declaration_kind::object, output.symbol, 0,
                input_parameter.type, input_parameter.name_span}));
        }
        if (input.body) output.body = input.body->text;
        if (input.block_body) {
            auto body = std::make_unique<function_body>();
            body->statements = build_statements(input.block_body->statements, result.program, output.symbol);
            mark_statement_types(body->statements, template_names);
            output.function_statements = std::move(body);
        }
        if (output.is_entry_point) check_main(output, result.diagnostics);
        // A member alias of a dependent type is a dependent scope.
        for (const auto& member : input.members) {
            if (member->kind == declaration_kind::type &&
                is_dependent_alias(member->alias, template_names)) {
                template_names.push_back(member->name);
            }
        }
        output.members.reserve(input.members.size());
        const auto member_scope = output.kind == declaration_kind::namespace_node
            ? declaration_scope::namespace_scope
            : declaration_scope::type_scope;
        // Enumerators and `@regex` members are the only type-scope objects whose
        // type is deduced.
        const auto is_enum = std::any_of(output.metaclasses.begin(), output.metaclasses.end(),
            [](const std::string& name) { return name == "enum" || name == "flag_enum"; });
        const auto is_regex = std::find(output.metaclasses.begin(), output.metaclasses.end(),
                                        "regex") != output.metaclasses.end();
        for (const auto& member : input.members) {
            auto& built = output.members.emplace_back(
                self(self, *member, output.symbol, member_scope));
            if (built.name == "this") {
                const auto error = [&](std::string code, std::string message) {
                    result.diagnostics.push_back({std::move(code), diagnostic_severity::error,
                                                  built.span, std::move(message)});
                };
                if (output.kind != declaration_kind::type ||
                    built.kind != declaration_kind::object || !built.alias.empty()) {
                    error("invalid-base", "a member named 'this' declares a base subobject, "
                          "and must be followed by a base type name");
                }
                else if (std::any_of(output.members.begin(), output.members.end() - 1,
                             [](const ir_declaration& previous) {
                                 return previous.kind == declaration_kind::function;
                             })) {
                    error("base-after-function",
                          "a type cannot declare a parent after defining a function");
                }
                else if (built.access == access_kind::private_access ||
                         built.access == access_kind::protected_access) {
                    error("non-public-base", "a base type must be public (the default)");
                }
                else if (built.type.is_wildcard()) {
                    error("deduced-base", "a base type must be a specific type, not a deduced "
                          "type (omitted or '_'-wildcarded)");
                }
                else if (std::any_of(input.members.begin(), input.members.end(),
                             [&](const auto& other) {
                                 return other->kind == declaration_kind::type &&
                                     mentions_name(built.type, other->name);
                             })) {
                    error("base-names-member-type", "a base type cannot name a member type of "
                          "the type it is declared in, because a Cpp1 base list cannot see it");
                }
                for (auto& previous : output.members) {
                    if (&previous == &built || previous.kind != declaration_kind::object ||
                        !previous.alias.empty() || previous.is_lazy || previous.name == "this" ||
                        !previous.base_helper.empty()) {
                        continue;
                    }
                    if (!output.template_parameters.empty()) {
                        error("data-member-before-base", "a data member declared before a base "
                              "type is not supported in a template");
                        break;
                    }
                    previous.base_helper = output.name + "_" + previous.name + "_as_base";
                }
                continue;
            }
            if (member_scope == declaration_scope::type_scope && !is_enum &&
                !(is_regex && is_regex_member_name(built.name)) &&
                built.kind == declaration_kind::object && built.alias.empty() &&
                built.type.core().is_wildcard()) {
                result.diagnostics.push_back({
                    "type-scope-object-requires-type", diagnostic_severity::error, built.span,
                    "a type-scope object must have a declared type"});
            }
        }
        template_names.resize(enclosing_names);
        return output;
    };

    result.program.declarations.reserve(tree.declarations.size());
    for (const auto& input : tree.declarations) {
        auto declaration = build_declaration(build_declaration, *input, invalid_symbol, declaration_scope::file);
        if (declaration.scope == declaration_scope::file) {
            declaration.scope = declaration_scope::namespace_scope;
        }
        result.program.declarations.push_back(std::move(declaration));
    }
    return result;
}

}
