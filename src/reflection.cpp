#include <cppfrontier/pipeline.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <compare>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cpp1_expression_renderer.hpp"
#include "cpp1_type_renderer.hpp"
#include "ir_walk.hpp"
#include "autodiff_metafunction.hpp"
#include "generated_members.hpp"
#include "regex_metafunction.hpp"
#include "trait_metafunction.hpp"

namespace cppfrontier {

namespace {

auto is_base_member(const ir_declaration& member) -> bool {
    return member.kind == declaration_kind::object && member.name == "this";
}

// An object member that each value holds, a base or a data member: not a
// `==` constant or a `lazy` object, which are static.
auto is_instance_object(const ir_declaration& member) -> bool {
    return member.kind == declaration_kind::object && member.alias.empty() && !member.is_lazy;
}

auto is_data_member(const ir_declaration& member) -> bool {
    return is_instance_object(member) && !is_base_member(member);
}

// A base or a data member held in a helper base: what a constructor initializes
// as a base, and its name in a leading `name = value;` statement.
auto base_type(const ir_declaration& member) -> const std::string& {
    return member.base_helper.empty() ? member.type.text : member.base_helper;
}

auto is_value_type(const ir_declaration& declaration) -> bool {
    return declaration.kind == declaration_kind::type &&
        std::any_of(declaration.metaclasses.begin(), declaration.metaclasses.end(),
            [](const std::string& name) {
                return name == "value" || name == "basic_value" ||
                    name == "weakly_ordered_value" || name == "partially_ordered_value";
            });
}

auto value_set_constructor_kind(const ir_declaration& input)
    -> ir_declaration::special_member_kind {
    if (!is_value_set_constructor(input)) {
        return ir_declaration::special_member_kind::none;
    }
    if (input.parameters.size() == 1) {
        return ir_declaration::special_member_kind::user_default_constructor;
    }
    if (const auto* that = that_parameter(input); that != nullptr && input.parameters.size() == 2) {
        return that->passing == passing_style::move
            ? ir_declaration::special_member_kind::move_constructor
            : ir_declaration::special_member_kind::copy_constructor;
    }
    return ir_declaration::special_member_kind::converting_constructor;
}

auto value_set_assignment_kind(const ir_declaration& input)
    -> ir_declaration::special_member_kind {
    if (!is_value_set_assignment(input)) return ir_declaration::special_member_kind::none;
    if (input.parameters.size() != 2 || that_parameter(input) == nullptr) {
        return ir_declaration::special_member_kind::converting_assignment;
    }
    return input.parameters[1].passing == passing_style::move
        ? ir_declaration::special_member_kind::move_assignment
        : ir_declaration::special_member_kind::copy_assignment;
}

auto has_invalid_this_receiver(const ir_declaration& input) -> bool {
    const auto* value = input.kind == declaration_kind::function ? receiver(input) : nullptr;
    return value != nullptr && value->passing != passing_style::in &&
        value->passing != passing_style::inout && value->passing != passing_style::out &&
        value->passing != passing_style::move;
}

// types.md: `operator==` returns bool and `operator<=>` an ordering. Every
// comparison function is checked, member or not: `==`, `!=`, `<`, `<=`, `>`
// and `>=` return `bool`, and `<=>` a standard comparison category or a
// deduced type.
auto comparison_return_error(const ir_declaration& function) -> std::string {
    static constexpr auto boolean = std::array<std::string_view, 6>{
        "operator==", "operator!=", "operator<", "operator<=", "operator>", "operator>="};
    if (function.kind != declaration_kind::function) return {};
    const auto form = return_form_of(function);
    if (std::find(boolean.begin(), boolean.end(), function.name) != boolean.end()) {
        if (form == return_form::declared && function.type.text == "bool") return {};
        return function.name + " must return bool";
    }
    if (function.name != "operator<=>") return {};
    if (form != return_form::declared && form != return_form::declared_forwarding &&
        form != return_form::implicit_void) {
        return {};
    }
    auto name = std::string_view{function.type.text};
    if (name.starts_with("::")) name.remove_prefix(2);
    if (name.starts_with("std::")) name.remove_prefix(5);
    if (form == return_form::declared &&
        (name == "strong_ordering" || name == "weak_ordering" || name == "partial_ordering")) {
        return {};
    }
    return "operator<=> must return std::strong_ordering, std::weak_ordering, or "
           "std::partial_ordering";
}

// Marks an `operator=` member of `type` as a constructor, an assignment, or a
// destructor, and rejects malformed receivers. A member already marked, such
// as a generated one, is kept.
auto classify_special_member(const ir_declaration& type, ir_declaration& member,
                             std::vector<diagnostic>& diagnostics) -> void {
    using kind = ir_declaration::special_member_kind;
    if (member.special_member == kind::none) {
        const auto constructor_kind = value_set_constructor_kind(member);
        const auto assignment_kind = value_set_assignment_kind(member);
        if (constructor_kind != kind::none || assignment_kind != kind::none ||
            that_parameter(member) != nullptr) {
            member.special_member_owner = type.name;
        }
        if (constructor_kind != kind::none) {
            member.special_member = constructor_kind;
            if (constructor_kind != kind::converting_constructor) member.name = type.name;
        }
        else if (assignment_kind != kind::none) {
            member.special_member = assignment_kind;
        }
        else if (is_destructor(member)) {
            member.special_member_owner = type.name;
            member.special_member = kind::destructor;
            if (member.parameters.size() != 1) {
                diagnostics.push_back({
                    "invalid-destructor-parameters", diagnostic_severity::error, member.span,
                    "a destructor may not have parameters other than 'this'"});
            }
        }
    }
    if (member.kind == declaration_kind::function &&
        has_receiver(member, passing_style::out) && !is_constructor(member)) {
        diagnostics.push_back({
            "invalid-constructor-name", diagnostic_severity::error, member.span,
            "a function with an 'out this' parameter must be named 'operator='"});
    }
    if (has_invalid_this_receiver(member)) {
        diagnostics.push_back({
            "invalid-this-passing-style", diagnostic_severity::error, member.span,
            "a 'this' parameter must be in, inout, out, or move"});
    }
}

auto classify_special_members(ir_declaration& type, std::vector<diagnostic>& diagnostics)
    -> void {
    for (auto& member : type.members) classify_special_member(type, member, diagnostics);
}

// Adds a generated member with its own symbol.
auto add_member(ir_program& program, ir_declaration& type, ir_declaration member) -> void {
    member.symbol = add_symbol(program, {
        invalid_symbol, member.name, member.kind, type.symbol, 0,
        member.type, member.span, true});
    type.members.push_back(std::move(member));
}

auto is_assignment_with_that(const ir_declaration& declaration) -> bool {
    return is_assignment(declaration) && that_parameter(declaration) != nullptr &&
        (declaration.parameters[1].passing == passing_style::in ||
         declaration.parameters[1].passing == passing_style::move);
}

auto is_copy_constructor(const ir_declaration& declaration) -> bool {
    return declaration.special_member ==
        ir_declaration::special_member_kind::copy_constructor;
}

auto is_move_constructor(const ir_declaration& declaration) -> bool {
    return declaration.special_member ==
        ir_declaration::special_member_kind::move_constructor;
}

auto make_value_set_member(const ir_declaration& source, const ir_declaration& type,
                           ir_declaration::special_member_kind kind)
    -> ir_declaration {
    auto member = ir_declaration{};
    member.kind = declaration_kind::function;
    member.name = kind == ir_declaration::special_member_kind::copy_constructor
        ? type.name : "operator=";
    member.scope = declaration_scope::type_scope;
    member.access = source.access;
    member.span = source.span;
    member.special_member = kind;
    member.special_member_owner = type.name;
    member.is_generated = true;
    if (kind == ir_declaration::special_member_kind::move_constructor) {
        member.parameters = {
            parameter{passing_style::out, parse_type(type.name), "this"},
            parameter{passing_style::move, parse_type(type.name), "that"},
        };
    }
    else if (kind == ir_declaration::special_member_kind::copy_assignment ||
             kind == ir_declaration::special_member_kind::move_assignment) {
        member.parameters = source.parameters;
        member.parameters.front().passing = passing_style::inout;
        member.parameters.front().type = parse_type(type.name);
        if (that_parameter(member) == nullptr && member.parameters.size() > 1) {
            member.parameters.erase(member.parameters.begin());
            member.parameters.insert(member.parameters.begin(),
                parameter{passing_style::inout, parse_type(type.name), "this"});
        }
        if (kind == ir_declaration::special_member_kind::move_assignment &&
            that_parameter(member) != nullptr) {
            member.parameters[1].passing = passing_style::move;
        }
        member.type = parse_type(type.name + "&");
    }
    else {
        member.parameters = source.parameters;
    }
    // An assignment from a template converting constructor is a template too.
    member.template_parameters = source.template_parameters;
    member.requires_clause = source.requires_clause;
    member.contracts = source.contracts;
    member.is_constexpr = source.is_constexpr;
    member.function_statements = source.function_statements;
    member.body = source.body;
    member.cpp1_body = source.cpp1_body;
    return member;
}

struct declared_value_set final {
    const ir_declaration* copy_constructor = nullptr;
    const ir_declaration* move_constructor = nullptr;
    const ir_declaration* copy_assignment = nullptr;
    const ir_declaration* move_assignment = nullptr;
};

auto declared_value_set_of(const ir_declaration& type) -> declared_value_set {
    auto result = declared_value_set{};
    for (const auto& member : type.members) {
        switch (member.special_member) {
        case ir_declaration::special_member_kind::copy_constructor:
            result.copy_constructor = &member;
            break;
        case ir_declaration::special_member_kind::move_constructor:
            result.move_constructor = &member;
            break;
        case ir_declaration::special_member_kind::copy_assignment:
            result.copy_assignment = &member;
            break;
        case ir_declaration::special_member_kind::move_assignment:
            result.move_assignment = &member;
            break;
        default:
            break;
        }
    }
    return result;
}

auto has_assignment_from(const ir_declaration& type, std::string_view source_type) -> bool {
    return std::any_of(type.members.begin(), type.members.end(),
        [source_type](const ir_declaration& member) {
            return is_assignment(member) && member.parameters.size() == 2 &&
                !is_assignment_with_that(member) &&
                member.parameters[1].type.text == source_type;
        });
}

// A function with a `virtual`, `override` or `final` this parameter.
auto is_virtual_function(const ir_declaration& member) -> bool {
    return member.kind == declaration_kind::function && !member.parameters.empty() &&
        member.parameters.front().this_specifier != virtual_specifier::none;
}

// A type with a base or a virtual function.
auto is_polymorphic(const ir_declaration& type) -> bool {
    return std::any_of(type.members.begin(), type.members.end(), [](const auto& member) {
        return member.name == "this" || is_virtual_function(member);
    });
}

auto generate_value_set(ir_program& program, ir_declaration& type) -> void {
    const auto declared = declared_value_set_of(type);
    const auto* general_constructor = declared.copy_constructor;
    auto conversions = std::vector<const ir_declaration*>{};
    for (const auto& member : type.members) {
        if (member.special_member ==
                ir_declaration::special_member_kind::converting_constructor &&
            member.parameters.size() == 2) {
            conversions.push_back(&member);
        }
    }

    auto generated = std::vector<ir_declaration>{};
    const auto has_generated = [&](ir_declaration::special_member_kind kind) {
        return std::any_of(generated.begin(), generated.end(), [kind](const auto& member) {
            return member.special_member == kind;
        });
    };

    if (general_constructor != nullptr) {
        if (!declared.move_constructor) {
            generated.push_back(make_value_set_member(
                *general_constructor, type,
                ir_declaration::special_member_kind::move_constructor));
        }
        if (!declared.move_constructor && !declared.copy_assignment &&
            !declared.move_assignment) {
            generated.push_back(make_value_set_member(
                *general_constructor, type,
                ir_declaration::special_member_kind::copy_assignment));
            generated.push_back(make_value_set_member(
                *general_constructor, type,
                ir_declaration::special_member_kind::move_assignment));
        }
    }
    if (declared.copy_assignment && !declared.move_assignment &&
        !has_generated(ir_declaration::special_member_kind::move_assignment) &&
        declared.copy_assignment->parameters.size() == 2 &&
        that_parameter(*declared.copy_assignment) != nullptr) {
        generated.push_back(make_value_set_member(
            *declared.copy_assignment, type,
            ir_declaration::special_member_kind::move_assignment));
    }
    if (declared.copy_constructor && !declared.move_constructor &&
        !has_generated(ir_declaration::special_member_kind::move_constructor) &&
        declared.copy_constructor->parameters.size() == 2 &&
        that_parameter(*declared.copy_constructor) != nullptr) {
        generated.push_back(make_value_set_member(
            *declared.copy_constructor, type,
            ir_declaration::special_member_kind::move_constructor));
    }

    // As upstream, a polymorphic type has no generated converting assignment;
    // nor has an `explicit` converting constructor, which converts only explicitly.
    if (is_polymorphic(type)) conversions.clear();
    std::erase_if(conversions, [](const ir_declaration* conversion) {
        return conversion->is_explicit;
    });
    for (const auto* conversion : conversions) {
        const auto& source_type = conversion->parameters[1].type.text;
        const auto has_conversion_assignment = std::any_of(
            generated.begin(), generated.end(), [&](const ir_declaration& member) {
                return is_assignment(member) && member.parameters.size() == 2 &&
                    !is_assignment_with_that(member) &&
                    member.parameters[1].type.text == source_type;
            });
        if (!has_assignment_from(type, source_type) && !has_conversion_assignment) {
            generated.push_back(make_value_set_member(
                *conversion, type,
                ir_declaration::special_member_kind::copy_assignment));
        }
    }

    for (auto& member : generated) {
        add_member(program, type, std::move(member));
    }
}

auto has_assignment(const ir_declaration& declaration) -> bool {
    return std::any_of(declaration.members.begin(), declaration.members.end(),
        [](const ir_declaration& member) {
            return is_assignment(member) && member.parameters.size() > 1;
        });
}

auto has_assignment_with_that(const ir_declaration& declaration) -> bool {
    return std::any_of(declaration.members.begin(), declaration.members.end(),
        [](const ir_declaration& member) {
            return is_assignment_with_that(member);
        });
}

auto make_copy_assignment(const ir_declaration& declaration) -> ir_declaration {
    auto assignment = ir_declaration{};
    assignment.kind = declaration_kind::function;
    assignment.name = "operator=";
    assignment.type = parse_type(declaration.name + "&");
    assignment.scope = declaration_scope::type_scope;
    assignment.span = declaration.span;
    assignment.special_member =
        ir_declaration::special_member_kind::copy_assignment;
    assignment.special_member_owner = declaration.name;
    assignment.is_generated = true;
    assignment.parameters = {
        parameter{passing_style::inout, parse_type(declaration.name), "this"},
        parameter{passing_style::in, parse_type(declaration.name), "that"},
    };
    auto body = std::string{"{ "};
    for (const auto& member : declaration.members) {
        if (!is_data_member(member)) continue;
        body += cpp1_identifier(member.name) + " = that." + cpp1_identifier(member.name) + "; ";
    }
    body += "}";
    assignment.cpp1_body = std::move(body);
    return assignment;
}

auto is_cpp2_constructor(const ir_declaration& declaration) -> bool {
    return declaration.special_member ==
        ir_declaration::special_member_kind::user_default_constructor ||
        declaration.special_member == ir_declaration::special_member_kind::copy_constructor ||
        declaration.special_member == ir_declaration::special_member_kind::move_constructor ||
        declaration.special_member ==
            ir_declaration::special_member_kind::converting_constructor;
}

auto has_constructor_with_that(const ir_declaration& declaration) -> bool {
    return std::any_of(declaration.members.begin(), declaration.members.end(),
        [](const ir_declaration& member) {
            return member.special_member ==
                    ir_declaration::special_member_kind::copy_constructor ||
                member.special_member ==
                    ir_declaration::special_member_kind::move_constructor;
        });
}

auto make_default_constructor(const ir_declaration& type) -> ir_declaration {
    auto constructor = ir_declaration{};
    constructor.kind = declaration_kind::function;
    constructor.name = type.name;
    constructor.scope = declaration_scope::type_scope;
    constructor.access = access_kind::private_access;
    constructor.span = type.span;
    constructor.parameters = {
        parameter{passing_style::out, parse_type(type.name), "this"},
    };
    constructor.special_member = ir_declaration::special_member_kind::default_constructor;
    constructor.special_member_owner = type.name;
    return constructor;
}

auto make_defaulted_default_constructor(const ir_declaration& type) -> ir_declaration {
    auto constructor = ir_declaration{};
    constructor.kind = declaration_kind::function;
    constructor.name = type.name;
    constructor.scope = declaration_scope::type_scope;
    constructor.access = access_kind::public_access;
    constructor.span = type.span;
    constructor.special_member =
        ir_declaration::special_member_kind::defaulted_default_constructor;
    constructor.special_member_owner = type.name;
    return constructor;
}

auto make_copy_constructor(const ir_declaration& type) -> ir_declaration {
    auto constructor = ir_declaration{};
    constructor.kind = declaration_kind::function;
    constructor.name = type.name;
    constructor.scope = declaration_scope::type_scope;
    constructor.access = access_kind::public_access;
    constructor.span = type.span;
    constructor.parameters = {
        parameter{passing_style::out, parse_type(type.name), "this"},
        parameter{passing_style::in, parse_type(type.name), "that"},
    };
    constructor.special_member = ir_declaration::special_member_kind::copy_constructor;
    constructor.special_member_owner = type.name;
    for (const auto& member : type.members) {
        if (!is_data_member(member)) continue;
        constructor.member_initializers.push_back({member.name, "that." + cpp1_identifier(member.name)});
    }
    return constructor;
}

auto make_move_constructor(const ir_declaration& type) -> ir_declaration {
    auto constructor = ir_declaration{};
    constructor.kind = declaration_kind::function;
    constructor.name = type.name;
    constructor.scope = declaration_scope::type_scope;
    constructor.access = access_kind::public_access;
    constructor.span = type.span;
    constructor.parameters = {
        parameter{passing_style::out, parse_type(type.name), "this"},
        parameter{passing_style::move, parse_type(type.name), "that"},
    };
    constructor.special_member = ir_declaration::special_member_kind::move_constructor;
    constructor.special_member_owner = type.name;
    for (const auto& member : type.members) {
        if (!is_data_member(member)) continue;
        constructor.member_initializers.push_back(
            {member.name, "cpp2::move(that)." + cpp1_identifier(member.name)});
    }
    return constructor;
}

auto make_singleton_instance(const ir_declaration& type) -> ir_declaration {
    auto instance = ir_declaration{};
    instance.kind = declaration_kind::function;
    instance.name = "instance";
    instance.scope = declaration_scope::type_scope;
    instance.access = access_kind::public_access;
    instance.type = parse_type(type.name);
    instance.deduction = return_deduction::by_forwarding_typed;
    instance.is_static_function = true;
    instance.is_generated = true;
    instance.span = type.span;

    auto value = std::make_unique<statement>(statement_kind::declaration_statement);
    value->span = type.span;
    auto& declaration = value->as<statement::declaration_form>().declaration.emplace();
    declaration.name = "_instance";
    declaration.type = parse_type(type.name);
    declaration.initializer = parsed_expression{"()"};
    declaration.is_static = true;

    auto result = std::make_unique<statement>(statement_kind::return_statement);
    result->span = type.span;
    result->as<statement::expression_form>().text = parsed_expression{"_instance"};

    auto body = std::make_shared<function_body>();
    body->statements.push_back(std::move(value));
    body->statements.push_back(std::move(result));
    instance.function_statements = std::move(body);
    return instance;
}

auto make_deleted_copy_members(const ir_declaration& type)
    -> std::pair<ir_declaration, ir_declaration> {
    auto constructor = ir_declaration{};
    constructor.kind = declaration_kind::function;
    constructor.name = type.name;
    constructor.scope = declaration_scope::type_scope;
    constructor.access = access_kind::public_access;
    constructor.span = type.span;
    constructor.special_member = ir_declaration::special_member_kind::deleted_copy_constructor;
    constructor.special_member_owner = type.name;

    auto assignment = ir_declaration{};
    assignment.kind = declaration_kind::function;
    assignment.name = "operator=";
    assignment.scope = declaration_scope::type_scope;
    assignment.access = access_kind::public_access;
    assignment.span = type.span;
    assignment.special_member = ir_declaration::special_member_kind::deleted_copy_assignment;
    assignment.special_member_owner = type.name;
    return {std::move(constructor), std::move(assignment)};
}

auto validate_singleton(const ir_declaration& declaration, std::vector<diagnostic>& diagnostics)
    -> bool {
    auto valid = true;
    if (declaration.kind != declaration_kind::type) {
        diagnostics.push_back({
            "singleton-requires-type", diagnostic_severity::error, declaration.span,
            "'@singleton' can only be applied to a type declaration"});
        return false;
    }
    for (const auto& member : declaration.members) {
        if (is_cpp2_constructor(member)) {
            diagnostics.push_back({
                "singleton-constructor", diagnostic_severity::error, member.span,
                "a '@singleton' type must not have a user-written constructor"});
            valid = false;
        }
        if (is_data_member(member) && member.body.empty()) {
            diagnostics.push_back({
                "singleton-member-initializer", diagnostic_severity::error, member.span,
                "a '@singleton' data member must have a default initializer"});
            valid = false;
        }
    }
    return valid;
}

auto apply_singleton(ir_program& program, ir_declaration& declaration,
                     std::vector<diagnostic>& diagnostics) -> void {
    if (!validate_singleton(declaration, diagnostics)) return;
    if (std::none_of(declaration.members.begin(), declaration.members.end(),
                     [](const ir_declaration& member) {
                         return member.special_member ==
                             ir_declaration::special_member_kind::default_constructor;
                     })) {
        auto constructor = make_default_constructor(declaration);
        add_member(program, declaration, std::move(constructor));
    }
    add_member(program, declaration, make_singleton_instance(declaration));
    auto [copy_constructor, copy_assignment] = make_deleted_copy_members(declaration);
    add_member(program, declaration, std::move(copy_constructor));
    add_member(program, declaration, std::move(copy_assignment));
}

auto apply_implicit_special_members(ir_program& program, ir_declaration& declaration) -> void {
    if (declaration.kind != declaration_kind::type ||
        has_metaclass(declaration, "singleton")) {
        return;
    }
    const auto has_non_default_constructor = std::any_of(
        declaration.members.begin(), declaration.members.end(),
        [&declaration](const ir_declaration& member) {
            return is_constructor(member);
        });
    if (!has_non_default_constructor) {
        auto constructor = make_defaulted_default_constructor(declaration);
        add_member(program, declaration, std::move(constructor));
    }
    if (!has_constructor_with_that(declaration)) {
        auto [copy_constructor, copy_assignment] = make_deleted_copy_members(declaration);
        add_member(program, declaration, std::move(copy_constructor));
        if (has_assignment_with_that(declaration)) return;
        add_member(program, declaration, std::move(copy_assignment));
    }
}

// As upstream, the postfix form of `++` and `--` returns a copy, so a type
// that cannot be copied does not get one.
auto mark_postfix_counterparts(ir_declaration& type) -> void {
    const auto noncopyable = std::any_of(type.members.begin(), type.members.end(),
        [](const ir_declaration& member) {
            return member.special_member ==
                ir_declaration::special_member_kind::deleted_copy_constructor;
        });
    for (auto& member : type.members) {
        member.omits_postfix = noncopyable &&
            (member.name == "operator++" || member.name == "operator--");
    }
}

// `@struct`: only public members, no virtual functions and no user-written
// `operator=`. Nothing is generated, so the Cpp1 struct is an aggregate with
// its implicit copy and move operations, initialized memberwise as `S x {a, b}`.
auto apply_struct(ir_program& program, ir_declaration& declaration,
                  std::vector<diagnostic>& diagnostics) -> void {
    for (auto& member : declaration.members) {
        if (member.access == access_kind::private_access ||
            member.access == access_kind::protected_access) {
            diagnostics.push_back({
                "struct-member-access", diagnostic_severity::error, member.span,
                "all struct members must be public"});
        }
        if (member.special_member != ir_declaration::special_member_kind::none) {
            diagnostics.push_back({
                "struct-operator-assign", diagnostic_severity::error, member.span,
                "a struct may not have a user-defined operator="});
        }
        if (is_virtual_function(member)) {
            diagnostics.push_back({
                "struct-virtual-function", diagnostic_severity::error, member.span,
                "a struct may not have a virtual function"});
        }
        member.access = access_kind::public_access;
    }
    // A struct with a base gets a memberwise constructor, which initializes the
    // base from its initializer or by default, as an aggregate would
    // copy-list-initialize the base and so reject an explicit constructor.
    // Otherwise the struct is an aggregate.
    if (std::none_of(declaration.members.begin(), declaration.members.end(), is_base_member)) {
        return;
    }
    auto constructor = ir_declaration{};
    constructor.kind = declaration_kind::function;
    constructor.name = declaration.name;
    constructor.scope = declaration_scope::type_scope;
    constructor.access = access_kind::public_access;
    constructor.span = declaration.span;
    constructor.special_member = ir_declaration::special_member_kind::converting_constructor;
    constructor.special_member_owner = declaration.name;
    constructor.is_generated = true;
    constructor.parameters = {parameter{passing_style::out, parse_type(declaration.name), "this"}};
    auto body = std::make_shared<function_body>();
    auto has_all_initializers = true;
    for (const auto& member : declaration.members) {
        if (!is_data_member(member)) continue;
        has_all_initializers = has_all_initializers && !member.body.empty();
        constructor.parameters.push_back({passing_style::forward, member.type, member.name + "_"});
        auto& assignment = body->statements.emplace_back(
            std::make_unique<statement>(statement_kind::expression));
        assignment->span = declaration.span;
        assignment->as<statement::expression_form>().text =
            parsed_expression{member.name + " = " + member.name + "_"};
    }
    if (constructor.parameters.size() == 1) return;
    constructor.function_statements = std::move(body);
    add_member(program, declaration, std::move(constructor));
    if (has_all_initializers) {
        add_member(program, declaration, make_defaulted_default_constructor(declaration));
    }
}

// `@copyable`: a missing `(out this, that)` constructor is generated and
// expanded by `generate_value_set`. Returns false if the type declares only
// more specific copy or move functions.
auto apply_copyable(ir_program& program, ir_declaration& declaration,
                    std::vector<diagnostic>& diagnostics) -> bool {
    const auto declared = declared_value_set_of(declaration);
    if (declared.copy_constructor) return true;
    if (declared.move_constructor || declared.copy_assignment || declared.move_assignment) {
        diagnostics.push_back({
            "value-partially-copyable", diagnostic_severity::error, declaration.span,
            "this type is partially copyable/movable - when you provide any of the "
            "more-specific operator= signatures, you must also provide the one with "
            "the general signature (out this, that); alternatively, consider removing "
            "all the operator= functions and let them all be generated for you with "
            "default memberwise semantics"});
        return false;
    }
    auto copy = make_copy_constructor(declaration);
    copy.member_initializers.clear();
    copy.is_generated = true;
    add_member(program, declaration, std::move(copy));
    return true;
}

// `@value`, `@basic_value`: `@copyable` with a default constructor, and no
// protected or virtual functions or non-public destructor.
auto apply_basic_value(ir_program& program, ir_declaration& declaration,
                       std::vector<diagnostic>& diagnostics) -> void {
    for (const auto& member : declaration.members) {
        if (member.kind != declaration_kind::function) continue;
        if (member.access == access_kind::protected_access || is_virtual_function(member)) {
            diagnostics.push_back({
                "value-protected-or-virtual", diagnostic_severity::error, member.span,
                "a value type may not have a protected or virtual function"});
        }
        if (member.special_member == ir_declaration::special_member_kind::destructor &&
            (member.access == access_kind::protected_access ||
             member.access == access_kind::private_access)) {
            diagnostics.push_back({
                "value-non-public-destructor", diagnostic_severity::error, member.span,
                "a value type may not have a non-public destructor"});
        }
    }
    if (!apply_copyable(program, declaration, diagnostics)) return;
    const auto has_default_constructor = std::any_of(
        declaration.members.begin(), declaration.members.end(), [](const auto& member) {
            return member.special_member ==
                    ir_declaration::special_member_kind::user_default_constructor ||
                member.special_member ==
                    ir_declaration::special_member_kind::default_constructor ||
                member.special_member ==
                    ir_declaration::special_member_kind::defaulted_default_constructor;
        });
    if (!has_default_constructor) {
        // Not `explicit`, so an array of values can be list-initialized. Its
        // empty body must initialize every member, as a user-written one must.
        auto constructor = make_defaulted_default_constructor(declaration);
        constructor.function_statements = std::make_shared<function_body>();
        add_member(program, declaration, std::move(constructor));
    }
}

// The digits and base of the integer literal `text`, possibly negative and
// without a suffix, as in `-1'000`, `0x7f`, `0b101` or `017`; nullopt if
// `text` is not one.
auto integer_literal(std::string_view text) -> std::optional<std::pair<std::string, int>> {
    if (text.starts_with('-')) text.remove_prefix(1);
    auto base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) base = 16;
    else if (text.size() > 2 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) base = 2;
    else if (text.size() > 1 && text[0] == '0') base = 8;
    text.remove_prefix(base == 16 || base == 2 ? 2 : base == 8 ? 1 : 0);
    auto digits = std::string{};
    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto character = text[index];
        // A digit separator is between two digits.
        if (character == '\'' && !digits.empty() && index + 1 < text.size() &&
            text[index + 1] != '\'') {
            continue;
        }
        const auto digit = std::isdigit(static_cast<unsigned char>(character))
            ? character - '0'
            : std::isxdigit(static_cast<unsigned char>(character))
                ? std::tolower(static_cast<unsigned char>(character)) - 'a' + 10 : base;
        if (digit >= base) return std::nullopt;
        digits += character;
    }
    if (digits.empty()) return std::nullopt;
    return std::pair{std::move(digits), base};
}

