#pragma once

#include <cppfrontier/ir.hpp>

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace cppfrontier {

// Where a name is looked up: in `scope`, within its item `ordinal`. Items are
// a scope's declarations or statements, in source order.
struct scope_point final {
    static constexpr auto none = static_cast<std::size_t>(-1);
    std::size_t scope = none;
    std::size_t ordinal = none;

    [[nodiscard]] auto valid() const noexcept -> bool { return scope != none; }
};

// The scopes of a program and the declarations in each. Namespace and type
// scopes are unordered: each member is visible throughout. In function,
// block, statement-parameter and function-expression scopes, a declaration
// is visible from its own item onward, so a local is in scope in its own
// initializer. A function expression's scope is nested where it appears.
class scope_model final {
public:
    enum class scope_kind : std::uint8_t {
        namespace_scope,
        type,
        function,
        block,
        statement_parameters,
        function_expression,
    };

    enum class entry_kind : std::uint8_t {
        object,
        function,
        type,
        namespace_node,
        concept_declaration,
        parameter,
        // A statement parameter `(x := value)`, which is in scope in its initializer.
        statement_parameter,
        template_parameter,
        // `using N::name;`, which names `target`.
        using_name,
    };

    struct entry final {
        std::string name;
        entry_kind kind = entry_kind::object;
        symbol_id symbol = invalid_symbol;
        source_span span;
        std::size_t scope = scope_point::none;
        std::size_t ordinal = scope_point::none;
        // The scope a namespace, type or function opens.
        std::size_t inner = scope_point::none;
        // The declared type of an object or parameter, and of a value template
        // parameter; empty for a type template parameter.
        type_id type;
        // The qualified name a using-declaration or namespace alias names.
        std::string target;
        // The type a type alias names.
        std::string alias;
        bool is_template = false;
        // A function whose first parameter is `this`.
        bool has_this = false;
        // A `static` local or an alias, which is not an ordinary local object.
        bool is_static_or_alias = false;
        const statement* declaring_statement = nullptr;
        const parameter* declared_parameter = nullptr;
    };

    explicit scope_model(const ir_program& program);

    // The point of a statement in a function body, or of a statement parameter.
    [[nodiscard]] auto at(const statement& value) const -> scope_point;
    [[nodiscard]] auto at(const initialized_parameter& value) const -> scope_point;
    // The innermost point containing a source position.
    [[nodiscard]] auto at(source_position position) const -> scope_point;
    // Inside a declaration: a function's signature and body, or an object's initializer.
    [[nodiscard]] auto inside(symbol_id declaration) const -> scope_point;
    // Where a declaration appears; for a function expression, the expression's point.
    [[nodiscard]] auto outside(symbol_id declaration) const -> scope_point;

    // Unqualified or qualified lookup of `name` at `point`, through enclosing
    // scopes, members of enclosing types, and `using` declarations.
    [[nodiscard]] auto lookup(std::string_view name, scope_point point) const -> const entry*;
    // A member of the namespace or type `owner` names.
    [[nodiscard]] auto member(const entry& owner, std::string_view name) const -> const entry*;
    // The declaration a using-declaration or namespace alias names, else `value`.
    [[nodiscard]] auto resolve(const entry& value) const -> const entry*;
    // The type whose members are visible at `point` through `this`.
    [[nodiscard]] auto enclosing_type(scope_point point) const -> const entry*;
    // Whether `point` is in the initializer of the object `value` declares.
    [[nodiscard]] auto under_initialization(const entry& value, scope_point point) const -> bool;
    // Whether `value` is declared directly in the scope `owner` opens.
    [[nodiscard]] auto declared_in(const entry& value, const entry& owner) const -> bool;
    // The local a declaration statement declares.
    [[nodiscard]] auto declared_by(const statement& value) const -> const entry*;
    // The local a binding declares; none for `_`.
    [[nodiscard]] auto declared_by(const statement::binding& value) const
        -> const entry*;
    // The entry of a function, statement, range or catch parameter.
    [[nodiscard]] auto declared_as(const parameter& value) const -> const entry*;
    // The parameter `name` of the function `function`.
    [[nodiscard]] auto parameter_of(symbol_id function, std::string_view name) const
        -> const entry*;

private:
    struct item final {
        source_span span;
        // The block and statement-parameter scopes anchored at this item.
        std::vector<std::size_t> blocks;
    };

    struct name_hash final {
        using is_transparent = void;
        auto operator()(std::string_view value) const noexcept -> std::size_t {
            return std::hash<std::string_view>{}(value);
        }
    };

    struct scope final {
        scope_kind kind = scope_kind::namespace_scope;
        std::size_t parent = scope_point::none;
        std::size_t anchor = scope_point::none;
        std::vector<source_span> spans;
        std::vector<item> items;
        std::vector<std::size_t> children;
        // `using N::_;` directives, with the ordinal where each appears.
        std::vector<std::pair<std::size_t, std::string>> using_namespaces;
        bool generated = false;
        // The entry of the namespace or type that opens this scope.
        std::size_t owner = scope_point::none;
        // The spans of the other non-generated child scopes, ordered by begin.
        std::vector<std::pair<source_span, std::size_t>> regions;
        // Whether item begins, and item ends, are non-decreasing, and whether
        // `regions` are disjoint; each allows a binary search in `at`.
        bool begins_sorted = true;
        bool ends_sorted = true;
        bool regions_disjoint = true;
        // The entries declared in this scope by name.
        std::unordered_map<std::string, std::vector<std::size_t>, name_hash, std::equal_to<>>
            names;
    };

    std::vector<scope> scopes_;
    std::vector<entry> entries_;
    std::unordered_map<const void*, scope_point> statement_points_;
    std::unordered_map<const statement*, std::size_t> statement_entries_;
    std::unordered_map<const statement::binding*, std::size_t> binding_entries_;
    std::unordered_map<const parameter*, std::size_t> parameter_entries_;
    std::unordered_map<symbol_id, std::size_t> declaration_entries_;
    std::unordered_map<symbol_id, std::size_t> declaration_scopes_;
    // Symbols declared inside others by name and position, used while
    // building to find the symbols of parameters.
    std::map<std::tuple<std::string, std::uint32_t, std::uint32_t>, std::vector<const symbol*>>
        parameter_symbols_;

    auto add_scope(scope_kind kind, std::size_t parent, std::size_t anchor,
                   std::vector<source_span> spans, bool generated) -> std::size_t;
    auto add_item(std::size_t scope, source_span span) -> std::size_t;
    auto add_region(std::size_t scope, source_span span, std::size_t child) -> void;
    auto add_entry(entry value) -> std::size_t;
    // The symbol of `value`, declared in `parent` when one is given.
    [[nodiscard]] auto parameter_symbol(const parameter& value,
                                        std::optional<symbol_id> parent) const -> symbol_id;
    auto add_declaration(const ir_declaration& declaration, std::size_t scope, bool generated)
        -> void;
    auto add_function_scope(const ir_declaration& declaration, scope_kind kind,
                            std::size_t parent, std::size_t anchor, bool generated)
        -> std::size_t;
    auto add_statements(const statement_list& statements, std::size_t scope, bool generated)
        -> void;
    auto add_statement(const statement& value, std::size_t scope, bool generated) -> void;
    // Adds a scope for the names a condition of `value` binds, in which `body`
    // is nested, and returns the body's block scope; just the block without them.
    auto add_bound(const statement& value, const std::vector<statement::binding>& bindings,
                   const statement_list& body, std::size_t parent, std::size_t anchor,
                   bool generated) -> std::size_t;
    // Adds a block scope for `statements` and `leading`, unless both are empty, and returns it.
    auto add_nested(const statement_list& statements, std::size_t parent, std::size_t anchor,
                    const parameter* leading, bool generated) -> std::size_t;

    [[nodiscard]] auto ordered(std::size_t scope) const -> bool;
    [[nodiscard]] auto find_in(std::size_t scope, std::size_t ordinal, std::string_view name) const
        -> const entry*;
    [[nodiscard]] auto qualified(std::string_view name, scope_point point, int depth) const
        -> const entry*;
    [[nodiscard]] auto unqualified(std::string_view name, scope_point point, int depth) const
        -> const entry*;
    [[nodiscard]] auto resolve(const entry& value, int depth) const -> const entry*;
};

}
