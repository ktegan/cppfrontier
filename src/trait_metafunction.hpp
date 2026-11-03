#pragma once

#include <map>
#include <string>
#include <vector>

#include <cppfrontier/diagnostic.hpp>
#include <cppfrontier/ir.hpp>

#include "generated_members.hpp"

namespace cppfrontier {

struct trait_info final {
    std::string name;
    // `::geo::Shape`.
    std::string qualified_name;
    // The qualified name of the namespace declaring the trait, `::geo`, or
    // empty at file scope.
    std::string scope_name;
    std::vector<std::string> functions;
};

// The `@trait` types of a program, by symbol, and the trait that each
// `@impl` or `@given` names, by the symbol of the type it is applied to.
struct trait_table final {
    std::map<symbol_id, trait_info> traits;
    std::map<symbol_id, symbol_id> named;

    [[nodiscard]] auto named_by(const ir_declaration& type) const -> const trait_info*;
};

// Collects the traits of `program` before reflection, and resolves the trait
// each `@impl` and `@given` names where it is declared.
[[nodiscard]] auto collect_traits(const ir_program& program) -> trait_table;

// `@trait`: `type`, declared in `parent` (null at file scope), becomes the
// concept `Trait`, preceded by the type `Trait_calls` and followed by the
// types `Trait_archetype` and `dyn_Trait`, all generated as Cpp2.
[[nodiscard]] auto apply_trait(ir_program& program, ir_declaration& type,
                               const ir_declaration* parent,
                               std::vector<diagnostic>& diagnostics) -> surrounding_declarations;

// `@impl<Trait>` on a type, or `@impl<Trait, Type>` on an empty marker type: a
// trailing `static_assert(Trait<Type>)`. Returns whether `type` is a marker,
// which gets no generated members.
auto apply_impl(const ir_program& program, ir_declaration& type, const trait_table& traits,
                std::vector<diagnostic>& diagnostics) -> bool;

// `@given<Trait, Type>`: `type`, declared in `parent` (null at file scope),
// holds the trait's functions for `Type`, each taking the instance first.
// They become free functions following `type`, which is left an empty marker.
[[nodiscard]] auto apply_given(ir_program& program, ir_declaration& type,
                               const ir_declaration* parent, const trait_table& traits,
                               std::vector<diagnostic>& diagnostics)
    -> surrounding_declarations;

// After reflection: diagnoses a trait used as the type of an object, parameter
// or return value, and adds a trailing check to each namespace-scope function
// template whose template parameters are all traits, which instantiates its
// body with the traits' archetypes.
auto check_trait_uses(ir_program& program, const trait_table& traits,
                      std::vector<diagnostic>& diagnostics) -> void;

}
