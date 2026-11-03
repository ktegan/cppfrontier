#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <cppfrontier/expression.hpp>
#include <cppfrontier/source.hpp>

namespace cppfrontier {

enum class passing_style : std::uint8_t { in, in_ref, copy, inout, out, move, forward };

enum class type_kind : std::uint8_t {
    none,         // no type is written
    deduced,      // `_` or `auto`; `_ is C` holds the constraint `C` as its child
    name,         // a possibly qualified name; see `components`
    fundamental,  // a Cpp2 fundamental type keyword such as `i32`, or a Cpp1 one such as `int`
    pointer,      // `*T`; the child is `T`
    constant,     // `const T`; the child is `T`
    reference,    // a Cpp1 `T&` or `T&&`, as generated members spell them
    pack,         // `T...`; the child is `T`
    function,     // `(parameters) -> R`; the children are the parameter types, then `R`
    decltype_of,  // `decltype(e)` or `type_of(e)`, kept as text
    expression,   // a non-type template argument such as `:i32 = 0` or `x.size()`
    cpp1,         // any other spelling, kept as text
    union_type,         // `A | B` (experiment `type-operators`); the children are the operands
    intersection_type,  // `A & B` (experiment `type-operators`); the children are the operands
};

struct type_id;

// One `::`-separated part of a name, such as `vector<int>` in `std::vector<int>`.
struct type_name_component final {
    std::string identifier;
    std::vector<type_id> arguments;
    // `X<>` has an empty argument list.
    bool has_arguments = false;
    // Written `template X<...>`.
    bool has_template_keyword = false;
};

// A parameter of a function type; its type is the child at the same index.
struct type_parameter final {
    passing_style passing = passing_style::in;
    // Empty when unnamed.
    std::string name;
};

// A Cpp2 type-id, parsed once where it is written.
struct type_id final {
    type_kind kind = type_kind::none;
    // The Cpp2 spelling, without surrounding whitespace.
    std::string text;
    // Where the type is written; empty for a generated type.
    source_span span;
    std::vector<type_name_component> components;
    std::vector<type_id> children;
    std::vector<type_parameter> parameters;
    // `::std::string`.
    bool is_global = false;
    // Written with the disambiguator `type`, as in `type T::member`.
    bool has_type_marker = false;
    // A qualified name whose scope depends on a template parameter of an
    // enclosing declaration, as in `T::x` or `std::vector<T>::size_type`, or
    // is a `decltype`, so Cpp1 needs `typename`. Set by `build_ir`.
    bool is_dependent = false;
    // A `T&&` reference.
    bool is_rvalue = false;
    // `-> forward T`.
    bool returns_forward = false;

    [[nodiscard]] auto empty() const noexcept -> bool { return kind == type_kind::none; }
    // No type, `_`, `auto`, or a constrained `_ is C`: the type is deduced.
    [[nodiscard]] auto is_wildcard() const noexcept -> bool {
        return kind == type_kind::none || kind == type_kind::deduced;
    }
    // `C` in `_ is C`, or null.
    [[nodiscard]] auto constraint() const noexcept -> const type_id* {
        return kind == type_kind::deduced && !children.empty() ? &children.front() : nullptr;
    }
    // The Cpp1 spelling `auto`, which Cpp2 source spells `_`.
    [[nodiscard]] auto is_cpp1_auto() const noexcept -> bool {
        return kind == type_kind::deduced && text == "auto";
    }
    [[nodiscard]] auto is_function() const noexcept -> bool { return kind == type_kind::function; }
    // `A | B` or `A & B`.
    [[nodiscard]] auto is_operator() const noexcept -> bool {
        return kind == type_kind::union_type || kind == type_kind::intersection_type;
    }
    [[nodiscard]] auto is_void() const noexcept -> bool {
        return kind == type_kind::fundamental && text == "void";
    }
    // The type below any leading `*` and `const`.
    [[nodiscard]] auto core() const noexcept -> const type_id&;
    [[nodiscard]] auto core() noexcept -> type_id&;
};

struct type_parse_options final {
    // Experiment `type-operators`: `A | B` and `A & B` are type operators.
    // `|` binds loosest, then `&`; `*` and `const` apply to one operand and a
    // parenthesized operand groups. A function type takes the whole range.
    bool type_operators = false;
};

// Parses the type-id `text`, written at `span` when it comes from source.
[[nodiscard]] auto parse_type(std::string_view text, source_span span = {},
                              type_parse_options options = {}) -> type_id;
// Parses tokens [first, last) of an expression as a type-id.
[[nodiscard]] auto parse_type(const std::vector<expression_token>& tokens, std::size_t first,
                              std::size_t last, type_parse_options options = {}) -> type_id;

// The number of `*` levels of `type`, looking through `const`.
[[nodiscard]] auto pointer_depth(const type_id& type) noexcept -> int;

// Whether `name` occurs in `type` as an unqualified identifier.
[[nodiscard]] auto mentions_name(const type_id& type, std::string_view name) -> bool;

// Whether `type` has a `decltype(e)` part, which may depend on a template
// parameter, including the implicit one of a generic function.
[[nodiscard]] auto mentions_decltype(const type_id& type) -> bool;

// Marks the dependent qualified names in the type positions of `type`: those
// whose first component is one of the template parameters `names`, or whose
// scope has template arguments that mention one or a `decltype`, and the later
// components with template arguments take `template`; and members of a
// `decltype` scope, such as `decltype(x)::value_type`. Template arguments are
// not type positions, since a dependent name there may be a value, unless
// marked `type`.
auto mark_dependent_names(type_id& type, const std::vector<std::string>& names) -> void;

// Qualifies each name in `type` whose first component is `name` with the
// components of the name `scope`, as `typename` when `dependent`.
auto qualify_name(type_id& type, std::string_view name, const type_id& scope, bool dependent)
    -> void;

// The Cpp1 spelling of the Cpp2 fundamental type name `name`, such as `int`
// for `i32`, or empty.
[[nodiscard]] auto cpp1_fundamental_spelling(std::string_view name) noexcept -> std::string_view;

// Whether `name` is a Cpp2 keyword naming a fundamental type, which is never a value.
[[nodiscard]] auto is_fundamental_type_keyword(std::string_view name) noexcept -> bool;

// Whether `name` names a fundamental type by itself: a Cpp2 name such as
// `i32` or a Cpp1 one such as `int` or `bool`.
[[nodiscard]] auto is_fundamental_type_name(std::string_view name) noexcept -> bool;

// Whether `word` is one of the words of a multi-word Cpp1 fundamental type.
[[nodiscard]] auto is_cpp1_fundamental_word(std::string_view word) noexcept -> bool;

// The Cpp2 spelling of the multi-word Cpp1 fundamental type `words`, such as
// `'ulong'` for `unsigned long`, or empty.
[[nodiscard]] auto cpp2_fundamental_suggestion(const std::vector<std::string_view>& words)
    -> std::string_view;

}
