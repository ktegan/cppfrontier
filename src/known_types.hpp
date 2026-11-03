#pragma once

#include <cppfrontier/expression.hpp>
#include <cppfrontier/ir.hpp>

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "scope_model.hpp"

namespace cppfrontier {

// A type this translation unit states for a value, and where its names are
// looked up.
struct known_type final {
    type_id type;
    scope_point point;
};

// The functions a call may name, each viable for its number of arguments.
// Argument 0 is the receiver: the object of a member or UFCS call `x.f()`,
// or the implicit `this` of an unqualified call of a member function;
// argument `n` is the call's `n`th argument.
struct call_candidates final {
    std::vector<const ir_declaration*> functions;
    // The call has an explicit receiver, `x.f()`.
    bool receiver = false;

    // The parameter of `function` that argument `argument` binds, or null.
    [[nodiscard]] auto parameter_for(const ir_declaration& function, std::size_t argument) const
        -> const parameter*;

    // Whether every candidate, and at least one, binds `argument` to a
    // parameter for which `test` holds.
    template<class Test>
    [[nodiscard]] auto all_bind(std::size_t argument, const Test& test) const -> bool {
        if (functions.empty()) return false;
        for (const auto* function : functions) {
            const auto* found = parameter_for(*function, argument);
            if (found == nullptr || !test(*found)) return false;
        }
        return true;
    }
};

// What the declarations of this translation unit say about the types of
// values and the callees of calls. A type is known when it is written, other
// than `_`, or deduced from an initializer whose type is known: a literal,
// `:T = v`, `x as T`, `T(...)` for a type of this translation unit, a call of
// a function of it with a written return type, or a name or member of known
// type. A range-for parameter has the element type of a range of type
// `std::C<T, ...>` for a standard container `C`, and a destructuring binding
// its template argument of a `std::pair` or `std::tuple`.
class known_types final {
public:
    known_types(const ir_program& program, const scope_model& scopes);

    [[nodiscard]] auto scopes() const -> const scope_model& { return scopes_; }

    // The type of the local, parameter or member `value`.
    [[nodiscard]] auto type_of(const scope_model::entry& value) const -> std::optional<known_type>;
    // The type of the expression `node` of `text`, evaluated at `point`.
    [[nodiscard]] auto type_of(const expression_text& text, const expression& node,
                               scope_point point) const -> std::optional<known_type>;
    // The type of this translation unit that `type` names, or null.
    [[nodiscard]] auto declared_type(const known_type& type) const -> const scope_model::entry*;
    // A fundamental, pointer or reference type, whose move is a copy.
    [[nodiscard]] static auto is_scalar(const known_type& type) -> bool;

    // The functions of this translation unit that the call `call` of `text`
    // at `point` may name. The receiver of `x.f()` binds `this` of a member
    // function `f` of the type of `x` when it has one, or else the first
    // parameter of a function `f` that unqualified lookup finds.
    [[nodiscard]] auto callees(const expression_text& text, const expression& call,
                               scope_point point) const -> call_candidates;
    // The functions of this translation unit, declared in one scope, that `found` names.
    [[nodiscard]] auto overloads(const scope_model::entry* found) const
        -> std::vector<const ir_declaration*>;

private:
    struct initializer final {
        const parsed_expression* text = nullptr;
        scope_point point;
    };

    const scope_model& scopes_;
    // The functions by the scope they are declared in and their name.
    std::map<std::pair<std::size_t, std::string>, std::vector<const ir_declaration*>> functions_;
    // The initializers of statement parameters, and the ranges of range-for parameters.
    std::unordered_map<const parameter*, initializer> initializers_;
    std::unordered_map<const parameter*, initializer> ranges_;

    [[nodiscard]] auto type_of(const scope_model::entry& value, int depth) const
        -> std::optional<known_type>;
    [[nodiscard]] auto type_of(const expression_text& text, const expression& node,
                               scope_point point, int depth) const -> std::optional<known_type>;
    // The overloads a call's callee names, and whether it has an explicit receiver.
    [[nodiscard]] auto callee_functions(const expression_text& text, const expression& callee,
                                        scope_point point, int depth) const
        -> std::pair<std::vector<const ir_declaration*>, bool>;
};

}