// An enumerator value, as a sign and a magnitude that covers both i64 and u64.
struct enum_value {
    bool negative = false;
    std::uint64_t magnitude = 0;

    auto operator<=>(const enum_value& other) const -> std::strong_ordering {
        if (negative != other.negative) return other.negative <=> negative;
        return negative ? other.magnitude <=> magnitude : magnitude <=> other.magnitude;
    }
    auto operator==(const enum_value&) const -> bool = default;
};

auto to_string(const enum_value& value) -> std::string {
    return (value.negative ? "-" : "") + std::to_string(value.magnitude);
}

// The values an underlying integer type holds.
struct enum_range {
    enum_value min;
    enum_value max;
};

template <typename T>
constexpr auto range_of() -> enum_range {
    constexpr auto min = std::numeric_limits<T>::min();
    return {{min < 0, min < 0 ? static_cast<std::uint64_t>(-(min + 1)) + 1 : 0},
            {false, static_cast<std::uint64_t>(std::numeric_limits<T>::max())}};
}

// The range of a fixed-width or fundamental integer type, or nullopt.
auto enum_range_of(std::string_view type) -> std::optional<enum_range> {
    if (type.starts_with("std::")) type.remove_prefix(5);
    if (type == "i8" || type == "int8_t" || type == "signed char") return range_of<std::int8_t>();
    if (type == "i16" || type == "int16_t" || type == "short") return range_of<std::int16_t>();
    if (type == "i32" || type == "int32_t" || type == "int") return range_of<std::int32_t>();
    if (type == "i64" || type == "int64_t" || type == "long long") return range_of<std::int64_t>();
    if (type == "u8" || type == "uint8_t" || type == "unsigned char") return range_of<std::uint8_t>();
    if (type == "u16" || type == "uint16_t" || type == "unsigned short") {
        return range_of<std::uint16_t>();
    }
    if (type == "u32" || type == "uint32_t" || type == "unsigned" || type == "unsigned int") {
        return range_of<std::uint32_t>();
    }
    if (type == "u64" || type == "uint64_t" || type == "unsigned long long") {
        return range_of<std::uint64_t>();
    }
    return std::nullopt;
}

// The integer literal `text`, or nullopt if it is outside the range of u64
// or, when negative, of i64.
auto parse_enum_value(std::string_view text) -> std::optional<enum_value> {
    auto value = enum_value{text.starts_with('-'), 0};
    const auto literal = integer_literal(text);
    if (!literal) return std::nullopt;
    const auto& [digits, base] = *literal;
    if (std::from_chars(digits.data(), digits.data() + digits.size(), value.magnitude, base).ec !=
        std::errc{}) {
        return std::nullopt;
    }
    if (value.magnitude == 0) value.negative = false;
    if (value.negative && value.magnitude > range_of<std::int64_t>().min.magnitude) {
        return std::nullopt;
    }
    return value;
}

