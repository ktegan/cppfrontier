#pragma once

#include <cppfrontier/type.hpp>

#include <string>
#include <string_view>

namespace cppfrontier {

// The Cpp1 type-id of `type`. A deduced type is `auto`.
[[nodiscard]] auto render_cpp1_type(const type_id& type) -> std::string;
// The same for a type in an expression, where a fundamental alias such as
// `u8` is the runtime's `cpp2::u8`, one name, so `u8(5)` is a functional cast.
[[nodiscard]] auto render_cpp1_expression_type(const type_id& type) -> std::string;
// The Cpp1 declaration of `name` with `type`, such as `int* p` or `void(*f)(int)`.
[[nodiscard]] auto render_cpp1_type_declaration(const type_id& type, std::string_view name)
    -> std::string;
// The same for a type spelled as Cpp2 text, such as one a metafunction generates.
[[nodiscard]] auto render_cpp1_type(std::string_view type) -> std::string;
[[nodiscard]] auto render_cpp1_type_declaration(std::string_view type, std::string_view name)
    -> std::string;
// The check that `subject`, a Cpp1 type, satisfies the constraint `C<args...>`
// in `_ is C<args...>` or `T: C<args...>`: `C<subject, args...>`. A union or
// intersection constraint is the disjunction or conjunction of its operands',
// without surrounding parentheses.
[[nodiscard]] auto render_cpp1_constraint(const type_id& constraint, std::string_view subject)
    -> std::string;
// Whether the Cpp1 spelling of `type`, with or without the `cpp2::` aliases of
// fundamental types, is one simple type specifier, so `T{}` value-initializes
// it; otherwise `std::type_identity_t<T>{}` is needed.
[[nodiscard]] auto is_simple_type_specifier(const type_id& type, bool aliases = false) -> bool;

}
