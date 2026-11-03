#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include <cppfrontier/expression.hpp>
#include <cppfrontier/source.hpp>
#include <cppfrontier/type.hpp>

namespace cppfrontier {

using symbol_id = std::uint64_t;
constexpr symbol_id invalid_symbol = 0;

struct ast_node {
    source_span span;
};

struct expression_statement final : ast_node {
    parsed_expression text;
};

// `virtual`, `override`, or `final` before `this`.
enum class virtual_specifier : std::uint8_t { none, virtual_function, override_function, final_function };

struct parameter final {
    passing_style passing = passing_style::in;
    type_id type;
    std::string name;
    source_span name_span;
    // `x: int = 1`; emitted only where the function is first declared.
    parsed_expression default_value;
    virtual_specifier this_specifier = virtual_specifier::none;
    // `args...: T` declares a function parameter pack.
    bool is_variadic = false;
};

// A parameter with an initializer: a statement parameter `(x := value)` or a
// named return value `-> (x: int = value)`.
struct initialized_parameter final {
    parameter value;
    parsed_expression initializer;
};

[[nodiscard]] inline auto is_main_args_parameter(const parameter& value) noexcept -> bool {
    return value.name == "args" && value.passing == passing_style::in &&
        value.type.is_wildcard() && !value.type.is_cpp1_auto();
}

enum class declaration_kind : std::uint8_t {
    function,
    object,
    namespace_node,
    type,
    concept_declaration,
    using_declaration,
};

enum class access_kind : std::uint8_t { default_access, public_access, protected_access, private_access };

enum class declaration_scope : std::uint8_t { file, namespace_scope, type_scope, function_scope };

enum class return_deduction : std::uint8_t {
    none,
    by_value,
    by_forwarding,
    by_forwarding_ref,
    by_forwarding_typed,
};

struct template_parameter final {
    std::string name;
    // A value parameter's type, such as `i32` in `N: i32`, or `_`; empty for a
    // type parameter, `T` or `T: type`.
    type_id type;
    // A type parameter's default type.
    type_id default_type;
    // A value parameter's default value.
    parsed_expression default_value;
    // `T: A & B` or `T: A | B` (experiment `type-operators`): the parameter is a
    // type and the declaration requires the conjunction or disjunction.
    type_id constraint;
    bool is_variadic = false;

    [[nodiscard]] auto is_type_parameter() const noexcept -> bool { return type.empty(); }
    [[nodiscard]] auto has_default() const noexcept -> bool {
        return !default_type.empty() || !default_value.empty();
    }
};

// The `<group, flags...>(condition, message)` of a contract; an empty group is
// the default group.
struct contract_clause {
    std::string group;
    // Where the group begins in the source.
    std::optional<source_position> group_begin;
    std::vector<parsed_expression> flags;
    parsed_expression condition;
    parsed_expression message;
};

// A function's `pre<group, flags...>(condition, message)` or `post(...)`.
// Each capture `x$` in a postcondition reads as a name bound to `x` on entry.
struct function_contract final : contract_clause {
    struct capture final {
        std::string name;
        parsed_expression expression;
    };