// The value after `previous`: one more, or for a flag enum the next power of
// two. Nullopt on overflow.
auto next_enum_value(const std::optional<enum_value>& previous, bool flags)
    -> std::optional<enum_value> {
    if (!previous) return enum_value{false, flags ? 1u : 0u};
    if (previous->negative) {
        return flags ? enum_value{false, 1}
                     : enum_value{previous->magnitude > 1, previous->magnitude - 1};
    }
    const auto limit = std::numeric_limits<std::uint64_t>::max();
    if (!flags) {
        if (previous->magnitude == limit) return std::nullopt;
        return enum_value{false, previous->magnitude + 1};
    }
    if (previous->magnitude == 0) return enum_value{false, 1};
    if (previous->magnitude > limit / 2) return std::nullopt;
    return enum_value{false, previous->magnitude * 2};
}

// Adds the Cpp2 members `text` that a metafunction generates to `type`.
auto add_generated_members(ir_program& program, ir_declaration& type, const std::string& text,
                           std::vector<diagnostic>& diagnostics) -> void {
    auto parsed = parse_generated_members(text, type, program, type.span);
    if (!parsed.error.empty()) {
        diagnostics.push_back({"metafunction-generated-code", diagnostic_severity::error,
                               type.span, "generated code is not valid Cpp2 (" + parsed.error +
                                   "):\n" + text});
        return;
    }
    for (auto& member : parsed.members) {
        classify_special_member(type, member, diagnostics);
        type.members.push_back(std::move(member));
    }
}

struct enumerator final {
    std::string name;
    // A Cpp2 expression.
    std::string value;
    // Where the user declared it; none for a generated `none`.
    std::optional<source_span> span;
};

// The members of an `@enum` or `@flag_enum` type with these enumerators, as
// upstream's `basic_enum` generates them except for the special members. The
// default value is the first enumerator, or `none` for flags, held as the
// initializer of `_value` unless it is zero.
auto enum_members(const std::string& name, const std::string& underlying,
                  const std::vector<enumerator>& enumerators, bool flags) -> std::string {
    const auto default_value = flags ? std::string{"none"} : enumerators.front().name;
    const auto& first = enumerators.front().value;
    auto text = "_value: " + underlying +
        (flags || first == "0" ? std::string{}
                               : " = cpp2::unchecked_narrow<" + underlying + ">(" + first + ")") +
        ";\n"
        "private operator=: (implicit out this, _val: i64) == "
        "_value = cpp2::unchecked_narrow<" + underlying + ">(_val);\n";
    if (flags) {
        for (const auto* op : {"|", "&", "^"}) {
            text += "operator" + std::string{op} + "=: (inout this, that) == _value " + op +
                "= that._value;\n";
        }
        for (const auto* op : {"|", "&", "^"}) {
            text += "operator" + std::string{op} + ": (this, that) -> " + name + " == _value " +
                op + " that._value;\n";
        }
        text += "has: (this, that) -> bool == _value & that._value;\n"
            "set: (inout this, that) == { _value |= that._value; }\n"
            "clear: (inout this, that) == { _value &= that._value~; }\n";
    }
    for (const auto& value : enumerators) {
        text += value.name + ": " + name + " == " + value.value + ";\n";
    }
    text += "get_raw_value: (this) -> " + underlying + " == _value;\n"
        "operator<=>: (this, that) -> std::strong_ordering;\n";
    text += "to_string_impl: (this, prefix: std::string_view" +
        std::string{flags ? ", separator: std::string_view) -> std::string = {\n"
                            "ret: std::string = \"(\";\nsep: std::string = ();\n"
                            "if this == none { return \"(none)\"; }\n"
                          : ") -> std::string = {\n"} +
        "pref := cpp2::to_string(prefix);\n";
    for (const auto& value : enumerators) {
        const auto qualified = name + "::" + value.name;
        if (!flags) {
            text += "if this == " + qualified + " { return pref + \"" + value.name + "\"; }\n";
        }
        else if (value.name != "none") {
            text += "if (this & " + qualified + ") == " + qualified + " { ret += sep + pref + \"" +
                value.name + "\"; sep = separator; }\n";
        }
    }
    text += flags ? "return ret + \")\";\n}\n"
                  : "return \"invalid " + name + " value\";\n}\n";
    text += flags
        ? "to_string: (this) -> std::string = to_string_impl(\"\", \", \");\n"
          "to_code: (this) -> std::string = to_string_impl(\"" + name + "::\", \" | \");\n"
        : "to_string: (this) -> std::string = to_string_impl(\"\");\n"
          "to_code: (this) -> std::string = to_string_impl(\"" + name + "::\");\n";
    text += "from_string: (s: std::string_view) -> " + name + " = {\n" +
        (flags ? "ret := none;\nouter: do {\n"
                 "for cpp2::string_util::split_string_list(s) do (x) {\n"
               : "x := s;\n");
    auto else_ = std::string{};
    for (const auto& value : enumerators) {
        text += else_ + "if \"" + value.name + "\" == x { " + (flags ? "ret |= " : "return ") +
            name + "::" + value.name + "; }\n";
        else_ = "else ";
    }
    if (flags) text += "else { break outer; }\n}\nreturn ret;\n} while false;\n";
    text += std::string{"cpp2::type_safety.report_violation((\"can't convert string '\" + "
        "cpp2::to_string(s) + \"' to "} + (flags ? "flag_" : "") + "enum of type " + name +
        "\").c_str());\nreturn " + name + "::" + default_value + ";\n}\n";
    text += "from_code: (s: std::string_view) -> " + name + " = { str: std::string = s; "
        "return from_string(cpp2::string_util::replace_all(str, \"" + name + "::\", \"\")); }\n";
    return text;
}

// The enumerator value `value` as the `i64` the generated constructor takes; a
// `u64` value above the `i64` range wraps and converts back.
auto enumerator_argument(const enum_value& value) -> std::string {
    if (!value.negative && value.magnitude > range_of<std::int64_t>().max.magnitude) {
        return "cpp2::unchecked_narrow<i64>(" + to_string(value) + "u)";
    }
    return to_string(value);
}

// Records the generated member `name` of `kind` as a case of the sum type `declaration`.
auto add_sum_case(ir_declaration& declaration, std::string_view name, declaration_kind kind)
    -> void {
    for (const auto& member : declaration.members) {
        if (member.is_generated && member.kind == kind && member.name == name) {
            declaration.sum_cases.push_back(member.symbol);
        }
    }
}

