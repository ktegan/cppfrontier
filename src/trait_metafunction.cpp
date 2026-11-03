#include "trait_metafunction.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "cpp1_type_renderer.hpp"
#include "generated_members.hpp"
#include "ir_walk.hpp"
#include "scope_model.hpp"

namespace cppfrontier {

namespace {

auto add_error(std::vector<diagnostic>& diagnostics, const source_span& span, std::string code,
               std::string message) -> void {
    diagnostics.push_back({std::move(code), diagnostic_severity::error, span, std::move(message)});
}

// `::a::b::name` for the symbol `id`.
auto qualified_name(const ir_program& program, symbol_id id) -> std::string {
    auto result = std::string{};
    for (const auto* found = find_symbol(program, id); found != nullptr;
         found = find_symbol(program, found->parent)) {
        result.insert(0, "::" + found->name);
    }
    return result;
}

// The qualified name of the scope declaring the symbol `id`; empty at file scope.
auto scope_name(const ir_program& program, symbol_id id) -> std::string {
    const auto* found = find_symbol(program, id);
    return found == nullptr ? std::string{} : qualified_name(program, found->parent);
}

// One function of a trait.
struct trait_function final {
    struct parameter final {
        passing_style passing = passing_style::in;
        std::string type;
        // As a Cpp1 parameter of a requires-expression.
        std::string cpp1;
    };
    std::string name;
    // `inout this`; `this` otherwise.
    bool is_inout = false;
    // `-> forward T`.
    bool returns_forward = false;
    // Empty for void.
    std::string return_type;
    std::vector<parameter> parameters;
};

auto keyword(passing_style passing) -> std::string {
    switch (passing) {
    case passing_style::in: return {};
    case passing_style::in_ref: return "in_ref ";
    case passing_style::copy: return "copy ";
    case passing_style::inout: return "inout ";
    case passing_style::out: return "out ";
    case passing_style::move: return "move ";
    case passing_style::forward: return "forward ";
    }
    return {};
}

auto argument_name(std::size_t index) -> std::string {
    return std::string{generated_name_prefix} + "arg" + std::to_string(index);
}

// `, inout __cpp2_arg0: T, ...`, or with each parameter unnamed as `_`.
auto parameter_list(const trait_function& function, bool named) -> std::string {
    auto result = std::string{};
    for (auto index = std::size_t{0}; index < function.parameters.size(); ++index) {
        const auto& value = function.parameters[index];
        result += ", " + keyword(value.passing) + (named ? argument_name(index) : "_") + ": " +
            value.type;
    }
    return result;
}

// `__cpp2_arg0, out __cpp2_arg1, ...`, passing each parameter on.
auto argument_list(const trait_function& function) -> std::string {
    auto result = std::string{};
    for (auto index = std::size_t{0}; index < function.parameters.size(); ++index) {
        const auto passing = function.parameters[index].passing;
        result += (index == 0 ? "" : ", ") +
            std::string{passing == passing_style::out ? "out " :
                        passing == passing_style::move ? "move " : ""} +
            argument_name(index);
    }
    return result;
}

// ` -> T`, or nothing for void.
auto return_list(const trait_function& function) -> std::string {
    if (function.return_type.empty()) return {};
    return std::string{" -> "} + (function.returns_forward ? "forward " : "") +
        function.return_type;
}

// `(inout this, k: int) -> T`, with `specifier` before `this`.
auto signature(const trait_function& function, std::string_view specifier, bool named)
    -> std::string {
    return "(" + std::string{specifier} + (function.is_inout ? "inout " : "") + "this" +
        parameter_list(function, named) + ")" + return_list(function);
}

// ` = call;`, or a block discarding the result for a void function.
auto body(const trait_function& function, const std::string& call) -> std::string {
    return function.return_type.empty() ? " = { _ = " + call + "; }\n" : " = " + call + ";\n";
}

// The functions of the trait `type`, or nothing after a diagnostic.
auto trait_functions(const ir_declaration& type, std::vector<diagnostic>& diagnostics)
    -> std::optional<std::vector<trait_function>> {
    const auto error = [&](const source_span& span, std::string code, std::string message) {
        add_error(diagnostics, span, std::move(code), std::move(message));
    };
    auto failed = false;
    auto functions = std::vector<trait_function>{};
    if (!type.template_parameters.empty()) {
        error(type.span, "trait-template", "a trait cannot be a template");
        failed = true;
    }
    for (const auto& member : type.members) {
        if (member.is_generated) continue;
        if (member.kind != declaration_kind::function) {
            error(member.span, "trait-member-kind", "a trait may only declare functions");
            failed = true;
            continue;
        }
        if (!member.body.empty() || member.function_statements) {
            error(member.span, "trait-function-body",
                  "a trait function must not have a body; remove the '=' initializer");
            failed = true;
        }
        if (!member.contracts.empty()) {
            error(member.span, "trait-function-contract", "a trait function cannot have contracts");
            failed = true;
        }
        const auto* self = receiver(member);
        if (self == nullptr ||
            (self->passing != passing_style::in && self->passing != passing_style::inout)) {
            error(member.span, "trait-function-this",
                  "a trait function must take 'this' or 'inout this'");
            failed = true;
            continue;
        }
        if (!member.template_parameters.empty() || member.name == "operator=") {
            error(member.span, "trait-function-form",
                  "a trait function cannot be a template or an operator=");
            failed = true;
            continue;
        }
        auto function = trait_function{};
        function.name = member.name;
        function.is_inout = self->passing == passing_style::inout;
        const auto form = return_form_of(member);
        if ((form == return_form::declared && member.named_returns.empty()) ||
            form == return_form::declared_forwarding) {
            function.returns_forward = form == return_form::declared_forwarding;
            if (!member.type.is_void()) function.return_type = member.type.text;
        }
        else if (form != return_form::implicit_void) {
            error(member.span, "trait-deduced-return",
                  "a trait function must return void, or declare its return type with '-> T' "
                  "or '-> forward T'");
            failed = true;
            continue;
        }
        for (auto index = std::size_t{1}; index < member.parameters.size(); ++index) {
            const auto& parameter = member.parameters[index];
            if (parameter.type.is_wildcard() || parameter.is_variadic ||
                parameter.passing == passing_style::forward) {
                error(parameter.name_span, "trait-generic-parameter",
                      "a trait function parameter must have a declared type and cannot be "
                      "'forward'");
                failed = true;
                continue;
            }
            if (!parameter.default_value.empty()) {
                error(parameter.name_span, "trait-default-argument",
                      "a trait function parameter cannot have a default argument");
                failed = true;
            }
            const auto cpp1 = render_cpp1_type(parameter.type);
            const auto by_reference = parameter.passing == passing_style::inout ||
                parameter.passing == passing_style::out ||
                parameter.type.kind == type_kind::constant;
            function.parameters.push_back({parameter.passing, parameter.type.text,
                parameter.passing == passing_style::move ? cpp1 + "&&"
                : by_reference ? cpp1 + "&" : cpp1 + " const&"});
        }
        functions.push_back(std::move(function));
    }
    if (functions.empty() && !failed) {
        error(type.span, "trait-empty", "a trait must declare at least one function");
        failed = true;
    }
    if (failed) return std::nullopt;
    return functions;
}

// `Trait_calls: <T> type`, defined after every function declaration, so that
// a function given for `T` is visible to it wherever it is declared. Its
// `__cpp2_satisfied()` is whether `T` has each trait function, as a member or
// as a free function UFCS reaches, and each `__cpp2_f` calls the function `f`.
auto render_calls(const std::string& name, const std::vector<trait_function>& functions)
    -> std::string {
    auto requirements = std::string{};
    auto forwarders = std::string{};
    for (const auto& function : functions) {
        auto parameters = std::string{"__cpp2_T"} + (function.is_inout ? "& " : " const& ") +
            "__cpp2_self";
        for (auto index = std::size_t{0}; index < function.parameters.size(); ++index) {
            parameters += ", " + function.parameters[index].cpp1 + " " + argument_name(index);
        }
        const auto call = "__cpp2_self." + function.name + "(" + argument_list(function) + ")";
        // A forwarded result is an lvalue, const unless `this` is `inout`, as a
        // member function returning `forward` returns it.
        const auto forwarded = function.is_inout
            ? function.return_type : "std::add_const_t<" + function.return_type + ">";
        const auto requirement = function.return_type.empty() ? call + ";"
            : function.returns_forward
            ? "{ (" + call + ")& } -> std::convertible_to<std::add_pointer_t<" + forwarded + ">>;"
            : "{ " + call + " } -> std::convertible_to<" + function.return_type + ">;";
        requirements += std::string{requirements.empty() ? "" : "\n        && "} + "requires (" +
            parameters + ") { " + requirement + " }";
        auto forwarder = function;
        if (function.returns_forward) forwarder.return_type = forwarded;
        forwarders += "    __cpp2_" + function.name + ": (" +
            (function.is_inout ? "inout" : "in_ref") + " __cpp2_self: __cpp2_T" +
            parameter_list(function, true) + ")" + return_list(forwarder) + body(function, call);
    }
    return name + "_calls: <__cpp2_T> type = {\n    __cpp2_satisfied: () -> bool == " +
        requirements + ";\n" + forwarders + "}\n";
}

// `Trait_archetype`: a type with exactly the trait's functions, which may be
// instantiated against but never called.
auto render_archetype(const std::string& name, const std::vector<trait_function>& functions)
    -> std::string {
    auto result = name + "_archetype: @cpp1_rule_of_zero type = {\n";
    for (const auto& function : functions) {
        result += "    " + function.name + ": " + signature(function, "", false) +
            " = { std::abort(); }\n";
    }
    return result + "}\n";
}

// `dyn_Trait`: a value holding any copyable type satisfying the trait, through
// the inner `@interface` `__cpp2_interface` and its implementation
// `__cpp2_model<T>`, which holds the value.
auto render_dyn(const std::string& name, const std::vector<trait_function>& functions)
    -> std::string {
    const auto dyn = "dyn_" + name;
    auto interface = std::string{};
    auto model = std::string{};
    auto forwarders = std::string{};
    for (const auto& function : functions) {
        const auto arguments = argument_list(function);
        interface += "        " + function.name + ": " + signature(function, "", true) + ";\n";
        model += "        " + function.name + ": " + signature(function, "override ", true) +
            body(function, name + "_calls<T>::__cpp2_" + function.name + "(__cpp2_value" +
                               (arguments.empty() ? "" : ", " + arguments) + ")");
        forwarders += "    " + function.name + ": " + signature(function, "", true) +
            body(function, "__cpp2_self*." + function.name + "(" + arguments + ")");
    }
    return dyn + ": type = {\n"
        "    __cpp2_interface: @interface type = {\n"
        "        __cpp2_clone: (this) -> std::unique_ptr<__cpp2_interface>;\n" + interface +
        "    }\n"
        "    __cpp2_model: <T: " + name + "> type = {\n"
        "        this: __cpp2_interface = ();\n"
        "        __cpp2_value: T;\n"
        "        operator=: (out this, copy value: T) = { __cpp2_value = value; }\n"
        "        __cpp2_clone: (override this) -> std::unique_ptr<__cpp2_interface> = "
        "std::make_unique<__cpp2_model>(__cpp2_value);\n" + model +
        "    }\n"
        "    __cpp2_self: std::unique_ptr<__cpp2_interface>;\n"
        "    operator=: <T: " + name + "> (implicit out this, copy value: T) "
        "requires !std::same_as<T, " + dyn + "> && std::copy_constructible<T> = { "
        "__cpp2_self = std::make_unique<__cpp2_model<T>>(value); }\n"
        "    operator=: (out this, that) = { __cpp2_self = that.__cpp2_self*.__cpp2_clone(); }\n"
        "    operator=: (out this, move that) = { __cpp2_self = that.__cpp2_self; }\n"
        "    operator=: (inout this, that) = { __cpp2_self = that.__cpp2_self*.__cpp2_clone(); }\n"
        "    operator=: (inout this, move that) = { __cpp2_self = that.__cpp2_self; }\n" +
        forwarders + "}\n";
}

// Parses Cpp2 declarations generated for the trait `origin`, declared in
// `parent` (null at file scope).
auto parse_declarations(std::string_view text, const ir_declaration* parent,
                        const ir_declaration& origin, ir_program& program,
                        std::vector<diagnostic>& diagnostics) -> std::vector<ir_declaration> {
    auto file_scope = ir_declaration{};
    file_scope.kind = declaration_kind::namespace_node;
    auto parsed = parse_generated_members(text, parent == nullptr ? file_scope : *parent,
                                          program, origin.span);
    if (!parsed.error.empty()) {
        add_error(diagnostics, origin.span, "trait-generated-code",
                  "'@trait' generated invalid Cpp2: " + parsed.error);
        return {};
    }
    if (parent == nullptr) {
        for (auto& declaration : parsed.members) declaration.scope = declaration_scope::file;
    }
    return std::move(parsed.members);
}

}

auto trait_table::named_by(const ir_declaration& type) const -> const trait_info* {
    const auto found = named.find(type.symbol);
    if (found == named.end()) return nullptr;
    const auto trait = traits.find(found->second);
    return trait == traits.end() ? nullptr : &trait->second;
}

auto collect_traits(const ir_program& program) -> trait_table {
    auto result = trait_table{};
    auto names_trait = false;
    ir_detail::visit_declarations(program.declarations, [&](const ir_declaration& declaration) {
        if (declaration.kind != declaration_kind::type) return;
        names_trait = names_trait || has_metaclass(declaration, "impl") ||
            has_metaclass(declaration, "given");
        if (!has_metaclass(declaration, "trait")) return;
        auto& info = result.traits[declaration.symbol];
        info.name = declaration.name;
        info.qualified_name = qualified_name(program, declaration.symbol);
        info.scope_name = scope_name(program, declaration.symbol);
        for (const auto& member : declaration.members) {
            if (member.kind == declaration_kind::function && !member.is_generated) {
                info.functions.push_back(member.name);
            }
        }
    });
    if (result.traits.empty() || !names_trait) return result;
    const auto scopes = scope_model{program};
    ir_detail::visit_declarations(program.declarations, [&](const ir_declaration& declaration) {
        for (const auto* metaclass : {"impl", "given"}) {
            const auto* arguments = metaclass_arguments_of(declaration, metaclass);
            if (declaration.kind != declaration_kind::type || arguments == nullptr ||
                arguments->empty()) {
                continue;
            }
            const auto* entry = scopes.lookup(arguments->front(), scopes.outside(declaration.symbol));
            if (entry != nullptr) entry = scopes.resolve(*entry);
            if (entry != nullptr && result.traits.contains(entry->symbol)) {
                result.named[declaration.symbol] = entry->symbol;
            }
        }
    });
    return result;
}

auto apply_trait(ir_program& program, ir_declaration& type, const ir_declaration* parent,
                 std::vector<diagnostic>& diagnostics) -> surrounding_declarations {
    if (parent != nullptr && parent->kind != declaration_kind::namespace_node) {
        add_error(diagnostics, type.span, "trait-scope",
                  "a trait can only be declared at namespace scope");
        return {};
    }
    const auto functions = trait_functions(type, diagnostics);
    if (!functions) return {};
    auto collides = false;
    const auto& siblings = parent == nullptr ? program.declarations : parent->members;
    for (const auto& name : {type.name + "_calls", type.name + "_archetype", "dyn_" + type.name}) {
        for (const auto& sibling : siblings) {
            if (sibling.name != name) continue;
            add_error(diagnostics, sibling.span, "trait-generated-name",
                      "the trait '" + type.name + "' declares '" + name +
                      "'; rename this declaration");
            collides = true;
        }
    }
    if (collides) return {};
    auto before = parse_declarations(render_calls(type.name, *functions), parent, type, program,
                                     diagnostics);
    auto concept_declaration = parse_declarations(
        type.name + ": <__cpp2_T> concept = " + type.name +
            "_calls<__cpp2_T>::__cpp2_satisfied();",
        parent, type, program, diagnostics);
    auto after = parse_declarations(
        render_archetype(type.name, *functions) + render_dyn(type.name, *functions), parent,
        type, program, diagnostics);
    if (before.size() != 1 || concept_declaration.size() != 1 || after.size() != 2) return {};
    before.front().is_defined_last = true;
    // The trait becomes the concept, keeping its symbol.
    auto& generated = concept_declaration.front();
    for (auto& symbol : program.symbols) {
        if (symbol.parent == generated.symbol) symbol.parent = type.symbol;
        if (symbol.id == generated.symbol) symbol.name.clear();
    }
    program.symbols[type.symbol - 1].kind = declaration_kind::concept_declaration;
    type.kind = declaration_kind::concept_declaration;
    type.template_parameters = std::move(generated.template_parameters);
    type.body = std::move(generated.body);
    type.members.clear();
    return {std::move(before), std::move(after)};
}

auto apply_impl(const ir_program& program, ir_declaration& type, const trait_table& traits,
                std::vector<diagnostic>& diagnostics) -> bool {
    const auto* listed = metaclass_arguments_of(type, "impl");
    const auto arguments = listed == nullptr ? std::vector<std::string>{} : *listed;
    const auto error = [&](std::string code, std::string message) {
        add_error(diagnostics, type.span, std::move(code), std::move(message));
    };
    if (arguments.empty() || arguments.size() > 2) {
        error("impl-arguments", "'@impl' takes a trait, as in '@impl<Shape>', or a trait and "
              "the type that implements it, as in '@impl<Shape, Circle>'");
        return false;
    }
    const auto is_marker = arguments.size() == 2;
    if (!type.template_parameters.empty()) {
        error("impl-template-type", "a template type cannot be checked against a trait");
        return is_marker;
    }
    if (is_marker &&
        std::any_of(type.members.begin(), type.members.end(),
                    [](const ir_declaration& member) { return !member.is_generated; })) {
        error("impl-external-member", "a type declared with '@impl<Trait, Type>' only "
              "records that Type implements Trait, so it must have no members");
        return true;
    }
    const auto* trait = traits.named_by(type);
    if (trait == nullptr) {
        error("impl-not-trait", "'" + arguments[0] + "' does not name a trait");
        return is_marker;
    }
    type.trailing_cpp1 = "static_assert(" + trait->qualified_name + "<" +
        (is_marker ? render_cpp1_type(parse_type(arguments[1]))
                   : qualified_name(program, type.symbol)) +
        ">, \"" + (is_marker ? arguments[1] : type.name) + " does not satisfy " + trait->name +
        "\");";
    return is_marker;
}

auto apply_given(ir_program& program, ir_declaration& type, const ir_declaration* parent,
                 const trait_table& traits, std::vector<diagnostic>& diagnostics)
    -> surrounding_declarations {
    const auto* listed = metaclass_arguments_of(type, "given");
    const auto arguments = listed == nullptr ? std::vector<std::string>{} : *listed;
    const auto error = [&](const source_span& span, std::string code, std::string message) {
        add_error(diagnostics, span, std::move(code), std::move(message));
    };
    if (arguments.size() != 2) {
        error(type.span, "given-arguments",
              "'@given' takes a trait and the type it is given for, as in '@given<Show, int>'");
        return {};
    }
    if (!type.template_parameters.empty()) {
        error(type.span, "given-template-type", "a given cannot be a template");
        return {};
    }
    const auto* trait = traits.named_by(type);
    if (trait == nullptr) {
        error(type.span, "given-not-trait", "'" + arguments[0] + "' does not name a trait");
        return {};
    }
    auto failed = false;
    if ((parent != nullptr && parent->kind != declaration_kind::namespace_node) ||
        scope_name(program, type.symbol) != trait->scope_name) {
        error(type.span, "given-scope", "a given of '" + arguments[0] +
              "' must be declared in the namespace that declares the trait");
        failed = true;
    }
    const auto& instance = arguments[1];
    const auto instance_type = render_cpp1_type(parse_type(instance));
    for (const auto& member : type.members) {
        if (member.is_generated) continue;
        if (member.kind != declaration_kind::function) {
            error(member.span, "given-member-kind", "a given may only declare functions");
            failed = true;
        }
        else if (receiver(member) != nullptr) {
            error(member.span, "given-this", "a given function takes the instance as its first "
                  "parameter, as in '(x: " + instance + ")', not 'this'");
            failed = true;
        }
        else if (member.parameters.empty() ||
                 render_cpp1_type(member.parameters.front().type) != instance_type) {
            error(member.span, "given-instance-parameter",
                  "the first parameter of a given function must have the type the given is "
                  "for, '" + instance + "'");
            failed = true;
        }
    }
    for (const auto& name : trait->functions) {
        if (std::none_of(type.members.begin(), type.members.end(),
                         [&](const ir_declaration& member) { return member.name == name; })) {
            error(type.span, "given-missing-function", "the given of '" + arguments[0] +
                  "' for '" + instance + "' does not define '" + name + "'");
            failed = true;
        }
    }
    if (failed) return {};
    // The functions become free functions after the given, where UFCS finds them.
    const auto enclosing = program.symbols[type.symbol - 1].parent;
    auto after = std::vector<ir_declaration>{};
    for (auto& member : type.members) {
        if (member.is_generated) continue;
        member.is_type_member = false;
        member.scope = parent == nullptr ? declaration_scope::file : declaration_scope::namespace_scope;
        member.access = access_kind::default_access;
        if (member.symbol != invalid_symbol) program.symbols[member.symbol - 1].parent = enclosing;
        after.push_back(std::move(member));
    }
    std::erase_if(type.members, [](const ir_declaration& member) { return !member.is_generated; });
    return {{}, std::move(after)};
}

auto check_trait_uses(ir_program& program, const trait_table& traits,
                      std::vector<diagnostic>& diagnostics) -> void {
    if (traits.traits.empty()) return;
    const auto scopes = scope_model{program};
    const auto trait_at = [&](const type_id& type, scope_point point) -> const trait_info* {
        if (type.kind != type_kind::name || !point.valid()) return nullptr;
        const auto* entry = scopes.lookup(type.text, point);
        if (entry != nullptr) entry = scopes.resolve(*entry);
        if (entry == nullptr) return nullptr;
        const auto found = traits.traits.find(entry->symbol);
        return found == traits.traits.end() ? nullptr : &found->second;
    };
    const auto check = [&](const type_id& type, const source_span& span, scope_point point) {
        if (const auto* trait = trait_at(type, point)) {
            diagnostics.push_back({"trait-as-type", diagnostic_severity::error,
                                   type.span.begin.line != 0 ? type.span : span,
                                   "'" + type.text + "' is a trait, not a type; use 'dyn_" +
                                   trait->name + "' for a value of any type satisfying it, or "
                                   "constrain a template parameter with it"});
        }
    };
    // The qualified and the plain names of the archetypes of the traits
    // constraining every template parameter of `function`, when no parameter
    // adds an implicit one; empty otherwise.
    const auto archetypes = [&](const ir_declaration& function)
        -> std::pair<std::string, std::string> {
        auto result = std::pair<std::string, std::string>{};
        const auto point = scopes.inside(function.symbol);
        for (const auto& parameter : function.template_parameters) {
            const auto* trait = parameter.is_variadic ? nullptr : trait_at(parameter.type, point);
            if (trait == nullptr) return {};
            const auto separator = result.first.empty() ? "" : ", ";
            result.first += separator + trait->qualified_name + "_archetype";
            result.second += separator + trait->name + "_archetype";
        }
        const auto deduced = std::any_of(function.parameters.begin(), function.parameters.end(),
            [](const parameter& value) { return value.type.is_wildcard() || value.is_variadic; });
        return deduced ? std::pair<std::string, std::string>{} : result;
    };
    const auto visit = [&](const auto& self, std::vector<ir_declaration>& declarations) -> void {
        for (auto& declaration : declarations) {
            if (declaration.is_generated) continue;
            self(self, declaration.members);
            if (declaration.kind != declaration_kind::function &&
                declaration.kind != declaration_kind::object) {
                continue;
            }
            const auto inside = scopes.inside(declaration.symbol);
            check(declaration.type, declaration.span, inside);
            for (const auto& parameter : declaration.parameters) {
                check(parameter.type, parameter.name_span, inside);
            }
            if (declaration.function_statements) {
                ir_detail::visit_statements(declaration.function_statements->statements,
                    [&](const statement& value) {
                        if (value.kind != statement_kind::declaration_statement) return;
                        const auto& form = value.as<statement::declaration_form>();
                        if (form.declaration) {
                            check(form.declaration->type, value.span, scopes.at(value));
                        }
                    });
            }
            const auto overloads = std::count_if(declarations.begin(), declarations.end(),
                [&](const ir_declaration& other) {
                    return other.kind == declaration_kind::function && other.name == declaration.name;
                });
            if (declaration.kind != declaration_kind::function || declaration.is_type_member ||
                declaration.template_parameters.empty() || !declaration.requires_clause.empty() ||
                overloads != 1) {
                continue;
            }
            if (const auto [checked, names] = archetypes(declaration); !checked.empty()) {
                declaration.trailing_cpp1 = "static_assert((static_cast<void>(&" +
                    declaration.name + "<" + checked + ">), true), \"the body of " +
                    declaration.name + " is checked against " + names + "\");";
            }
        }
    };
    visit(visit, program.declarations);
}

}