    bool is_post = false;
    std::vector<capture> captures;
};

struct declaration_signature {
    declaration_kind kind = declaration_kind::object;
    access_kind access = access_kind::default_access;
    std::string name;
    // The declared type; a function's return type.
    type_id type;
    return_deduction deduction = return_deduction::none;
    std::vector<template_parameter> template_parameters;
    parsed_expression requires_clause;
    std::vector<parameter> parameters;
    // `-> (name: type, ...)`; `type` then names the generated result struct.
    std::vector<initialized_parameter> named_returns;
    std::vector<std::string> metaclasses;
    // The `<arguments...>` of each metaclass, such as `u8` in `@enum<u8>`, as
    // the source text of each comma-separated argument.
    std::vector<std::vector<std::string>> metaclass_arguments;
    declaration_scope scope = declaration_scope::file;
    bool is_entry_point = false;
    // A function declared with `==`.
    bool is_constexpr = false;
    // A `final type`.
    bool is_final = false;
    // Experiment `lazy`: a `name: lazy T = init` object, initialized on first use.
    bool is_lazy = false;
    std::vector<function_contract> contracts;
};

// The arguments of the first metaclass `name` of `declaration`, such as `f32`
// in `@newtype<f32>`, or null when it has none named `name`.
[[nodiscard]] inline auto metaclass_arguments_of(const declaration_signature& declaration,
                                                 std::string_view name) noexcept
    -> const std::vector<std::string>* {
    for (std::size_t index = 0; index < declaration.metaclasses.size(); ++index) {
        if (declaration.metaclasses[index] == name) {
            return index < declaration.metaclass_arguments.size()
                ? &declaration.metaclass_arguments[index] : nullptr;
        }
    }
    return nullptr;
}

[[nodiscard]] inline auto has_metaclass(const declaration_signature& declaration,
                                        std::string_view name) noexcept -> bool {
    return std::find(declaration.metaclasses.begin(), declaration.metaclasses.end(), name) !=
        declaration.metaclasses.end();
}

// Receiver and special-member shapes. `this` must be the first parameter and
// `that` the second.
[[nodiscard]] inline auto is_this_parameter(const parameter& value) noexcept -> bool {
    return value.name == "this";
}

[[nodiscard]] inline auto is_that_parameter(const parameter& value) noexcept -> bool {
    return value.name == "that";
}

[[nodiscard]] inline auto receiver(const declaration_signature& declaration) noexcept
    -> const parameter* {
    return !declaration.parameters.empty() && is_this_parameter(declaration.parameters.front())
        ? &declaration.parameters.front() : nullptr;
}

[[nodiscard]] inline auto has_receiver(const declaration_signature& declaration,
                                       passing_style passing) noexcept -> bool {
    const auto* value = receiver(declaration);
    return value != nullptr && value->passing == passing;
}

[[nodiscard]] inline auto that_parameter(const declaration_signature& declaration) noexcept
    -> const parameter* {
    return declaration.parameters.size() > 1 && is_that_parameter(declaration.parameters[1])
        ? &declaration.parameters[1] : nullptr;
}

[[nodiscard]] inline auto is_special_operator(const declaration_signature& declaration,
                                              passing_style receiver_passing) noexcept -> bool {
    return declaration.kind == declaration_kind::function && declaration.name == "operator=" &&
        has_receiver(declaration, receiver_passing);
}

// `operator=: (out this, ...)` is a constructor.
[[nodiscard]] inline auto is_value_set_constructor(const declaration_signature& declaration) noexcept
    -> bool {
    return is_special_operator(declaration, passing_style::out);
}

// `operator=: (inout this, ...)` is an assignment.
[[nodiscard]] inline auto is_value_set_assignment(const declaration_signature& declaration) noexcept
    -> bool {
    return is_special_operator(declaration, passing_style::inout);
}

// `operator=: (move this)` is a destructor.
[[nodiscard]] inline auto is_destructor(const declaration_signature& declaration) noexcept
    -> bool {
    return is_special_operator(declaration, passing_style::move);
}

enum class statement_kind : std::uint8_t {
    expression,
    discard_statement,
    block,
    try_statement,
    if_statement,
    while_statement,
    do_statement,
    for_statement,
    break_statement,
    continue_statement,
    return_statement,
    declaration_statement,
    destructuring_statement,
    using_declaration,
    contract_statement,
    // Experiment `defer`: a block that runs when its enclosing block exits.
    defer_statement,
    // Experiment `unsafe`: a block whose runtime checks are not emitted.
    unsafe_block,
};

// Identifies a statement, so that analysis results about it survive copies
// of the IR. Unique within the process.
using statement_id = std::uint64_t;

[[nodiscard]] inline auto next_statement_id() noexcept -> statement_id {
    static auto next = std::atomic<statement_id>{0};
    return next.fetch_add(1, std::memory_order_relaxed) + 1;
}

struct statement;
using statement_list = std::vector<std::unique_ptr<statement>>;

// Shared by the AST and IR; the IR builder copies statements and assigns symbols.
// `form` holds the data of the kind: see `form_of`.
struct statement final : ast_node {
    struct declaration_data final {
        symbol_id symbol = invalid_symbol;
        std::string name;
        type_id type;
        parsed_expression initializer;
        std::string alias;
        std::vector<template_parameter> template_parameters;
        declaration_kind kind = declaration_kind::object;
        bool is_alias = false;
        bool is_static = false;
        // A local holding one of the function's named return values.
        bool is_named_return = false;
    };

    struct handler final {
        parameter caught;
        statement_list body;
    };

    // A name that a destructuring, a condition's pattern test (experiment
    // `condition-bindings`) or a let-else (experiment `let-else`) declares.
    struct binding final {
        std::string name;
        source_span span;
        symbol_id symbol = invalid_symbol;
    };

    // `expression`, `discard_statement` and `return_statement`; a bare
    // `return` has empty text.
    struct expression_form final {
        parsed_expression text;
    };

    struct declaration_form final {
        // `name = initializer;`, or the statement text when it declares nothing.
        parsed_expression text;
        std::optional<declaration_data> declaration;
        // Experiment `let-else`: `name := value as T else { ... }`, whose
        // initializer is `value as T`, runs `else_body` unless `value is T`.
        statement_list else_body;
        bool is_let_else = false;
    };

    // `(a, b) := initializer;` (experiment `destructuring`): a deduced local
    // for each element of the initializer, in order; `_` discards an element.
    struct destructuring_form final {
        using binding = statement::binding;
        std::vector<binding> bindings;
        parsed_expression initializer;
        // Experiment `let-else`: `(a, b) := value as T else { ... }`.
        statement_list else_body;
        bool is_let_else = false;
    };