// `@enum<U>`: each object member is an enumerator whose value is its initializer
// or one more than the previous value. Without `U`, the smallest signed type
// holding every value is the underlying type. `@flag_enum<U>` values default to
// successive powers of two, a `none = 0` enumerator is added, and the default
// underlying type is the smallest unsigned type holding twice the largest value.
auto apply_enum(ir_program& program, ir_declaration& declaration,
                std::vector<diagnostic>& diagnostics) -> void {
    const auto error = [&](std::string code, source_span span, std::string message) {
        diagnostics.push_back({std::move(code), diagnostic_severity::error, span,
                               std::move(message)});
    };
    const auto flags = has_metaclass(declaration, "flag_enum");
    const auto metaclass = std::string{flags ? "flag_enum" : "enum"};
    const auto position = static_cast<std::size_t>(
        std::find(declaration.metaclasses.begin(), declaration.metaclasses.end(), metaclass) -
        declaration.metaclasses.begin());
    auto underlying = std::string{};
    if (position < declaration.metaclass_arguments.size() &&
        !declaration.metaclass_arguments[position].empty()) {
        underlying = declaration.metaclass_arguments[position].front();
    }
    // Each value must fit the underlying type, or without one the largest
    // default underlying type.
    const auto declared_range = enum_range_of(underlying);
    const auto default_range =
        flags ? range_of<std::uint64_t>() : range_of<std::int64_t>();
    const auto range = declared_range.value_or(default_range);
    const auto range_name = declared_range ? underlying : flags ? "u64" : "i64";
    auto value = std::optional<enum_value>{};
    auto min_value = enum_value{};
    auto max_value = enum_value{};
    auto found_non_numeric = false;
    auto enumerators = std::vector<enumerator>{};
    std::erase_if(declaration.members, [&](const ir_declaration& member) {
        const auto special = member.special_member != ir_declaration::special_member_kind::none;
        if (special || (member.kind == declaration_kind::function &&
                        member.name == "operator<=>")) {
            error("enum-reserved-name", member.span, "an enum may not have a user-defined " +
                  (special ? std::string{"operator="} : member.name));
            return false;
        }
        if (!is_instance_object(member)) return false;
        if (member.symbol != invalid_symbol && program.symbols[member.symbol - 1].name == "_") {
            error("enum-anonymous-enumerator", member.span,
                  "an enumerator must have a name, which its value is spelled with");
        }
        if (member.access == access_kind::private_access ||
            member.access == access_kind::protected_access) {
            error("enum-enumerator-access", member.span,
                  "an enumerator cannot be protected or private");
        }
        if (std::any_of(enumerators.begin(), enumerators.end(),
                        [&](const enumerator& other) { return other.name == member.name; })) {
            error("enum-duplicate-enumerator", member.span,
                  "'" + member.name + "' is already an enumerator of " + declaration.name);
        }
        if (!member.type.is_wildcard()) {
            error("enum-enumerator-type", member.span,
                  "an explicit underlying type should be specified as a compile-time argument "
                  "to the metafunction - try 'enum<u16>'");
        }
        const auto& init = member.body.source();
        if (init.empty() || integer_literal(init)) {
            if (found_non_numeric) {
                error("enum-enumerator-order", member.span,
                      member.name + ": enumerators with non-numeric values must come after all "
                      "default and numeric values");
            }
            const auto next = init.empty() ? next_enum_value(value, flags)
                                           : parse_enum_value(init);
            if (!next || *next < range.min || range.max < *next) {
                error("enum-enumerator-range", member.span,
                      member.name + ": the value is outside the range of " + range_name);
            }
            if (next) value = next;
            const auto current = value.value_or(enum_value{});
            min_value = std::min(min_value, current);
            max_value = std::max(max_value, current);
            enumerators.push_back({member.name, enumerator_argument(current), member.span});
        }
        else {
            found_non_numeric = true;
            enumerators.push_back({member.name, init, member.span});
        }
        return true;
    });
    if (enumerators.empty()) {
        error("enum-empty", declaration.span,
              "an enumeration must contain at least one enumerator value");
        return;
    }
    if (flags) enumerators.push_back({"none", "0", std::nullopt});
    if (underlying.empty()) {
        if (found_non_numeric) {
            error("enum-requires-underlying-type", declaration.span,
                  "if you write an enumerator with a non-numeric-literal value, you must "
                  "specify the enumeration's underlying type");
        }
        const auto fits = [&](const enum_range& limits) {
            return limits.min <= min_value && max_value <= limits.max;
        };
        const auto umax = max_value.magnitude > std::numeric_limits<std::uint64_t>::max() / 2
            ? std::numeric_limits<std::uint64_t>::max() : max_value.magnitude * 2;
        underlying = flags
            ? (umax <= 0xff ? "u8" : umax <= 0xffff ? "u16" : umax <= 0xffff'ffff ? "u32" : "u64")
            : fits(range_of<std::int8_t>()) ? "i8" : fits(range_of<std::int16_t>()) ? "i16" :
                fits(range_of<std::int32_t>()) ? "i32" : "i64";
    }
    const auto first_generated = declaration.members.size();
    add_generated_members(program, declaration,
                          enum_members(declaration.name, underlying, enumerators, flags),
                          diagnostics);
    // The enumerators are written where the user declared them, in order, so a
    // clash with a generated name is reported there.
    auto written = enumerators.begin();
    for (auto index = first_generated; index < declaration.members.size(); ++index) {
        auto& member = declaration.members[index];
        if (written == enumerators.end() || !written->span ||
            member.kind != declaration_kind::object || member.name != written->name) {
            continue;
        }
        member.span = *written->span;
        if (member.symbol != invalid_symbol) program.symbols[member.symbol - 1].span = member.span;
        ++written;
    }
    // A `@flag_enum` value may combine flags, so it is not a closed sum type.
    if (!flags) {
        for (const auto& value : enumerators) {
            add_sum_case(declaration, value.name, declaration_kind::object);
        }
    }
    // The special members are Cpp1's defaults, so the type is trivially copyable,
    // and trivial when its default value is zero. `e as U` uses the explicit
    // conversion, which Cpp2 cannot declare.
    add_member(program, declaration, make_defaulted_default_constructor(declaration));
    auto conversion = ir_declaration{};
    conversion.kind = declaration_kind::function;
    conversion.name = "operator " + underlying;
    conversion.scope = declaration_scope::type_scope;
    conversion.access = access_kind::public_access;
    conversion.span = declaration.span;
    conversion.is_generated = true;
    conversion.cpp1_text = "constexpr explicit operator " + render_cpp1_type(underlying) +
        "() const { return _value; }";
    add_member(program, declaration, std::move(conversion));
}

// `@union`: each object member is an alternative held in shared storage with a
// discriminator, with the accessors and special members upstream's `union`
// generates, except that a move moves the alternative.
auto apply_union(ir_program& program, ir_declaration& declaration,
                 std::vector<diagnostic>& diagnostics) -> void {
    const auto error = [&](std::string code, source_span span, std::string message) {
        diagnostics.push_back({std::move(code), diagnostic_severity::error, span,
                               std::move(message)});
    };
    struct alternative final {
        std::string name;
        std::string type;
    };
    auto alternatives = std::vector<alternative>{};
    std::erase_if(declaration.members, [&](const ir_declaration& member) {
        if (member.name.starts_with("is_") || member.name.starts_with("set_")) {
            error("union-alternative-name", member.span,
                  "a union alternative's name cannot start with 'is_' or 'set_' - that could "
                  "cause user confusion with the 'is_alternative' and 'set_alternative' "
                  "generated functions");
        }
        if (!is_data_member(member)) return false;
        if (member.access == access_kind::private_access ||
            member.access == access_kind::protected_access) {
            error("union-alternative-access", member.span,
                  "a union alternative cannot be protected or private");
        }
        if (member.symbol != invalid_symbol && program.symbols[member.symbol - 1].name == "_") {
            error("union-anonymous-alternative", member.span,
                  "a union alternative must have a name, which its accessors use");
        }
        if (!member.body.empty()) {
            error("union-alternative-initializer", member.span,
                  "a union alternative cannot have an initializer");
        }
        if (std::any_of(alternatives.begin(), alternatives.end(),
                        [&](const alternative& other) { return other.name == member.name; })) {
            error("union-duplicate-alternative", member.span,
                  "'" + member.name + "' is already an alternative of " + declaration.name);
        }
        alternatives.push_back({member.name, member.type.text});
        return true;
    });
    if (alternatives.empty()) {
        error("union-empty", declaration.span, "a union must contain at least one alternative");
        return;
    }
    auto sizes = std::string{};
    auto alignments = std::string{};
    // `std::type_identity_t` makes each type a template argument, where any Cpp2
    // type can be written, as `*const char` cannot be in `sizeof(...)`.
    for (const auto& [name, type] : alternatives) {
        const auto identity = "(std::type_identity_t<" + type + ">)";
        sizes += (sizes.empty() ? "sizeof" : ", sizeof") + identity;
        alignments += (alignments.empty() ? "alignof" : ", alignof") + identity;
    }
    const auto count = alternatives.size();
    auto text = "_storage: cpp2::aligned_storage<cpp2::max(" + sizes + "), cpp2::max(" +
        alignments + ")> = ();\n_discriminator: " +
        (count < 0x7f ? "i8" : count < 0x7fff ? "i16" : "i32") + " = -1;\n";
    auto destroy = std::string{};
    auto value_set = std::string{};
    auto take = std::string{};
    for (std::size_t index = 0; index < count; ++index) {
        const auto& [name, type] = alternatives[index];
        const auto value = std::to_string(index);
        const auto storage = "reinterpret_cast<*" + type + ">(_storage&)";
        text += "is_" + name + ": (this) -> bool = _discriminator == " + value + ";\n" +
            name + ": (this) -> forward " + type + " pre(is_" + name + "()) = "
            "reinterpret_cast<* const " + type + ">(_storage&)*;\n" +
            name + ": (inout this) -> forward " + type + " pre(is_" + name + "()) = " +
            storage + "*;\n" +
            "set_" + name + ": (inout this, _value: " + type + ") = { if !is_" + name +
            "() { _destroy(); std::construct_at(" + storage + ", _value); } else { " + storage +
            "* = _value; } _discriminator = " + value + "; }\n" +
            "set_" + name + ": (inout this, forward _args...: _) = { if !is_" + name +
            "() { _destroy(); std::construct_at(" + storage + ", forward _args...); } else { " +
            storage + "* = :" + type + " = (forward _args...); } _discriminator = " + value +
            "; }\n";
        destroy += "if _discriminator == " + value + " { std::destroy_at(" + storage + "); }\n";
        value_set += "if that.is_" + name + "() { set_" + name + "(that." + name + "()); }\n";
        take += "if source.is_" + name + "() { set_" + name + "(move source." + name + "()); }\n";
    }
    text += "private _destroy: (inout this) = {\n" + destroy + "_discriminator = -1;\n}\n"
        "operator=: (move this) = { _destroy(); _ = this; }\n"
        "operator=: (out this, that) = {\n_storage = ();\n_discriminator = -1;\n" + value_set +
        "}\n"
        "operator=: (inout this, that) = {\n_storage = _;\n_discriminator = _;\n" + value_set +
        "}\n"
        "operator=: (out this, move that) = {\n_storage = ();\n_discriminator = -1;\n"
        "_take(that);\n_ = that;\n}\n"
        "operator=: (inout this, move that) = {\n_storage = _;\n_discriminator = _;\n"
        "_take(that);\n_ = that;\n}\n"
        "private _take: (inout this, inout source: " + declaration.name + ") = {\n" + take +
        "}\n";
    add_generated_members(program, declaration, text, diagnostics);
    // Not `explicit`, so an array of unions can be list-initialized.
    add_member(program, declaration, make_defaulted_default_constructor(declaration));
}

auto reflect_type(ir_program& program, ir_declaration& declaration, ir_declaration* parent,
                  std::vector<std::pair<ir_declaration*, ir_declaration>>& namespace_members,
                  std::vector<std::pair<symbol_id, surrounding_declarations>>& surrounding,
                  const trait_table& traits, const experiment_set& experiments,
                  std::vector<diagnostic>& diagnostics) -> void;

// A constructor that initializes the bases and data members.
auto initializes_members(const ir_declaration& function) -> bool {
    return is_constructor(function) &&
        function.special_member != ir_declaration::special_member_kind::deleted_copy_constructor;
}

// The base initialized by a leading `Base = value;` statement, or nullopt.
auto base_initializer(const statement& statement, const std::string& base_type)
    -> std::optional<std::string> {
    if (statement.kind != statement_kind::expression) return std::nullopt;
    const auto& source = statement.as<statement::expression_form>().text.source();
    auto equals = source.find('=');
    while (equals != std::string::npos &&
           ((equals > 0 && std::string_view{"<>!="}.find(source[equals - 1]) !=
                std::string_view::npos) ||
            (equals + 1 < source.size() && source[equals + 1] == '='))) {
        equals = source.find('=', equals + 2);
    }
    if (equals == std::string::npos) return std::nullopt;
    const auto without_spaces = [](std::string_view text) {
        auto result = std::string{};
        for (const auto character : text) {
            if (!std::isspace(static_cast<unsigned char>(character))) result += character;
        }
        return result;
    };
    if (without_spaces(source.substr(0, equals)) != without_spaces(base_type)) {
        return std::nullopt;
    }
    auto value = source.substr(std::min(source.find_first_not_of(" \t\n", equals + 1), source.size()));
    while (!value.empty() && (std::isspace(static_cast<unsigned char>(value.back())) ||
                              value.back() == ';')) {
        value.pop_back();
    }
    return value;
}

// Constructors and assignments set each base, in order, from a leading
// `Base = value;` statement or from `that` in a copy or move. Otherwise a
// constructor uses the base's declared initializer.
auto apply_base_initializers(ir_declaration& type) -> void {
    auto bases = std::vector<const ir_declaration*>{};
    for (const auto& member : type.members) {
        if (is_base_member(member) || !member.base_helper.empty()) bases.push_back(&member);
    }
    if (bases.empty()) return;
    for (auto& function : type.members) {
        if (function.kind != declaration_kind::function) continue;
        const auto has_that = that_parameter(function) != nullptr;
        const auto is_assignment = cppfrontier::is_assignment(function);
        if (!is_assignment && !initializes_members(function)) continue;
        const auto* statements = function.function_statements
            ? &function.function_statements->statements : nullptr;
        auto initializers = std::vector<ir_declaration::member_initializer>{};
        auto next = std::size_t{0};
        for (const auto* base : bases) {
            // Assignments set a helper-held member directly through its using-declaration.
            const auto is_helper = !base->base_helper.empty();
            if (statements != nullptr && next < statements->size()) {
                if (auto value = base_initializer(*(*statements)[next],
                                                  is_helper ? base->name : base->type.text)) {
                    initializers.push_back({is_assignment && is_helper ? base->name
                                                                       : base_type(*base),
                                            std::move(*value), !(is_assignment && is_helper)});
                    ++next;
                    continue;
                }
            }
            if (is_assignment && is_helper) continue;
            if (has_that) initializers.push_back({base_type(*base), "that", true});
            else if (!is_assignment && !is_helper && !base->body.empty()) {
                initializers.push_back({base->type.text, base->body.source(), true});
            }
        }
        function.initializer_statements = next;
        auto& target = is_assignment ? function.that_assignments : function.member_initializers;
        target.insert(target.begin(), initializers.begin(), initializers.end());
    }
}

// The value of a leading `name = value;` or `this.name = value;` statement, or nullopt.
auto member_initializer(const statement& statement, std::string_view name)
    -> std::optional<std::string> {
    if (statement.kind != statement_kind::expression) return std::nullopt;
    const auto& text = statement.as<statement::expression_form>().text;
    const auto& tokens = text.tree().tokens;
    const auto offset = tokens.size() > 2 && tokens[0].text == "this" && tokens[1].text == "."
        ? std::size_t{2} : std::size_t{0};
    if (tokens.size() < offset + 3 || tokens[offset].text != name ||
        tokens[offset + 1].text != "=") {
        return std::nullopt;
    }
    auto value = std::string_view{text.source()}.substr(text.source().find('=') + 1);
    while (!value.empty() && (std::isspace(static_cast<unsigned char>(value.front())))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (std::isspace(static_cast<unsigned char>(value.back())) ||
                              value.back() == ';')) {
        value.remove_suffix(1);
    }
    return std::string{value};
}

// As upstream, each constructor and `operator=` assignment sets the bases and
// data members in declaration order: from a leading `name = value;` (or
// `Base = value;`) statement, else from `that` in a copy or move, else from the
// declared initializer, which an assignment sets explicitly and a constructor
// leaves to Cpp1. A member set by none of these is diagnosed.
auto apply_member_initialization(ir_declaration& type, std::vector<diagnostic>& diagnostics)
    -> void {
    auto objects = std::vector<const ir_declaration*>{};
    for (const auto& member : type.members) {
        if (is_instance_object(member)) objects.push_back(&member);
    }
    for (auto& function : type.members) {
        if (function.kind != declaration_kind::function || !function.cpp1_body.empty()) {
            continue;
        }
        const auto has_that = that_parameter(function) != nullptr;
        const auto is_assignment = cppfrontier::is_assignment(function);
        if (!is_assignment && !initializes_members(function)) continue;
        // An expression body is a single statement.
        if (!function.function_statements && !function.body.empty()) {
            auto body = std::make_shared<function_body>();
            auto& statement = body->statements.emplace_back(
                std::make_unique<cppfrontier::statement>(statement_kind::expression));
            statement->span = function.span;
            statement->as<statement::expression_form>().text = std::move(function.body);
            function.function_statements = std::move(body);
            function.body = {};
        }
        const auto* statements = function.function_statements
            ? &function.function_statements->statements : nullptr;
        auto& target = is_assignment ? function.that_assignments : function.member_initializers;
        auto initializers = std::vector<ir_declaration::member_initializer>{};
        auto next = std::size_t{0};
        // Leading `out` parameter initializations are kept with the next member's.
        const auto out_initialization = [&]() -> const statement* {
            if (statements == nullptr || next >= statements->size()) return nullptr;
            const auto& candidate = *(*statements)[next];
            for (const auto& parameter : function.parameters) {
                if (parameter.passing == passing_style::out && parameter.name != "this" &&
                    member_initializer(candidate, parameter.name)) {
                    return &candidate;
                }
            }
            return nullptr;
        };
        auto initialized = std::size_t{0};
        for (const auto* object : objects) {
            if (std::any_of(target.begin(), target.end(),
                            [&](const auto& set) { return set.member == object->name; })) {
                continue;
            }
            while (const auto* statement = out_initialization()) {
                initializers.push_back({{}, {}, false, statement->id});
                ++next;
            }
            // An assignment sets a helper-held member directly through its using-declaration.
            const auto is_helper = !object->base_helper.empty();
            const auto as_base = is_base_member(*object) || (is_helper && !is_assignment);
            const auto* statement = statements != nullptr && next < statements->size()
                ? (*statements)[next].get() : nullptr;
            const auto value = statement == nullptr ? std::nullopt
                : is_base_member(*object) ? base_initializer(*statement, object->type.text)
                : member_initializer(*statement, object->name);
            if (value) {
                initialized = ++next;
                if (*value != "_") {
                    initializers.push_back({as_base ? base_type(*object) : object->name,
                                            *value, as_base, statement->id});
                }
            }
            else if (has_that) {
                initializers.push_back(as_base
                    ? ir_declaration::member_initializer{base_type(*object), "that", true}
                    : ir_declaration::member_initializer{
                          object->name, "that." + cpp1_identifier(object->name)});
            }
            else if (!object->body.empty()) {
                if (is_assignment) {
                    // A parenthesized initializer becomes a typed temporary.
                    const auto& value = object->body.source();
                    initializers.push_back(is_base_member(*object)
                        ? ir_declaration::member_initializer{object->type.text, value, true}
                        : ir_declaration::member_initializer{object->name,
                              value.starts_with('(')
                                  ? ":" + object->type.text + " = " + value : value});
                }
                else if (is_base_member(*object)) {
                    initializers.push_back({object->type.text, object->body.source(), true});
                }
            }
            else if (statements != nullptr && !function.is_generated && !object->is_generated &&
                     !is_helper) {
                // A base is initialized by its type name; a metafunction
                // initializes the members it generates.
                const auto& base_or_name =
                    is_base_member(*object) ? object->type.text : object->name;
                const auto position = std::to_string(object->span.begin.line) + ":" +
                    std::to_string(object->span.begin.column);
                diagnostics.push_back({
                    "member-not-initialized", diagnostic_severity::error,
                    statement != nullptr ? statement->span : function.span,
                    object->name == anonymous_object_name(object->span.begin)
                        ? "in operator=, the type scope object '_' at " + position +
                            " needs a default initializer, since no statement can name it"
                        : "in operator=, expected '" + base_or_name + " = ...' initialization "
                          "statement (because type scope object '" + base_or_name +
                          "' does not have a default initializer)"});
            }
        }
        while (!initializers.empty() && initializers.back().member.empty()) {
            initializers.pop_back();
        }
        // A base is set only by a leading statement, in declaration order.
        for (auto index = initialized; statements != nullptr && !function.is_generated &&
                 index < statements->size(); ++index) {
            for (const auto* object : objects) {
                if (is_base_member(*object) &&
                    base_initializer(*(*statements)[index], object->type.text)) {
                    diagnostics.push_back({"base-initialization-order",
                        diagnostic_severity::error, (*statements)[index]->span,
                        "in operator=, '" + object->type.text + " = ...' must be among the "
                        "statements that begin the body, which set the bases and data members "
                        "in declaration order"});
                }
            }
        }
        function.initializer_statements = initialized;
        target.insert(target.begin(), initializers.begin(), initializers.end());
    }
}

// The mentions of `that` in a function: the data members read as `that.name`,
// and whether `that` is also used otherwise. The captures of the function
// expressions in `expressions` are mentions where they appear.
struct that_mentions final {
    const std::vector<function_expression>& expressions;
    std::set<std::string, std::less<>> members;
    bool whole = false;
};

auto add_that_mentions(const expression_text& parsed, that_mentions& output) -> void {
    const auto& tokens = parsed.tokens;
    const auto text = [&](std::size_t index) {
        return index < tokens.size() ? std::string_view{tokens[index].text} : std::string_view{};
    };
    for (auto index = std::size_t{0}; index < tokens.size(); ++index) {
        if (tokens[index].kind == expression_token_kind::string) {
            for (const auto& expression : interpolated_expressions(tokens[index].text)) {
                add_that_mentions(parse_expression_text(expression), output);
            }
            continue;
        }
        if (is_function_expression_name(text(index))) {
            const auto found = std::find_if(output.expressions.begin(), output.expressions.end(),
                [&](const auto& value) { return value.name == text(index); });
            if (found == output.expressions.end()) continue;
            for (const auto& capture : found->captures) {
                add_that_mentions(capture.expression.tree(), output);
            }
            continue;
        }
        const auto previous = index > 0 ? text(index - 1) : std::string_view{};
        if (text(index) != "that" || previous == "." || previous == "::" || previous == "->") {
            continue;
        }
        if (text(index + 1) == "." && index + 2 < tokens.size() &&
            tokens[index + 2].kind == expression_token_kind::identifier && text(index + 3) != "(") {
            output.members.emplace(text(index + 2));
        }
        else {
            output.whole = true;
        }
    }
}

auto add_that_mentions(const statement_list& statements, that_mentions& output) -> void {
    ir_detail::visit_statements(statements, [&](const statement& value) {
        std::visit([&](const auto& form) {
            using form_type = std::remove_cvref_t<decltype(form)>;
            if constexpr (std::is_same_v<form_type, statement::expression_form> ||
                          std::is_same_v<form_type, statement::declaration_form>) {
                add_that_mentions(form.text.tree(), output);
            }
            else if constexpr (std::is_same_v<form_type, statement::destructuring_form>) {
                add_that_mentions(form.initializer.tree(), output);
            }
            else if constexpr (std::is_same_v<form_type, statement::block_form>) {
                for (const auto& parameter : form.parameters) {
                    add_that_mentions(parameter.initializer.tree(), output);
                }
            }
            else if constexpr (std::is_same_v<form_type, statement::if_form>) {
                add_that_mentions(form.condition.tree(), output);
            }
            else if constexpr (std::is_same_v<form_type, statement::loop_form>) {
                add_that_mentions(form.condition.tree(), output);
                add_that_mentions(form.next.tree(), output);
            }
            else if constexpr (std::is_same_v<form_type, statement::contract_form>) {
                add_that_mentions(form.condition.tree(), output);
                add_that_mentions(form.message.tree(), output);
                for (const auto& flag : form.flags) add_that_mentions(flag.tree(), output);
            }
        }, value.form);
    });
}

// A move function's generated `member = that.member` and `Base = that` move from
// `that`, as a Cpp1 defaulted move does, unless the function also reads that
// part of `that`: a mention of `that.member` keeps that member's copy, and any
// other use of `that` keeps all of them.
auto apply_moves_from_that(const ir_program& program, ir_declaration& type) -> void {
    auto own_members = std::set<std::string, std::less<>>{};
    for (const auto& member : type.members) {
        if (is_data_member(member)) own_members.insert(member.name);
    }
    for (auto& function : type.members) {
        const auto* that = function.kind == declaration_kind::function
            ? that_parameter(function) : nullptr;
        if (that == nullptr || that->passing != passing_style::move) continue;
        auto mentions = that_mentions{program.function_expressions};
        if (function.function_statements) {
            add_that_mentions(function.function_statements->statements, mentions);
        }
        for (const auto& contract : function.contracts) {
            add_that_mentions(contract.condition.tree(), mentions);
            add_that_mentions(contract.message.tree(), mentions);
            for (const auto& flag : contract.flags) add_that_mentions(flag.tree(), mentions);
        }
        if (mentions.whole) continue;
        const auto reads_only_own_members = std::all_of(mentions.members.begin(),
            mentions.members.end(), [&](const auto& name) { return own_members.contains(name); });
        for (auto* list : {&function.member_initializers, &function.that_assignments}) {
            for (auto& initializer : *list) {
                if (initializer.source) continue;
                if (initializer.is_base && initializer.value == "that") {
                    // A data member held in a helper base is read by its own name.
                    const auto helper = std::find_if(type.members.begin(), type.members.end(),
                        [&](const auto& member) {
                            return !member.base_helper.empty() &&
                                member.base_helper == initializer.member;
                        });
                    initializer.move_that = helper != type.members.end()
                        ? !mentions.members.contains(helper->name) : reads_only_own_members;
                }
                else if (!initializer.is_base && !mentions.members.contains(initializer.member) &&
                         initializer.value == "that." + cpp1_identifier(initializer.member)) {
                    initializer.value = "cpp2::move(that)." + cpp1_identifier(initializer.member);
                }
            }
        }
    }
}

auto make_virtual_destructor(const ir_declaration& type) -> ir_declaration {
    auto destructor = ir_declaration{};
    destructor.kind = declaration_kind::function;
    destructor.name = "operator=";
    destructor.scope = declaration_scope::type_scope;
    destructor.access = access_kind::public_access;
    destructor.span = type.span;
    destructor.is_generated = true;
    auto this_parameter = parameter{passing_style::move, parse_type(type.name), "this"};
    this_parameter.this_specifier = virtual_specifier::virtual_function;
    destructor.parameters = {std::move(this_parameter)};
    destructor.special_member = ir_declaration::special_member_kind::destructor;
    destructor.special_member_owner = type.name;
    return destructor;
}

auto is_copy_or_move(const ir_declaration& member) -> bool {
    return (is_constructor(member) || is_assignment(member)) && that_parameter(member) != nullptr;
}

// `@interface`: public pure virtual functions only, a default constructor,
// protected copy and move, and a virtual destructor. With `default_bodies`
// (experiment `interface-defaults`), a function with a body is virtual with
// that body as its default, and the default constructor is protected when no
// function is pure, so the type is still not instantiable.
auto apply_interface(ir_program& program, ir_declaration& declaration,
                     std::vector<diagnostic>& diagnostics, bool default_bodies) -> void {
    const auto error = [&](const ir_declaration& member, std::string code, std::string message) {
        diagnostics.push_back({std::move(code), diagnostic_severity::error, member.span,
                               std::move(message)});
    };
    auto has_destructor = false;
    auto has_pure_function = false;
    for (auto& member : declaration.members) {
        if (is_instance_object(member)) {
            error(member, "interface-data-member", "interfaces may not contain data objects");
            continue;
        }
        if (member.kind != declaration_kind::function) continue;
        const auto has_body = !member.body.empty() || member.function_statements;
        if (is_copy_or_move(member)) {
            error(member, "interface-copy-or-move",
                  "interfaces may not copy or move; consider a virtual clone() instead");
        }
        // A constructor is not virtual, so it cannot be a default.
        if (has_body && (!default_bodies || is_constructor(member))) {
            error(member, "interface-function-body", "interface functions must not have a "
                  "function body; remove the '=' initializer");
        }
        if (member.access == access_kind::private_access ||
            member.access == access_kind::protected_access) {
            error(member, "interface-function-access", "interface functions must be public");
        }
        member.access = access_kind::public_access;
        // An interface has no base, so `override` and `final` also mean virtual.
        if (!member.parameters.empty() && member.parameters.front().name == "this") {
            member.parameters.front().this_specifier = virtual_specifier::virtual_function;
        }
        else if (receiver(member) == nullptr && (default_bodies || !has_body)) {
            error(member, "interface-function-this",
                  "interface functions must be virtual, with a 'this' parameter");
        }
        const auto is_destructor = member.special_member ==
            ir_declaration::special_member_kind::destructor;
        has_destructor |= is_destructor;
        has_pure_function |= !has_body && !is_destructor && !is_constructor(member);
    }
    auto default_constructor = make_defaulted_default_constructor(declaration);
    if (default_bodies && !has_pure_function) {
        default_constructor.access = access_kind::protected_access;
    }
    add_member(program, declaration, std::move(default_constructor));
    auto copy = make_copy_constructor(declaration);
    auto move = make_move_constructor(declaration);
    auto copy_assignment = make_copy_assignment(declaration);
    for (auto* member : {&copy, &move, &copy_assignment}) {
        member->access = access_kind::protected_access;
    }
    add_member(program, declaration, std::move(copy));
    add_member(program, declaration, std::move(move));
    add_member(program, declaration, std::move(copy_assignment));
    if (!has_destructor) add_member(program, declaration, make_virtual_destructor(declaration));
}

// `@polymorphic_base`: no copy or move, and a destructor that is public and
// virtual (generated when missing) or protected and nonvirtual. A function
// without an access specifier is public, as upstream marks it.
auto apply_polymorphic_base(ir_program& program, ir_declaration& declaration,
                            std::vector<diagnostic>& diagnostics) -> void {
    auto has_destructor = false;
    for (auto& member : declaration.members) {
        if (member.kind == declaration_kind::function &&
            member.access == access_kind::default_access) {
            member.access = access_kind::public_access;
        }
        if (is_copy_or_move(member)) {
            diagnostics.push_back({
                "polymorphic-base-copy-or-move", diagnostic_severity::error, member.span,
                "polymorphic base types may not copy or move; consider a virtual clone() "
                "instead"});
        }
        if (member.special_member != ir_declaration::special_member_kind::destructor) continue;
        has_destructor = true;
        const auto is_protected = member.access == access_kind::protected_access;
        if (member.access == access_kind::private_access ||
            is_virtual_function(member) == is_protected) {
            diagnostics.push_back({
                "polymorphic-base-destructor", diagnostic_severity::error, member.span,
                "a polymorphic base type destructor must be public and virtual, or protected "
                "and nonvirtual"});
        }
    }
    if (!has_destructor) add_member(program, declaration, make_virtual_destructor(declaration));
}

auto apply_copy_constructible(ir_program& program, ir_declaration& declaration,
                              std::vector<diagnostic>& diagnostics) -> void {
    const auto has_copy = std::any_of(declaration.members.begin(), declaration.members.end(),
        is_copy_constructor);
    const auto has_move = std::any_of(declaration.members.begin(), declaration.members.end(),
        is_move_constructor);
    if (!has_copy && has_move) {
        diagnostics.push_back({
            "copy-constructible-partial", diagnostic_severity::error, declaration.span,
            "this type is partially copy/move constructible - when you provide the "
            "(out this, move that) operator= signature, you must also provide the one "
            "with the general signature (out this, that)"});
        return;
    }
    if (has_copy || has_move) return;
    auto copy = make_copy_constructor(declaration);
    auto move = make_move_constructor(declaration);
    add_member(program, declaration, std::move(copy));
    add_member(program, declaration, std::move(move));
}

// The comparison category required by an ordering metaclass, or empty.
auto ordering_of(std::string_view metaclass) -> std::string_view {
    if (metaclass == "ordered" || metaclass == "value") return "strong_ordering";
    if (metaclass == "weakly_ordered" || metaclass == "weakly_ordered_value") {
        return "weak_ordering";
    }
    if (metaclass == "partially_ordered" || metaclass == "partially_ordered_value") {
        return "partial_ordering";
    }
    return {};
}

// `@ordered` and its variants: a user-written `operator<=>` must return the
// category; otherwise a defaulted memberwise one is generated.
auto apply_ordered(ir_program& program, ir_declaration& declaration, std::string_view ordering,
                   std::vector<diagnostic>& diagnostics) -> void {
    auto has_spaceship = false;
    for (const auto& member : declaration.members) {
        if (member.kind != declaration_kind::function || member.name != "operator<=>") continue;
        has_spaceship = true;
        // As upstream, any return type spelled with the category is accepted.
        if (member.type.text.find(ordering) == std::string::npos) {
            diagnostics.push_back({
                "ordered-return-type", diagnostic_severity::error, member.span,
                "operator<=> must return std::" + std::string{ordering}});
        }
    }
    if (has_spaceship) return;
    auto spaceship = ir_declaration{};
    spaceship.kind = declaration_kind::function;
    spaceship.name = "operator<=>";
    spaceship.type = parse_type("std::" + std::string{ordering});
    spaceship.scope = declaration_scope::type_scope;
    spaceship.access = access_kind::public_access;
    spaceship.span = declaration.span;
    spaceship.is_generated = true;
    spaceship.parameters = {
        parameter{passing_style::in, parse_type(declaration.name), "this"},
        parameter{passing_style::in, parse_type(declaration.name), "that"},
    };
    add_member(program, declaration, std::move(spaceship));
}

// `@hashable`: `hash()` combines the hash of each base and data member.
auto apply_hashable(ir_program& program, ir_declaration& declaration,
                    std::vector<diagnostic>& diagnostics) -> void {
    auto body = std::make_shared<function_body>();
    const auto add_statement = [&](statement_kind kind, std::string text) {
        auto& added = body->statements.emplace_back(std::make_unique<statement>(kind));
        added->span = declaration.span;
        added->as<statement::expression_form>().text = parsed_expression{std::move(text)};
    };
    auto result = std::make_unique<statement>(statement_kind::declaration_statement);
    result->span = declaration.span;
    auto& ret = result->as<statement::declaration_form>().declaration.emplace();
    ret.name = "ret";
    ret.type = parse_type("size_t");
    ret.initializer = parsed_expression{"0"};
    body->statements.push_back(std::move(result));
    for (const auto& member : declaration.members) {
        if (!is_instance_object(member)) continue;
        add_statement(statement_kind::expression, "cpp2::hash_combine(ret, " +
            (is_base_member(member) ? member.type.text + "::hash()"
                                    : "std::hash<" + member.type.text + ">()(" + member.name + ")") +
            ")");
    }
    if (body->statements.size() == 1) {
        diagnostics.push_back({
            "hashable-without-members", diagnostic_severity::error, declaration.span,
            "a hashable type must have at least one data member"});
        return;
    }
    add_statement(statement_kind::return_statement, "ret");
    auto hash = ir_declaration{};
    hash.kind = declaration_kind::function;
    hash.name = "hash";
    hash.type = parse_type("size_t");
    hash.scope = declaration_scope::type_scope;
    hash.access = access_kind::public_access;
    hash.span = declaration.span;
    hash.is_generated = true;
    hash.parameters = {parameter{passing_style::in, parse_type(declaration.name), "this"}};
    hash.function_statements = std::move(body);
    add_member(program, declaration, std::move(hash));
}

// What is generated for a type after its metafunctions have run.
enum class member_generation : std::uint8_t {
    all,             // the value set, implicit special members and member initialization
    initialization,  // member initialization only, for the Cpp1 defaults of `@cpp1_rule_of_zero` and `@enum`
    bases,           // base initializers only, for `@struct`
    none,            // nothing: the concept a `@trait` becomes, `@impl`/`@given` markers, the alias a `@unit` becomes
};

// The members every type gets after its metafunctions, unless one of them
// leaves the special member functions to Cpp1.
auto finish_type(ir_program& program, ir_declaration& declaration,
                 std::vector<diagnostic>& diagnostics) -> void {
    if (!has_metaclass(declaration, "singleton")) generate_value_set(program, declaration);
    if (has_metaclass(declaration, "interface")) {
        for (auto& member : declaration.members) {
            if (is_copy_or_move(member)) member.access = access_kind::protected_access;
        }
    }
    if (is_value_type(declaration) && !has_assignment(declaration)) {
        add_member(program, declaration, make_copy_assignment(declaration));
    }
    apply_implicit_special_members(program, declaration);
    apply_member_initialization(declaration, diagnostics);
    apply_moves_from_that(program, declaration);
    mark_postfix_counterparts(declaration);
}

// `@cpp1_rule_of_zero`: no copy, move or destructor functions, and nothing
// generated, so Cpp1 provides the special member functions.
auto apply_cpp1_rule_of_zero(const ir_declaration& declaration,
                             std::vector<diagnostic>& diagnostics) -> void {
    for (const auto& member : declaration.members) {
        if (is_copy_or_move(member) ||
            member.special_member == ir_declaration::special_member_kind::destructor) {
            diagnostics.push_back({
                "rule-of-zero-special-member", diagnostic_severity::error, member.span,
                "the rule of zero requires no copy/move/destructor functions"});
        }
    }
}

// `@linear`: a move-only type whose values a function must consume exactly
// once, which `analyze_safety` checks. A copy `operator=` is rejected; a
// missing move constructor or move assignment is generated memberwise, and
// the copy functions are deleted.
auto apply_linear(ir_program& program, ir_declaration& declaration,
                  std::vector<diagnostic>& diagnostics) -> void {
    const auto declared = declared_value_set_of(declaration);
    for (const auto* copy : {declared.copy_constructor, declared.copy_assignment}) {
        if (copy == nullptr) continue;
        diagnostics.push_back({"linear-copy", diagnostic_severity::error, copy->span,
            "a @linear type cannot be copied: operator= must take 'move that', not 'that'"});
    }
    if (declared.move_constructor == nullptr) {
        add_generated_members(program, declaration, "operator=: (out this, move that) = { }",
                              diagnostics);
    }
    if (declared.move_assignment == nullptr) {
        add_generated_members(program, declaration, "operator=: (inout this, move that) = { }",
                              diagnostics);
    }
    auto [copy_constructor, copy_assignment] = make_deleted_copy_members(declaration);
    add_member(program, declaration, std::move(copy_constructor));
    add_member(program, declaration, std::move(copy_assignment));
}

// `@noncopyable`: no copy or move functions, which the type then lacks.
auto apply_noncopyable(const ir_declaration& declaration, std::vector<diagnostic>& diagnostics)
    -> void {
    for (const auto& member : declaration.members) {
        if (is_copy_or_move(member)) {
            diagnostics.push_back({
                "noncopyable-copy-or-move", diagnostic_severity::error, member.span,
                "a @noncopyable type must not have operator= with this and that parameters"});
        }
    }
}

// `@encapsulated`: no public data members.
auto apply_encapsulated(const ir_declaration& declaration, std::vector<diagnostic>& diagnostics)
    -> void {
    for (const auto& member : declaration.members) {
        if (is_data_member(member) && member.access == access_kind::public_access) {
            diagnostics.push_back({
                "encapsulated-public-data", diagnostic_severity::error, member.span,
                "an @encapsulated type must not have public data members"});
        }
    }
}

// What a metafunction applies to: `type`, declared in `parent` (null at file
// scope), and named `name` there.
struct metafunction_context final {
    ir_program& program;
    ir_declaration& type;
    ir_declaration* parent;
    std::string_view name;
    std::vector<diagnostic>& diagnostics;
    // Lowered by a metafunction that leaves members to Cpp1.
    member_generation generation = member_generation::all;
    // Members to add to an enclosing namespace once every type is reflected.
    std::vector<std::pair<ir_declaration*, ir_declaration>>& namespace_members;
    // Declarations to insert after the declaration with the given symbol, in
    // its scope, once every type is reflected.
    std::vector<std::pair<symbol_id, surrounding_declarations>>& surrounding;
    // The `@trait` types of the program, for `@given`.
    const trait_table& traits;
    const experiment_set& experiments;
    // The position of the metafunction being applied in `type.metaclasses`.
    std::size_t index = 0;
    // The names of the functions earlier `@delegate`s of `type` forwarded.
    std::vector<std::string> delegated;

    [[nodiscard]] auto arguments() const -> std::vector<std::string> {
        return index < type.metaclass_arguments.size() ? type.metaclass_arguments[index]
                                                       : std::vector<std::string>{};
    }

    auto lower(member_generation value) -> void { generation = std::max(generation, value); }
};

// The underlying type of a `@newtype<U>`, `@quantity<U>` or `@unit<U, ...>`
// declaration, or empty with a diagnostic.
auto underlying_type_of(const ir_declaration& declaration, std::string_view metaclass,
                        std::vector<diagnostic>& diagnostics) -> std::string {
    const auto* arguments = metaclass_arguments_of(declaration, metaclass);
    auto argument = arguments == nullptr || arguments->empty() ? std::string{}
                                                               : arguments->front();
    if (argument.empty()) {
        diagnostics.push_back({
            "newtype-without-underlying-type", diagnostic_severity::error, declaration.span,
            "'@" + std::string{metaclass} + "' needs the underlying type as its argument, as in '@" +
                std::string{metaclass} + "<i64>'"});
    }
    return argument;
}

auto is_distinct_type_metafunction(std::string_view name) -> bool {
    return name == "newtype" || name == "quantity" || name == "unit";
}

// `@newtype`, `@quantity` and `@unit` generate the whole type, so they combine
// only with `@hashable` and `@delegate`, which add functions over its members,
// and `@print`; a `@unit` type, an alias, with `@print` only. The first of
// them reports each other metafunction; false for each of them then.
auto check_distinct_type_composition(metafunction_context& context) -> bool {
    const auto& metaclasses = context.type.metaclasses;
    const auto first = static_cast<std::size_t>(
        std::find_if(metaclasses.begin(), metaclasses.end(),
                     [](const std::string& name) { return is_distinct_type_metafunction(name); }) -
        metaclasses.begin());
    const auto is_unit = metaclasses[first] == "unit";
    auto valid = true;
    for (auto index = std::size_t{0}; index < metaclasses.size(); ++index) {
        const auto& other = metaclasses[index];
        if (index == first || other == "print" ||
            (!is_unit && (other == "hashable" || other == "delegate"))) {
            continue;
        }
        valid = false;
        if (context.index != first) continue;
        context.diagnostics.push_back({
            "newtype-composition", diagnostic_severity::error, context.type.span,
            "'@" + other + "' cannot be combined with '@" + metaclasses[first] + "', which " +
                (is_unit ? "makes the type an alias; only @print can"
                         : "generates the type's members; only @hashable, @delegate and @print "
                           "can")});
    }
    return valid;
}

// Appends to `path` the declarations in `declarations` that `target` is
// nested in, outermost first; true if `target` is found.
auto find_enclosing(const std::vector<ir_declaration>& declarations, const ir_declaration& target,
                    std::vector<const ir_declaration*>& path) -> bool {
    for (const auto& declaration : declarations) {
        if (&declaration == &target) return true;
        path.push_back(&declaration);
        if (find_enclosing(declaration.members, target, path)) return true;
        path.pop_back();
    }
    return false;
}

// Why namespace scope cannot name the type `declaration`, nested in the
// types `enclosing` (outermost first), as a type to deduce from; empty if it
// can.
auto unnamed_at_namespace_scope(const ir_declaration& declaration,
                                const std::vector<const ir_declaration*>& enclosing)
    -> std::string {
    for (auto index = std::size_t{0}; index < enclosing.size(); ++index) {
        const auto& outer = *enclosing[index];
        const auto& inner = index + 1 < enclosing.size() ? *enclosing[index + 1] : declaration;
        if (!outer.template_parameters.empty()) {
            return "it is nested in the template '" + outer.name +
                "', whose template arguments 'k * q' cannot deduce";
        }
        if (inner.access == access_kind::private_access ||
            inner.access == access_kind::protected_access) {
            return "'" + inner.name + "' is a " +
                (inner.access == access_kind::private_access ? "private" : "protected") +
                " member of '" + outer.name + "'";
        }
    }
    return {};
}

// `@newtype<U>`: a distinct type holding one `U` in the private member
// `value`, constructed from a `U` and converted back explicitly (`u as T`,
// `t as U`), copyable, and compared with the underlying type's comparison;
// nothing converts implicitly, so it has no assignment from a `U`.
// `@quantity<U>` adds same-type `+`, `-`, `+=`, `-=`, unary `-`, scaling by a
// `U` and the ratio of two quantities; `k * q` is a function at namespace
// scope, which must be able to name the type. Other operators are the user's,
// as free functions.
auto apply_newtype(metafunction_context& context) -> void {
    auto& declaration = context.type;
    auto& diagnostics = context.diagnostics;
    const auto metaclass = context.name;
    if (!check_distinct_type_composition(context)) return;
    const auto underlying = underlying_type_of(declaration, metaclass, diagnostics);
    if (underlying.empty()) return;
    const auto error = [&](std::string code, std::string message) {
        diagnostics.push_back({std::move(code), diagnostic_severity::error, declaration.span,
                               std::move(message)});
    };
    if (std::any_of(declaration.members.begin(), declaration.members.end(),
                    [](const ir_declaration& member) { return member.name == "value"; })) {
        return error("newtype-member-name", "'@" + std::string{metaclass} + "' generates the "
                     "member 'value' holding the underlying value, so the type cannot declare "
                     "its own");
    }
    const auto is_quantity = metaclass == "quantity";
    // The types `declaration` is nested in, outermost first.
    auto enclosing = std::vector<const ir_declaration*>{};
    if (is_quantity) {
        find_enclosing(context.program.declarations, declaration, enclosing);
        std::erase_if(enclosing, [](const ir_declaration* outer) {
            return outer->kind == declaration_kind::namespace_node;
        });
        if (const auto reason = unnamed_at_namespace_scope(declaration, enclosing); !reason.empty()) {
            return error("quantity-scope", "the 'k * q' operator of a @quantity type is declared "
                         "at namespace scope, which cannot name '" + declaration.name + "': " +
                         reason);
        }
    }
    const auto& name = declaration.name;
    auto text = "value: " + underlying + ";\n"
        "cpp2_newtype_rep: type == " + underlying + ";\n"
        "operator=: (out this, v: " + underlying + ") = { value = v; }\n"
        "operator=: (out this, that) = { value = that.value; }\n"
        "operator<=>: (this, that) -> _;\n";
    if (is_quantity) {
        text += "operator+: (this, that) -> " + name + " = " + name + "(value + that.value);\n"
            "operator-: (this, that) -> " + name + " = " + name + "(value - that.value);\n"
            "operator-: (this) -> " + name + " = " + name + "(-value);\n"
            "operator+=: (inout this, that) = { value += that.value; }\n"
            "operator-=: (inout this, that) = { value -= that.value; }\n"
            "operator*: (this, k: " + underlying + ") -> " + name + " = " + name + "(value * k);\n"
            "operator/: (this, k: " + underlying + ") -> " + name + " = " + name + "(value / k);\n"
            "operator*=: (inout this, k: " + underlying + ") = { value *= k; }\n"
            "operator/=: (inout this, k: " + underlying + ") = { value /= k; }\n"
            "operator/: (this, that) -> " + underlying + " = value / that.value;\n";
    }
    add_generated_members(context.program, declaration, text, diagnostics);
    for (auto& member : declaration.members) {
        member.is_explicit = member.is_generated && member.special_member ==
            ir_declaration::special_member_kind::converting_constructor;
    }
    // Cpp2 spells neither a conversion function nor a friend. `as` cannot use
    // an explicit conversion function, so `t as U` is a hidden friend `as<U>`
    // that `cpp2::impl::as_` finds by argument-dependent lookup.
    const auto cpp1_underlying = render_cpp1_type(parse_type(underlying));
    auto conversions = ir_declaration{};
    conversions.kind = declaration_kind::function;
    conversions.name = "operator " + underlying;
    conversions.scope = declaration_scope::type_scope;
    conversions.access = access_kind::public_access;
    conversions.span = declaration.span;
    conversions.is_generated = true;
    conversions.cpp1_text = "constexpr explicit operator " + cpp1_underlying +
        "() const { return value; }\n"
        "template<typename C> requires std::same_as<C, " + cpp1_underlying +
        "> friend constexpr auto as(" + name + " const& x) -> " + cpp1_underlying +
        " { return x.value; }";
    add_member(context.program, declaration, std::move(conversions));
    if (!is_quantity) return;
    // `operator*: <Ts> (k: Q<Ts>::cpp2_newtype_rep, q: Q<Ts>) -> Q<Ts> = q * k;`
    // for the type `Q`, nested as `A::B::Q` in the types `A` and `B`, after
    // the outermost one, where argument-dependent lookup finds it. The template
    // arguments are deduced from `q` only.
    auto self = std::string{};
    for (const auto* outer : enclosing) self += outer->name + "::";
    self += name;
    for (const auto& parameter : declaration.template_parameters) {
        self += (&parameter == &declaration.template_parameters.front() ? "<" : ", ") +
            parameter.name + (parameter.is_variadic ? "..." : "");
    }
    if (!declaration.template_parameters.empty()) self += ">";
    auto scaled = ir_declaration{};
    scaled.kind = declaration_kind::function;
    scaled.name = "operator*";
    scaled.scope = declaration_scope::namespace_scope;
    scaled.span = declaration.span;
    scaled.is_generated = true;
    scaled.template_parameters = declaration.template_parameters;
    scaled.type = parse_type(self);
    auto scalar = parse_type(self + "::cpp2_newtype_rep");
    auto template_names = std::vector<std::string>{};
    for (const auto& parameter : declaration.template_parameters) {
        template_names.push_back(parameter.name);
    }
    mark_dependent_names(scalar, template_names);
    scaled.parameters = {parameter{passing_style::in, std::move(scalar), "k"},
                         parameter{passing_style::in, parse_type(self), "q"}};
    scaled.body = parsed_expression{"q * k"};
    auto following = surrounding_declarations{};
    following.after.push_back(std::move(scaled));
    context.surrounding.emplace_back(
        enclosing.empty() ? declaration.symbol : enclosing.front()->symbol, std::move(following));
}

// `@unit<U, L, M, T, I, Th, N, J>`: an alias of `cpp2::quantity<U, cpp2::dims<...>>`
// with the exponents given, in the order of the SI base dimensions, and the
// rest zero; `*` and `/` of quantities add and subtract exponents. The type
// cannot have members of its own, and is distinguished by its underlying type
// and exponents only, so two non-template `@unit` types with the same ones,
// which would name one type, are rejected.
auto apply_unit(metafunction_context& context) -> void {
    auto& declaration = context.type;
    auto& diagnostics = context.diagnostics;
    context.lower(member_generation::none);
    if (!check_distinct_type_composition(context)) return;
    const auto underlying = underlying_type_of(declaration, "unit", diagnostics);
    if (underlying.empty()) return;
    if (!declaration.members.empty()) {
        diagnostics.push_back({
            "unit-with-members", diagnostic_severity::error, declaration.span,
            "a @unit type is an alias of cpp2::quantity and cannot declare members; "
            "use @quantity for a type with members of its own"});
        return;
    }
    constexpr auto dimension_count = 7;
    auto exponents = std::vector<std::string>{};
    const auto& arguments = *metaclass_arguments_of(declaration, "unit");
    for (auto index = std::size_t{1}; index < arguments.size(); ++index) {
        const auto& exponent = arguments[index];
        auto value = 0;
        const auto end = exponent.data() + exponent.size();
        const auto [parsed, error] = std::from_chars(exponent.data(), end, value);
        if (exponents.size() == dimension_count || error != std::errc{} || parsed != end) {
            diagnostics.push_back({
                "unit-dimension", diagnostic_severity::error, declaration.span,
                "'@unit' takes the underlying type and up to " + std::to_string(dimension_count) +
                    " integer exponents, as in '@unit<double, 1, 0, -1>'"});
            return;
        }
        exponents.push_back(std::to_string(value));
    }
    exponents.resize(dimension_count, "0");
    auto dims = std::string{};
    for (const auto& exponent : exponents) dims += (dims.empty() ? "" : ", ") + exponent;
    declaration.alias = "cpp2::quantity<" + underlying + ", cpp2::dims<" + dims + ">>";
    if (!declaration.template_parameters.empty()) return;
    const auto spelling = render_cpp1_type(parse_type(declaration.alias));
    const ir_declaration* same = nullptr;
    ir_detail::visit_declarations(context.program.declarations,
        [&](const ir_declaration& other) {
            if (same == nullptr && &other != &declaration && !other.alias.empty() &&
                other.template_parameters.empty() && has_metaclass(other, "unit") &&
                render_cpp1_type(parse_type(other.alias)) == spelling) {
                same = &other;
            }
        });
    if (same != nullptr) {
        diagnostics.push_back({
            "unit-duplicate", diagnostic_severity::error, declaration.span,
            "'" + declaration.name + "' has the underlying type and exponents of '" +
                same->name + "', so both would name one type; a @unit type is distinguished "
                "by its dimensions only"});
    }
}

// Reflects the generated `type` after its nested types, as `apply_reflection`
// visits types.
auto reflect_nested(metafunction_context& context, ir_declaration& type, ir_declaration* parent)
    -> void {
    for (auto& member : type.members) {
        if (member.kind == declaration_kind::type) reflect_nested(context, member, &type);
    }
    reflect_type(context.program, type, parent, context.namespace_members,
                 context.surrounding, context.traits, context.experiments, context.diagnostics);
}

// Reflects the types a metafunction generated in `context.type`, which were
// added after reflection visited its nested types.
auto reflect_generated_types(metafunction_context& context) -> void {
    for (auto& member : context.type.members) {
        if (member.is_generated && member.kind == declaration_kind::type) {
            reflect_nested(context, member, &context.type);
        }
    }
}

// `@adt`: each bodyless function member `Name: (fields);` is a case, which
// becomes a nested `@struct` with those fields and `==`, so a case is built
// as `Name(fields...)` and may hold move-only fields. The type holds the
// active case in `_value`, a `std::variant` of the cases, converts implicitly
// from each case, has `is_Name()` and `as_Name()` accessors, and unless it
// declares `(out this, that)` is copied and moved as Cpp1 does. `is` and `as` see the variant through
// `cpp2_adt_value()`, which the runtime's `adt` overloads use.
auto apply_adt(metafunction_context& context) -> void {
    auto& program = context.program;
    auto& declaration = context.type;
    auto& diagnostics = context.diagnostics;
    const auto error = [&](std::string code, source_span span, std::string message) {
        diagnostics.push_back({std::move(code), diagnostic_severity::error, span,
                               std::move(message)});
    };
    struct adt_case final {
        std::string name;
        std::vector<parameter> fields;
    };
    auto cases = std::vector<adt_case>{};
    const auto first_diagnostic = diagnostics.size();
    std::erase_if(declaration.members, [&](const ir_declaration& member) {
        const auto is_object = is_data_member(member);
        const auto is_case = member.kind == declaration_kind::function &&
            receiver(member) == nullptr && member.body.empty() && !member.function_statements;
        if (!is_object && !is_case) return false;
        const auto well_formed = is_case && member.template_parameters.empty() &&
            member.named_returns.empty() && member.contracts.empty() &&
            return_form_of(member) == return_form::implicit_void &&
            std::all_of(member.parameters.begin(), member.parameters.end(),
                [](const parameter& field) {
                    return field.passing == passing_style::in && !field.is_variadic &&
                        field.default_value.empty() && !field.type.is_wildcard() &&
                        field.name != "_";
                });
        if (!well_formed) {
            error("adt-case-form", member.span,
                  "an @adt case is a list of typed fields: '" + member.name +
                  ": (name: type, ...);'");
        }
        else if (std::any_of(cases.begin(), cases.end(),
                             [&](const adt_case& other) { return other.name == member.name; })) {
            error("adt-duplicate-case", member.span,
                  "'" + member.name + "' is already a case of " + declaration.name);
        }
        else {
            cases.push_back({member.name, member.parameters});
        }
        return true;
    });
    if (diagnostics.size() != first_diagnostic) return;
    if (cases.empty()) {
        error("adt-empty", declaration.span, "an @adt must have at least one case");
        return;
    }
    // The cases compare as the type does, which a `@value` or `@ordered` type orders.
    auto comparisons = std::string{"operator==: (this, that) -> bool;\n"};
    for (const auto& metaclass : declaration.metaclasses) {
        if (const auto ordering = ordering_of(metaclass); !ordering.empty()) {
            comparisons += "operator<=>: (this, that) -> std::" + std::string{ordering} + ";\n";
            break;
        }
    }
    auto text = std::string{};
    auto variant = std::string{"std::variant<"};
    for (const auto& [name, fields] : cases) {
        text += name + ": @struct type = {\n";
        for (const auto& field : fields) text += field.name + ": " + field.type.text + ";\n";
        text += comparisons + "}\n";
        variant += (&name == &cases.front().name ? "" : ", ") + name;
    }
    variant += ">";
    text += "private _value: " + variant + ";\n";
    for (const auto& [name, fields] : cases) {
        text += "operator=: (implicit out this, copy _case: " + name + ") = { _value = _case; }\n"
            "is_" + name + ": (this) -> bool = std::holds_alternative<" + name + ">(_value);\n"
            "as_" + name + ": (this) -> forward " + name + " = std::get<" + name + ">(_value);\n"
            "as_" + name + ": (inout this) -> forward " + name + " = std::get<" + name +
            ">(_value);\n";
    }
    if (std::none_of(declaration.members.begin(), declaration.members.end(),
                     [](const ir_declaration& member) { return member.name == "operator=="; })) {
        text += "operator==: (this, that) -> bool;\n";
    }
    text += "cpp2_adt_value: (this) -> forward " + variant + " = _value;\n"
        "cpp2_adt_value: (inout this) -> forward " + variant + " = _value;\n"
        "cpp2_adt_value: (move this) -> forward_ref _ = _value;\n";
    add_generated_members(program, declaration, text, diagnostics);
    for (const auto& adt : cases) add_sum_case(declaration, adt.name, declaration_kind::type);
    reflect_generated_types(context);
    // Copy and move are Cpp1's, which a case with a move-only field leaves move-only.
    if (!has_constructor_with_that(declaration) && !is_value_type(declaration) &&
        !has_metaclass(declaration, "copyable")) {
        context.lower(member_generation::initialization);
    }
}

auto apply_ordered_value(metafunction_context& context) -> void {
    apply_ordered(context.program, context.type, ordering_of(context.name), context.diagnostics);
    if (context.name.ends_with("value")) {
        apply_basic_value(context.program, context.type, context.diagnostics);
    }
}

// As upstream's `@sample_traverser`, a type in a named namespace adds
// `add_1: (x) = x+1;` to that namespace. The reflection trace that upstream
// prints is not reproduced.
auto apply_sample_traverser(metafunction_context& context) -> void {
    auto* parent = context.parent;
    if (parent == nullptr || parent->kind != declaration_kind::namespace_node) return;
    auto add_1 = ir_declaration{};
    add_1.kind = declaration_kind::function;
    add_1.name = "add_1";
    add_1.scope = declaration_scope::namespace_scope;
    add_1.span = {parent->span.end, parent->span.end};
    add_1.is_generated = true;
    add_1.parameters = {parameter{passing_style::in, {}, "x"}};
    add_1.body = parsed_expression{"x+1"};
    context.namespace_members.emplace_back(parent, std::move(add_1));
}

// The declaration of the type named `type` as seen from `type_scope`, declared
// in `parent` (null at file scope): a nested type, one declared beside it, or
// one at file scope; a qualified name descends from where its first component
// is found. Null when no such type is declared in this translation unit.
auto find_type_declaration(const ir_program& program, const ir_declaration& type_scope,
                           const ir_declaration* parent, const type_id& type)
    -> const ir_declaration* {
    if (type.kind != type_kind::name || type.components.empty()) return nullptr;
    const auto find_in = [](const std::vector<ir_declaration>& declarations,
                            std::string_view name) -> const ir_declaration* {
        for (const auto& candidate : declarations) {
            if (candidate.name == name &&
                (candidate.kind == declaration_kind::type ||
                 candidate.kind == declaration_kind::namespace_node)) {
                return &candidate;
            }
        }
        return nullptr;
    };
    const auto& first = type.components.front().identifier;
    const auto* found = type.is_global ? nullptr : find_in(type_scope.members, first);
    if (found == nullptr && parent != nullptr && !type.is_global) {
        found = find_in(parent->members, first);
    }
    if (found == nullptr) found = find_in(program.declarations, first);
    for (auto position = std::size_t{1}; found != nullptr && position < type.components.size();
         ++position) {
        found = find_in(found->members, type.components[position].identifier);
    }
    return found != nullptr && found->kind == declaration_kind::type ? found : nullptr;
}

// `@sealed`: a `@polymorphic_base` or `@interface` whose derived types are all
// in this translation unit. The lowering is `seal_hierarchies`, once every type
// is reflected and the derived types are known.
auto apply_sealed(metafunction_context& context) -> void {
    const auto& type = context.type;
    if (!has_metaclass(type, "polymorphic_base") && !has_metaclass(type, "interface")) {
        context.diagnostics.push_back({
            "sealed-not-polymorphic", diagnostic_severity::error, type.span,
            "a @sealed type must also be @polymorphic_base or @interface"});
    }
    if (!type.template_parameters.empty()) {
        context.diagnostics.push_back({
            "sealed-template", diagnostic_severity::error, type.span,
            "a @sealed type cannot be a template"});
    }
    // A friend declaration cannot name a nested type declared after the base.
    if (context.parent != nullptr && context.parent->kind == declaration_kind::type) {
        context.diagnostics.push_back({
            "sealed-scope", diagnostic_severity::error, type.span,
            "a @sealed type must be declared at namespace scope"});
    }
}

// The `@sealed` lowering: the base's constructors become private and each
// derived type a friend, so a type in another translation unit cannot
// construct its base.
auto seal_hierarchies(ir_program& program, std::vector<diagnostic>& diagnostics) -> void {
    auto sealed = std::vector<ir_declaration*>{};
    ir_detail::visit_declarations(program.declarations, [&](ir_declaration& declaration) {
        if (declaration.kind == declaration_kind::type && has_metaclass(declaration, "sealed")) {
            sealed.push_back(&declaration);
        }
    });
    for (auto* base : sealed) {
        auto elsewhere = std::vector<const ir_declaration*>{};
        auto friends = std::string{};
        for (const auto* derived : sealed_derived_types(program, *base, &elsewhere)) {
            // A class template is a friend through its template head.
            auto head = std::string{};
            for (const auto& parameter : derived->template_parameters) {
                head += (head.empty() ? "template<" : ", ") +
                    (parameter.is_type_parameter() ? std::string{"typename"}
                                                   : render_cpp1_type(parameter.type)) +
                    (parameter.is_variadic ? "..." : "");
            }
            if (!head.empty()) head += "> ";
            friends += (friends.empty() ? "" : " ") + head + "friend " +
                (head.empty() ? "" : "struct ") + derived->name + ";";
            base->sum_cases.push_back(derived->symbol);
        }
        for (const auto* derived : elsewhere) {
            diagnostics.push_back({
                "sealed-derived-scope", diagnostic_severity::error, derived->span,
                "'" + derived->name + "' derives from the @sealed type '" + base->name +
                    "' but is not declared in the same scope"});
        }
        if (friends.empty()) {
            diagnostics.push_back({
                "sealed-no-derived", diagnostic_severity::warning, base->span,
                "nothing derives from the @sealed type '" + base->name +
                    "' in this translation unit"});
        }
        auto last_constructor = base->members.begin();
        for (auto member = base->members.begin(); member != base->members.end(); ++member) {
            if (!is_constructor(*member)) continue;
            member->access = access_kind::private_access;
            last_constructor = member + 1;
        }
        if (friends.empty()) continue;
        auto member = ir_declaration{};
        member.kind = declaration_kind::function;
        member.name = "__cpp2_sealed_friends";
        member.scope = declaration_scope::type_scope;
        member.access = access_kind::private_access;
        member.span = base->span;
        member.is_generated = true;
        member.is_type_member = true;
        member.cpp1_text = std::move(friends);
        member.symbol = add_symbol(program, {
            invalid_symbol, member.name, member.kind, base->symbol, 0, member.type, member.span,
            true});
        base->members.insert(last_constructor, std::move(member));
    }
}

// A public member function of a delegated-to type that `@delegate` forwards:
// one called on an object, and not a constructor, assignment, destructor or
// operator.
auto is_delegatable(const ir_declaration& member) -> bool {
    const auto* self = member.kind == declaration_kind::function ? receiver(member) : nullptr;
    return self != nullptr && !member.name.starts_with("operator") &&
        member.special_member == ir_declaration::special_member_kind::none &&
        (member.access == access_kind::public_access ||
         member.access == access_kind::default_access) &&
        (self->passing == passing_style::in || self->passing == passing_style::inout);
}

// The declarations from file scope to `target`, outermost first, or empty
// when `target` is not among `declarations` and their members.
auto declaration_path(const std::vector<ir_declaration>& declarations,
                      const ir_declaration& target) -> std::vector<const ir_declaration*> {
    for (const auto& declaration : declarations) {
        if (&declaration == &target) return {&declaration};
        auto path = declaration_path(declaration.members, target);
        if (!path.empty()) {
            path.insert(path.begin(), &declaration);
            return path;
        }
    }
    return {};
}

// The name of the last declaration of `path` qualified from the global
// namespace, as `::lib::Point`.
auto qualified_name(std::span<const ir_declaration* const> path) -> std::string {
    auto result = std::string{};
    for (const auto* declaration : path) result += "::" + declaration->name;
    return result;
}

// `text` with each unqualified name of a type, namespace or alias declared in
// a declaration of `path` qualified from the global namespace, as
// `::lib::Point`, so it means the same outside `path`. The names `hidden`
// are not qualified.
auto qualify_names(const std::string& text, const std::vector<const ir_declaration*>& path,
                   const std::vector<template_parameter>& hidden) -> std::string {
    const auto parsed = parse_expression_text(text);
    auto result = std::string{};
    auto previous = std::string_view{};
    for (const auto& token : parsed.tokens) {
        result += token.leading;
        const auto qualifies = token.kind == expression_token_kind::identifier &&
            previous != "::" && previous != "." &&
            std::none_of(hidden.begin(), hidden.end(),
                         [&](const template_parameter& value) { return value.name == token.text; });
        for (auto scope = path.size(); qualifies && scope-- > 0;) {
            const auto& members = path[scope]->members;
            if (std::any_of(members.begin(), members.end(), [&](const ir_declaration& member) {
                    return member.name == token.text &&
                        (member.kind == declaration_kind::type ||
                         member.kind == declaration_kind::namespace_node || !member.alias.empty());
                })) {
                result += qualified_name(std::span{path}.first(scope + 1)) + "::";
                break;
            }
        }
        result += token.text;
        previous = token.text;
    }
    return result;
}

// The forwarding function `name: (this, params...) -> R = { return this.member.name(args...); }`
// of `type` for `function` of `source`, the type of `member`, at `path`. Its
// parameter and return types are qualified to mean what they mean in `source`.
auto make_forwarding_function(const ir_declaration& type, std::string_view member,
                              const ir_declaration& function,
                              const std::vector<const ir_declaration*>& path)
    -> ir_declaration {
    auto call = "this." + std::string{member} + "." + function.name;
    for (const auto& value : function.template_parameters) {
        call += (&value == &function.template_parameters.front() ? "<" : ", ") + value.name +
            (value.is_variadic ? "..." : "");
    }
    call += function.template_parameters.empty() ? "(" : ">(";
    for (const auto& value : function.parameters) {
        if (is_this_parameter(value)) continue;
        if (call.back() != '(') call += ", ";
        // A `copy` or `move` parameter is moved as a definite last use.
        if (value.passing == passing_style::out) call += "out ";
        if (value.passing == passing_style::forward) call += "forward ";
        call += value.name;
        if (value.is_variadic) call += "...";
    }
    call += ")";
    const auto qualify = [&](const std::string& text) {
        return qualify_names(text, path, function.template_parameters);
    };
    const auto qualified = [&](const type_id& value) {
        auto text = qualify(value.text);
        return text == value.text ? value : parse_type(text);
    };
    auto forwarding = ir_declaration{};
    forwarding.kind = declaration_kind::function;
    forwarding.name = function.name;
    // A function with named return values returns the struct `source` declares.
    forwarding.type = function.named_returns.empty()
        ? qualified(function.type)
        : parse_type(qualified_name(path) + "::" + function.type.text);
    forwarding.deduction = function.deduction;
    forwarding.template_parameters = function.template_parameters;
    forwarding.requires_clause = function.requires_clause;
    forwarding.parameters = function.parameters;
    for (auto& value : forwarding.parameters) {
        value.type = qualified(value.type);
        if (auto text = qualify(value.default_value.source());
            text != value.default_value.source()) {
            value.default_value = parsed_expression{std::move(text)};
        }
    }
    forwarding.parameters.front() = parameter{function.parameters.front().passing,
                                              parse_type(type.name), "this"};
    forwarding.scope = declaration_scope::type_scope;
    forwarding.access = access_kind::public_access;
    forwarding.span = type.span;
    forwarding.is_generated = true;
    const auto form = return_form_of(function);
    // An expression body forwards as one, so the return type is deduced alike.
    if (form == return_form::implicit_deduced) {
        forwarding.body = parsed_expression{std::move(call)};
        return forwarding;
    }
    auto body = std::make_shared<function_body>();
    auto& added = body->statements.emplace_back(std::make_unique<statement>(
        form == return_form::implicit_void ? statement_kind::expression
                                           : statement_kind::return_statement));
    added->span = type.span;
    added->as<statement::expression_form>().text = parsed_expression{std::move(call)};
    forwarding.function_statements = std::move(body);
    return forwarding;
}

// `@delegate<member>` or `@delegate<member, f, g, ...>`: a forwarding function
// for every delegatable function of the type of the data member `member`, or
// for the named ones. A function the type already has is not forwarded; one
// that two `@delegate`s would forward is ambiguous.
auto apply_delegate(metafunction_context& context) -> void {
    auto& type = context.type;
    const auto error = [&](std::string id, std::string message) {
        context.diagnostics.push_back({std::move(id), diagnostic_severity::error, type.span,
                                       std::move(message)});
    };
    const auto names = context.arguments();
    if (names.empty()) {
        return error("delegate-unknown-member",
                     "@delegate needs the name of a data member, as in @delegate<m>");
    }
    const auto& member_name = names.front();
    const auto member = std::find_if(type.members.begin(), type.members.end(),
        [&](const ir_declaration& candidate) {
            return candidate.kind == declaration_kind::object && candidate.name == member_name &&
                !is_base_member(candidate);
        });
    if (member == type.members.end()) {
        return error("delegate-unknown-member",
                     "'" + type.name + "' has no data member named '" + member_name + "'");
    }
    const auto* source = find_type_declaration(context.program, type, context.parent, member->type);
    if (source == nullptr) {
        return error("delegate-unknown-type", "the type '" + member->type.text + "' of '" +
                     member_name + "' is not declared in this translation unit");
    }
    if (!source->template_parameters.empty()) {
        return error("delegate-template-type", "cannot delegate to '" + member_name +
                     "' because its type '" + source->name + "' is a template");
    }
    const auto selection = std::vector<std::string>{names.begin() + 1, names.end()};
    for (const auto& name : selection) {
        const auto has = [&](auto predicate) {
            return std::any_of(source->members.begin(), source->members.end(),
                [&](const ir_declaration& candidate) {
                    return candidate.name == name && predicate(candidate);
                });
        };
        if (!has([](const ir_declaration& candidate) {
                return candidate.kind == declaration_kind::function;
            })) {
            error("delegate-unknown-function",
                  "'" + source->name + "' has no function named '" + name + "'");
        }
        else if (!has(is_delegatable)) {
            error("delegate-not-forwardable",
                  "'" + source->name + "::" + name + "' is not a public function taking an 'in' "
                  "or 'inout' 'this', so @delegate cannot forward it");
        }
    }
    const auto path = declaration_path(context.program.declarations, *source);
    const auto named = [](const std::vector<std::string>& values, std::string_view name) {
        return std::find(values.begin(), values.end(), name) != values.end();
    };
    auto existing = std::vector<std::string>{};
    for (const auto& value : type.members) {
        if (!named(context.delegated, value.name)) existing.push_back(value.name);
    }
    auto ambiguous = std::vector<std::string>{};
    auto forwarded = std::vector<std::string>{};
    // Added after the loop: `source` may be a member of `type`.
    auto pending = std::vector<ir_declaration>{};
    for (const auto& function : source->members) {
        if (!is_delegatable(function) || (!selection.empty() && !named(selection, function.name)) ||
            named(existing, function.name)) {
            continue;
        }
        if (named(context.delegated, function.name)) {
            if (!named(ambiguous, function.name)) {
                ambiguous.push_back(function.name);
                error("delegate-ambiguous", "'" + function.name + "' is forwarded by more than "
                      "one @delegate of '" + type.name + "'");
            }
            continue;
        }
        if (!named(forwarded, function.name)) forwarded.push_back(function.name);
        pending.push_back(make_forwarding_function(type, member_name, function, path));
    }
    for (auto& function : pending) add_member(context.program, type, std::move(function));
    context.delegated.insert(context.delegated.end(), forwarded.begin(), forwarded.end());
}

struct metafunction final {
    std::string_view name;
    // Null for one applied elsewhere.
    void (*apply)(metafunction_context&);
    // The experiment that provides it, if any; unsupported unless enabled.
    std::optional<experiment_id> experiment = std::nullopt;
};

// The supported metafunctions. `@autodiff` is applied to the whole program
// before the others, and `@print` by the translator after them.
constexpr auto metafunctions = std::to_array<metafunction>({
    {"ordered", apply_ordered_value},
    {"weakly_ordered", apply_ordered_value},
    {"partially_ordered", apply_ordered_value},
    {"value", apply_ordered_value},
    {"weakly_ordered_value", apply_ordered_value},
    {"partially_ordered_value", apply_ordered_value},
    {"basic_value", [](metafunction_context& context) {
        apply_basic_value(context.program, context.type, context.diagnostics);
    }},
    {"copyable", [](metafunction_context& context) {
        static_cast<void>(apply_copyable(context.program, context.type, context.diagnostics));
    }},
    {"copy_constructible", [](metafunction_context& context) {
        apply_copy_constructible(context.program, context.type, context.diagnostics);
    }},
    {"hashable", [](metafunction_context& context) {
        apply_hashable(context.program, context.type, context.diagnostics);
    }},
    {"interface", [](metafunction_context& context) {
        apply_interface(context.program, context.type, context.diagnostics,
                        context.experiments.enabled("interface-defaults"));
    }},
    {"polymorphic_base", [](metafunction_context& context) {
        apply_polymorphic_base(context.program, context.type, context.diagnostics);
    }},
    {"singleton", [](metafunction_context& context) {
        apply_singleton(context.program, context.type, context.diagnostics);
    }},
    {"linear", [](metafunction_context& context) {
        apply_linear(context.program, context.type, context.diagnostics);
    }, experiment_id{"linear"}},
    {"noncopyable", [](metafunction_context& context) {
        apply_noncopyable(context.type, context.diagnostics);
    }},
    {"encapsulated", [](metafunction_context& context) {
        apply_encapsulated(context.type, context.diagnostics);
    }},
    {"cpp1_rule_of_zero", [](metafunction_context& context) {
        apply_cpp1_rule_of_zero(context.type, context.diagnostics);
        context.lower(member_generation::initialization);
    }},
    {"struct", [](metafunction_context& context) {
        apply_struct(context.program, context.type, context.diagnostics);
        context.lower(member_generation::bases);
    }},
    {"enum", [](metafunction_context& context) {
        apply_enum(context.program, context.type, context.diagnostics);
        context.lower(member_generation::initialization);
    }},
    {"flag_enum", [](metafunction_context& context) {
        apply_enum(context.program, context.type, context.diagnostics);
        context.lower(member_generation::initialization);
    }},
    {"union", [](metafunction_context& context) {
        apply_union(context.program, context.type, context.diagnostics);
    }},
    {"adt", apply_adt, experiment_id{"adt"}},
    {"regex", [](metafunction_context& context) {
        add_generated_members(context.program, context.type,
                              apply_regex(context.type, context.diagnostics),
                              context.diagnostics);
        reflect_generated_types(context);
    }},
    {"newtype", apply_newtype, experiment_id{"newtype"}},
    {"quantity", apply_newtype, experiment_id{"newtype"}},
    {"unit", apply_unit, experiment_id{"newtype"}},
    {"sample_traverser", apply_sample_traverser},
    {"trait", [](metafunction_context& context) {
        auto generated = apply_trait(context.program, context.type, context.parent,
                                     context.diagnostics);
        // A trait that failed becomes no Cpp1, so its bodyless functions are not checked.
        if (context.type.kind != declaration_kind::concept_declaration) {
            context.type.members.clear();
        }
        for (auto* declarations : {&generated.before, &generated.after}) {
            for (auto& declaration : *declarations) {
                if (declaration.kind == declaration_kind::type) {
                    reflect_nested(context, declaration, context.parent);
                }
            }
        }
        context.surrounding.emplace_back(context.type.symbol, std::move(generated));
        context.lower(member_generation::none);
    }, experiment_id{"trait"}},
    {"impl", [](metafunction_context& context) {
        if (apply_impl(context.program, context.type, context.traits, context.diagnostics)) {
            context.lower(member_generation::none);
        }
    }, experiment_id{"trait"}},
    {"given", [](metafunction_context& context) {
        context.surrounding.emplace_back(context.type.symbol,
            apply_given(context.program, context.type, context.parent, context.traits,
                        context.diagnostics));
        context.lower(member_generation::none);
    }, experiment_id{"given"}},
    {"delegate", apply_delegate, experiment_id{"delegate"}},
    {"sealed", apply_sealed, experiment_id{"sealed"}},
    {"autodiff", nullptr},
    {"print", nullptr},
});

auto find_metafunction(std::string_view name, const experiment_set& experiments)
    -> const metafunction* {
    const auto found = std::find_if(metafunctions.begin(), metafunctions.end(),
        [name](const metafunction& candidate) { return candidate.name == name; });
    if (found == metafunctions.end()) return nullptr;
    return !found->experiment || experiments.enabled(*found->experiment) ? &*found : nullptr;
}

// A name a metafunction generated (in `generated_by`, with that metafunction)
// is reserved for it: another member may have the name only when both are
// functions that overload, as two generated functions, operators and
// constructors do. The type's own name is its constructors'.
auto check_generated_names(const ir_declaration& type,
                           const std::map<std::string, std::string, std::less<>>& generated_by,
                           std::vector<diagnostic>& diagnostics) -> void {
    const auto overloads = [](const ir_declaration& member) {
        return member.name.starts_with("operator") ||
            member.special_member != ir_declaration::special_member_kind::none;
    };
    const auto clash = [&](const ir_declaration& a, const ir_declaration& b) {
        if (!a.is_generated && !b.is_generated) return false;
        if (a.kind != declaration_kind::function || b.kind != declaration_kind::function) {
            return true;
        }
        return a.is_generated != b.is_generated && !overloads(a) && !overloads(b);
    };
    for (const auto& [name, metafunction] : generated_by) {
        auto named = std::vector<const ir_declaration*>{};
        for (const auto& member : type.members) {
            if (member.name == name) named.push_back(&member);
        }
        // A member the user wrote, or one generated from it where it is written.
        const auto written = [&](const ir_declaration& member) {
            return !member.is_generated || member.span.begin.line != type.span.begin.line ||
                member.span.begin.column != type.span.begin.column;
        };
        const auto constructor_clash = std::find_if(named.begin(), named.end(),
            [&](const ir_declaration* member) { return !overloads(*member); });
        const ir_declaration* user = nullptr;
        auto found = name == type.name && constructor_clash != named.end();
        if (found) user = *constructor_clash;
        for (std::size_t i = 0; i < named.size(); ++i) {
            for (std::size_t j = i + 1; j < named.size(); ++j) {
                // Two written members are a redeclaration, left to Cpp1.
                if (!clash(*named[i], *named[j]) ||
                    (written(*named[i]) && written(*named[j]))) {
                    continue;
                }
                found = true;
                if (user == nullptr && written(*named[i])) user = named[i];
                if (user == nullptr && written(*named[j])) user = named[j];
            }
        }
        if (!found) continue;
        diagnostics.push_back({
            "metafunction-reserved-name", diagnostic_severity::error,
            user != nullptr ? user->span : type.span,
            "in a '@" + metafunction + "' type, the name '" + name + "' is reserved for use "
            "by the '@" + metafunction + "' implementation"});
    }
}

// Classifies the special members of the type `declaration`, applies its
// metafunctions in the order written, each to the type as the ones before it
// left it, and then generates what they leave to be generated.
auto reflect_type(ir_program& program, ir_declaration& declaration, ir_declaration* parent,
                  std::vector<std::pair<ir_declaration*, ir_declaration>>& namespace_members,
                  std::vector<std::pair<symbol_id, surrounding_declarations>>& surrounding,
                  const trait_table& traits, const experiment_set& experiments,
                  std::vector<diagnostic>& diagnostics) -> void {
    classify_special_members(declaration, diagnostics);
    auto context = metafunction_context{program, declaration, parent, {}, diagnostics,
                                        member_generation::all, namespace_members,
                                        surrounding, traits, experiments};
    // Each name a metafunction generated, with the first metafunction that did.
    auto generated_by = std::map<std::string, std::string, std::less<>>{};
    auto earlier = std::set<std::string, std::less<>>{};
    for (const auto& member : declaration.members) {
        if (member.is_generated) earlier.insert(member.name);
    }
    for (const auto& name : declaration.metaclasses) {
        context.index = static_cast<std::size_t>(&name - declaration.metaclasses.data());
        const auto* found = find_metafunction(name, experiments);
        if (found == nullptr) {
            const auto earlier = declaration.metaclasses.begin() +
                static_cast<std::ptrdiff_t>(context.index);
            // Reported once for each name.
            if (std::find(declaration.metaclasses.begin(), earlier, name) != earlier) continue;
            const auto known = std::find_if(metafunctions.begin(), metafunctions.end(),
                [&](const metafunction& candidate) { return candidate.name == name; });
            diagnostics.push_back({
                "unsupported-metaclass", diagnostic_severity::error, declaration.span,
                "'@" + name + "' " + (known == metafunctions.end()
                    ? std::string{"is not supported"}
                    : "requires '-experiment=" +
                          std::string{all_experiments[known->experiment->index()].name} + "'")});
            continue;
        }
        context.name = name;
        if (found->apply == nullptr) continue;
        found->apply(context);
        for (const auto& member : declaration.members) {
            if (member.is_generated && !earlier.contains(member.name)) {
                generated_by.try_emplace(member.name, name);
            }
        }
    }
    check_generated_names(declaration, generated_by, diagnostics);
    switch (context.generation) {
    case member_generation::all:
        finish_type(program, declaration, diagnostics);
        break;
    case member_generation::initialization:
        apply_member_initialization(declaration, diagnostics);
        apply_moves_from_that(program, declaration);
        mark_postfix_counterparts(declaration);
        break;
    case member_generation::bases:
        apply_base_initializers(declaration);
        break;
    case member_generation::none:
        break;
    }
}

// The declaration list holding the declaration with symbol `id`, and its index there.
auto locate(std::vector<ir_declaration>& declarations, symbol_id id)
    -> std::pair<std::vector<ir_declaration>*, std::size_t> {
    for (auto index = std::size_t{0}; index < declarations.size(); ++index) {
        if (declarations[index].symbol == id) return {&declarations, index};
        if (auto found = locate(declarations[index].members, id); found.first != nullptr) {
            return found;
        }
    }
    return {nullptr, 0};
}

}

