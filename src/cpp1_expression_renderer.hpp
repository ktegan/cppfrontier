#pragma once

#include <cppfrontier/experiments.hpp>
#include <cppfrontier/expression.hpp>

#include "cpp1_type_renderer.hpp"
#include "scope_model.hpp"

#include <string>
#include <vector>

namespace cppfrontier {

// The statements that `e?` operands hoist before the statement being rendered
// (the `try-operator` experiment): `auto&& name = e; if (!name) return ...;`.
struct try_hoisting final {
    std::vector<std::string> statements;
    // Names issued so far in the translation unit.
    std::size_t count = 0;
};

struct cpp1_expression_options {
    struct wrapped_parameter final {
        std::string name;
        // The Cpp1 type, or empty when deduced.
        std::string type;
        // An `out` parameter, which every assignment constructs.
        bool out = false;
        // The declaration, which applies only where `name` finds it.
        const scope_model::entry* entry = nullptr;
    };

    bool check_division_by_zero = true;
    bool check_subscripts = true;
    bool check_null_dereference = true;
    // `<`, `<=`, `>` and `>=` through `cpp2::impl::cmp_*`, which rejects mixed-sign integers.
    bool check_comparisons = true;
    experiment_set experiments;
    // Experiment `checked-arithmetic`: `+`, `-`, `*`, unary `-`, `+=`, `-=`,
    // `*=`, `++` and `--` on known integers through `cpp2::checked_*`.
    bool check_arithmetic = false;
    // UFCS calls use the `_NONLOCAL` macros, for code outside function bodies.
    bool nonlocal = false;
    // `x.f(args)` is a member call, as in the operand of `decltype` in a type,
    // which is rendered without name lookup.
    bool member_calls = false;
    // Name lookup for the rendered text, which appears at `point`.
    const scope_model* scopes = nullptr;
    scope_point point;
    std::vector<wrapped_parameter> wrapped_parameters;
    // Definite last uses in the rendered text, which render as rvalues.
    const std::vector<last_use>* last_uses = nullptr;
    // The assignments in the rendered text that construct a deferred local;
    // another assignment to one assigns its `.value()`.
    const std::vector<construction>* constructions = nullptr;
    // The `try-operator` experiment: where `?` operands are hoisted.
    try_hoisting* try_hoists = nullptr;
};

// Experiment `lazy`: a lazy object, which every rendered expression reads
// through `.value()`. A name applies where lookup finds its symbol, or
// everywhere when the expression has no `point`.
struct lazy_object final {
    std::string name;
    symbol_id symbol = invalid_symbol;
};

// Makes `objects` the lazy objects of every expression rendered on this thread
// while it lives, in any position: a body, an initializer, a default argument
// or a signature. Lowering one program creates one.
class lazy_objects_scope final {
public:
    explicit lazy_objects_scope(std::vector<lazy_object> objects);
    ~lazy_objects_scope();
    lazy_objects_scope(const lazy_objects_scope&) = delete;
    auto operator=(const lazy_objects_scope&) -> lazy_objects_scope& = delete;

private:
    std::vector<lazy_object> objects_;
    const std::vector<lazy_object>* previous_;
};

// Turns off the runtime checks that the `-no-*-checks` flags control, and
// checked arithmetic, as for code inside an `unsafe` block (experiment `unsafe`).
inline auto disable_runtime_checks(cpp1_expression_options& options) -> void {
    options.check_division_by_zero = false;
    options.check_subscripts = false;
    options.check_null_dereference = false;
    options.check_comparisons = false;
    options.check_arithmetic = false;
}

[[nodiscard]] auto render_cpp1_expression(
    std::string_view text, cpp1_expression_options options = {}) -> std::string;
[[nodiscard]] auto render_cpp1_expression(
    const parsed_expression& expression, cpp1_expression_options options = {}) -> std::string;

// Each item of `expression` when it is a list `(a, b, ...)` (see `whole_list`),
// else the whole expression.
[[nodiscard]] auto render_cpp1_expression_items(
    const parsed_expression& expression, cpp1_expression_options options = {})
    -> std::vector<std::string>;

// An operand of the top-level `&&` chain of a condition with pattern tests
// (experiment `condition-bindings`): a test, or for a pattern test the
// subject, its test of the name that holds it, and its bindings' declarations.
struct cpp1_condition_operand final {
    std::string subject;
    std::string test;
    std::string bindings;
};

// The operands of `condition`, whose pattern test `k` holds its subject in
// `name` followed by `k`.
[[nodiscard]] auto render_cpp1_condition(const parsed_expression& condition,
                                         cpp1_expression_options options, std::string_view name)
    -> std::vector<cpp1_condition_operand>;
// A let-else initializer `value as T` (experiment `let-else`) whose value is
// held in `name`: the value, the test `name is T`, and in `bindings`, `name as T`.
[[nodiscard]] auto render_cpp1_let_else(const parsed_expression& initializer,
                                        cpp1_expression_options options, std::string_view name)
    -> cpp1_condition_operand;
// The expressions of the interpolations `(expr)$` in a string literal token,
// in order, without format specifications.
// Tokens `first` to `last` as a type, with the names that depend on a template
// parameter found from `point`, or in `parameters`, marked; see `mark_dependent_names`.
[[nodiscard]] auto parse_dependent_type(const std::vector<expression_token>& tokens,
                                        std::size_t first, std::size_t last,
                                        const scope_model* scopes, scope_point point,
                                        std::vector<std::string> parameters = {},
                                        type_parse_options type_options = {}) -> type_id;
[[nodiscard]] auto parse_dependent_type(std::string_view text, const scope_model* scopes,
                                        scope_point point,
                                        std::vector<std::string> parameters = {},
                                        type_parse_options type_options = {}) -> type_id;

// `type` with each `decltype(x)` of a deferred local or `out` parameter `x` in
// `wrapped` replaced by `x`'s declared type, not that of its wrapper.
[[nodiscard]] auto with_declared_types(
    type_id type, const std::vector<cpp1_expression_options::wrapped_parameter>& wrapped)
    -> type_id;
// `type` with each `decltype(x)`, or `decltype(x)::member`, whose `x` finds a
// deferred local or `out` parameter at `options.point` replaced by its declared
// type; without a point, those in `options.wrapped_parameters`.
[[nodiscard]] auto with_declared_types(type_id type, const cpp1_expression_options& options)
    -> type_id;

[[nodiscard]] auto interpolated_expressions(std::string_view literal) -> std::vector<std::string>;
// The offset in `literal` of the character at `offset` in the interpolated
// expression `ordinal` of `interpolated_expressions(literal)`.
[[nodiscard]] auto interpolated_offset(std::string_view literal, std::size_t ordinal,
                                       std::size_t offset) -> std::size_t;
// Whether the subject `node` of an `is` or `inspect` is visibly a type at
// `point`, which makes `node is X` a compile-time query on the type.
[[nodiscard]] auto is_type_subject(const std::vector<expression_token>& tokens,
                                   const expression& node, const scope_model& scopes,
                                   scope_point point) -> bool;
// Whether `name` is a Cpp2 support name such as `finally`, which Cpp1 spells `cpp2::finally`.
[[nodiscard]] auto is_cpp2_support_name(std::string_view name) -> bool;
// Whether `name` renders as `cpp2::name` in an expression: a support name, and
// with `checked-arithmetic` a `wrapping_*` helper. Declaring one is an error.
[[nodiscard]] auto is_cpp2_support_name(std::string_view name, const experiment_set& experiments)
    -> bool;

// The operator between operands `index - 1` and `index` of the binary `node`,
// as one string: `>=` may be lexed as `>` `=`.
[[nodiscard]] auto binary_operator(const std::vector<expression_token>& tokens,
                                   const expression& node, std::size_t index) -> std::string;
// The kind of a comparison operator in a chain: 'l' for `<` and `<=`, 'g' for
// `>` and `>=`, 'e' for `==`, 'n' for `!=`, or '\0' for another operator.
[[nodiscard]] auto comparison_kind(std::string_view op) noexcept -> char;

}