    struct using_form final {
        std::string name;
    };

    // `assert<group, flags...>(condition, message)`.
    using contract_form = contract_clause;

    // `break_statement` and `continue_statement`.
    struct jump_form final {
        std::string label;
    };

    // `block`, `try_statement`, `defer_statement` and `unsafe_block`; only a
    // try has handlers.
    // A block with parameters is a parameterized statement, and its body holds
    // that statement.
    struct block_form final {
        statement_list body;
        std::vector<handler> handlers;
        std::vector<initialized_parameter> parameters;
    };

    struct if_form final {
        parsed_expression condition;
        statement_list body;
        // A plain `else` holds its statements; `else if` holds one if-statement
        // with `is_else_if` set.
        statement_list else_body;
        // The names the condition's pattern tests bind (experiment
        // `condition-bindings`), in order, in scope in the body.
        std::vector<binding> bindings;
        bool has_else = false;
        bool is_else_if = false;
        bool is_constexpr = false;
    };

    // `while_statement`, `do_statement` and `for_statement`. A for statement's
    // condition is its range.
    struct loop_form final {
        std::string label;
        parsed_expression condition;
        parsed_expression next;
        std::optional<parameter> range_parameter;
        statement_list body;
        // The names a while condition's pattern tests bind (experiment
        // `condition-bindings`), in order, in scope in the body.
        std::vector<binding> bindings;
    };

    using form_type = std::variant<expression_form, declaration_form, destructuring_form,
                                   using_form, jump_form, block_form, if_form, loop_form,
                                   contract_form>;

    statement_kind kind = statement_kind::expression;
    symbol_id symbol = invalid_symbol;
    // A copy of the statement keeps its id.
    statement_id id = next_statement_id();
    form_type form;

    statement() = default;
    explicit statement(statement_kind value) : kind{value}, form{form_of(value)} {}

    [[nodiscard]] static auto form_of(statement_kind kind) -> form_type {
        switch (kind) {
        case statement_kind::declaration_statement: return declaration_form{};
        case statement_kind::destructuring_statement: return destructuring_form{};
        case statement_kind::using_declaration: return using_form{};
        case statement_kind::break_statement:
        case statement_kind::continue_statement: return jump_form{};
        case statement_kind::block:
        case statement_kind::try_statement:
        case statement_kind::defer_statement:
        case statement_kind::unsafe_block: return block_form{};
        case statement_kind::if_statement: return if_form{};
        case statement_kind::while_statement:
        case statement_kind::do_statement:
        case statement_kind::for_statement: return loop_form{};
        case statement_kind::contract_statement: return contract_form{};
        default: return expression_form{};
        }
    }

    template <typename Form>
    [[nodiscard]] auto as() -> Form& { return std::get<Form>(form); }
    template <typename Form>
    [[nodiscard]] auto as() const -> const Form& { return std::get<Form>(form); }
    template <typename Form>
    [[nodiscard]] auto is() const noexcept -> bool { return std::holds_alternative<Form>(form); }
};

// The expression text of an expression or declaration statement.
[[nodiscard]] inline auto statement_text(const statement& value) -> const parsed_expression* {
    if (const auto* form = std::get_if<statement::expression_form>(&value.form)) return &form->text;
    if (const auto* form = std::get_if<statement::declaration_form>(&value.form)) return &form->text;
    return nullptr;
}

// Calls `visit` on each statement list nested directly in `value`, in source order.
template <typename Statement, typename Visitor>
auto for_each_nested_list(Statement& value, Visitor&& visit) -> void {
    std::visit([&](auto& form) {
        using form_type = std::remove_cvref_t<decltype(form)>;
        if constexpr (std::is_same_v<form_type, statement::block_form>) {
            visit(form.body);
            for (auto& handler : form.handlers) visit(handler.body);
        }
        else if constexpr (std::is_same_v<form_type, statement::if_form>) {
            visit(form.body);
            visit(form.else_body);
        }
        else if constexpr (std::is_same_v<form_type, statement::loop_form>) {
            visit(form.body);
        }
        else if constexpr (std::is_same_v<form_type, statement::declaration_form> ||
                           std::is_same_v<form_type, statement::destructuring_form>) {
            if (form.is_let_else) visit(form.else_body);
        }
    }, value.form);
}

struct function_body final : ast_node {
    statement_list statements;
};

struct declaration final : ast_node, declaration_signature {
    using node_kind = declaration_kind;
    std::vector<std::unique_ptr<declaration>> members;
    source_span body_span;
    std::unique_ptr<expression_statement> body;
    std::unique_ptr<function_body> block_body;
    std::string alias;
    std::string using_name;
};

struct ast final {
    std::vector<std::unique_ptr<declaration>> declarations;
};
}