auto sealed_derived_types(const ir_program& program, const ir_declaration& base,
                          std::vector<const ir_declaration*>* elsewhere)
    -> std::vector<const ir_declaration*> {
    // Every type with the declaration it is declared in (null at file scope).
    auto types = std::vector<std::pair<const ir_declaration*, const ir_declaration*>>{};
    const auto collect = [&](const auto& self, const std::vector<ir_declaration>& declarations,
                             const ir_declaration* parent) -> void {
        for (const auto& declaration : declarations) {
            if (declaration.kind == declaration_kind::type) types.emplace_back(&declaration, parent);
            self(self, declaration.members, &declaration);
        }
    };
    collect(collect, program.declarations, nullptr);
    const auto base_entry = std::find_if(types.begin(), types.end(),
                                         [&](const auto& entry) { return entry.first == &base; });
    if (base_entry == types.end()) return {};
    auto result = std::vector<const ir_declaration*>{};
    for (const auto& [type, parent] : types) {
        for (const auto& member : type->members) {
            if (!is_base_member(member) ||
                find_type_declaration(program, *type, parent, member.type) != &base) {
                continue;
            }
            if (parent == base_entry->second) result.push_back(type);
            else if (elsewhere != nullptr) elsewhere->push_back(type);
        }
    }
    return result;
}

auto apply_reflection(ir_program input, const experiment_set& experiments) -> reflection_result {
    auto result = reflection_result{std::move(input), {}};
    auto& program = result.program;
    // The functions the source declares, other than members of a type with
    // an unsupported metafunction, which might have given them their bodies.
    auto declared_functions = std::vector<symbol_id>{};
    auto unjudged = std::vector<symbol_id>{};
    ir_detail::visit_declarations(program.declarations, [&](const ir_declaration& value) {
        if (value.kind == declaration_kind::function && value.symbol != invalid_symbol) {
            declared_functions.push_back(value.symbol);
        }
        if (std::any_of(value.metaclasses.begin(), value.metaclasses.end(),
                [&](const std::string& name) { return !find_metafunction(name, experiments); })) {
            for (const auto& member : value.members) unjudged.push_back(member.symbol);
        }
    });
    std::erase_if(declared_functions, [&](symbol_id symbol) {
        return std::find(unjudged.begin(), unjudged.end(), symbol) != unjudged.end();
    });
    std::sort(declared_functions.begin(), declared_functions.end());
    apply_autodiff(program, result.diagnostics);
    const auto traits = experiments.enabled("trait") ? collect_traits(program) : trait_table{};
    auto namespace_members = std::vector<std::pair<ir_declaration*, ir_declaration>>{};
    auto surrounding = std::vector<std::pair<symbol_id, surrounding_declarations>>{};
    // Each declaration after its members, so a type's nested types are complete.
    const auto visit = [&](const auto& self, std::vector<ir_declaration>& declarations,
                           ir_declaration* parent) -> void {
        for (auto& declaration : declarations) {
            self(self, declaration.members, &declaration);
            if (declaration.kind == declaration_kind::type) {
                reflect_type(program, declaration, parent, namespace_members,
                             surrounding, traits, experiments, result.diagnostics);
                continue;
            }
            for (const auto& metaclass : declaration.metaclasses) {
                result.diagnostics.push_back({
                    "metafunction-requires-type", diagnostic_severity::error, declaration.span,
                    "'@" + metaclass + "' can only be applied to a type declaration"});
            }
        }
    };
    visit(visit, program.declarations, nullptr);
    // Namespaces are not inside types, so no member vector holding one changed.
    for (auto& [target, member] : namespace_members) {
        add_member(program, *target, std::move(member));
    }
    for (auto& [around, declarations] : surrounding) {
        const auto [siblings, index] = locate(program.declarations, around);
        const auto* preceding = find_symbol(program, around);
        if (siblings == nullptr || preceding == nullptr) continue;
        for (auto* inserted : {&declarations.before, &declarations.after}) {
            for (auto& declaration : *inserted) {
                if (declaration.symbol != invalid_symbol) continue;
                declaration.symbol = add_symbol(program, {
                    invalid_symbol, declaration.name, declaration.kind, preceding->parent, 0,
                    declaration.type, declaration.span, true});
            }
        }
        const auto position = static_cast<std::ptrdiff_t>(index);
        siblings->insert(siblings->begin() + position + 1,
                         std::make_move_iterator(declarations.after.begin()),
                         std::make_move_iterator(declarations.after.end()));
        siblings->insert(siblings->begin() + position,
                         std::make_move_iterator(declarations.before.begin()),
                         std::make_move_iterator(declarations.before.end()));
    }
    ir_detail::visit_declarations(program.declarations, [&](const ir_declaration& value) {
        // A function the source declares without a body is virtual, or
        // defaultable as Cpp1's `= default`.
        if (value.kind == declaration_kind::function && value.body.empty() &&
            std::binary_search(declared_functions.begin(), declared_functions.end(),
                               value.symbol) &&
            !value.function_statements && value.name != "operator==" &&
            value.name != "operator<=>" &&
            (value.parameters.empty() ||
             (value.parameters.front().this_specifier != virtual_specifier::virtual_function &&
              value.parameters.front().this_specifier != virtual_specifier::override_function))) {
            result.diagnostics.push_back({
                "missing-function-body", diagnostic_severity::error, value.span,
                "a function must have a body ('=' initializer), unless it is virtual (has a "
                "'virtual this' parameter) or is defaultable (operator== or operator<=>)"});
        }
        // Contracts are checked in a body, so a pure virtual or defaulted
        // function would drop them.
        if (value.kind == declaration_kind::function && !value.contracts.empty() &&
            value.body.empty() && !value.function_statements && value.cpp1_body.empty()) {
            result.diagnostics.push_back({
                "contract-without-body", diagnostic_severity::error, value.span,
                "a function without a body cannot have contracts, which are checked in its "
                "body"});
        }
        if (auto message = comparison_return_error(value); !message.empty()) {
            result.diagnostics.push_back({
                "comparison-return-type", diagnostic_severity::error, value.span,
                std::move(message)});
        }
        // Cpp1 does not allow a virtual function to deduce its return type.
        if (const auto form = return_form_of(value);
            value.kind == declaration_kind::function && !value.parameters.empty() &&
            value.parameters.front().this_specifier != virtual_specifier::none &&
            form != return_form::implicit_void && form != return_form::declared &&
            form != return_form::declared_forwarding) {
            result.diagnostics.push_back({
                "virtual-deduced-return", diagnostic_severity::error, value.span,
                "a virtual function cannot deduce its return type; declare it, as in "
                "'f: (virtual this) -> int = 1;'"});
        }
        // Each class would declare its own result type, which Cpp1 rejects
        // as an invalid covariant return type.
        if (value.kind == declaration_kind::function && !value.parameters.empty() &&
            value.parameters.front().this_specifier != virtual_specifier::none &&
            !value.named_returns.empty()) {
            result.diagnostics.push_back({
                "virtual-named-returns", diagnostic_severity::error, value.span,
                "a virtual function cannot have named return values; declare a return type, as in "
                "'f: (virtual this) -> std::pair<int, int>'"});
        }
    });
    if (experiments.enabled("sealed")) seal_hierarchies(program, result.diagnostics);
    if (experiments.enabled("trait")) check_trait_uses(program, traits, result.diagnostics);
    return result;
}

}
