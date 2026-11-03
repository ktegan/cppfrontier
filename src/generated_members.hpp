#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <cppfrontier/diagnostic.hpp>
#include <cppfrontier/ir.hpp>

namespace cppfrontier {

// Columns of the positions in generated members start here; no source line is
// this long.
inline constexpr auto generated_column = std::uint32_t{100000};

// Declarations a metafunction inserts before and after the declaration it
// applies to, in the same scope.
struct surrounding_declarations final {
    std::vector<ir_declaration> before;
    std::vector<ir_declaration> after;
};

struct generated_members final {
    std::vector<ir_declaration> members;
    // The first error, if `text` is not valid Cpp2.
    std::string error;
};

// Parses `text`, Cpp2 member declarations a metafunction generates for the
// type or namespace `target`, into IR members marked as generated, allocating
// their symbols in `program`. Their positions move to the line of `origin`, at
// distinct columns from `generated_column` on.
[[nodiscard]] auto parse_generated_members(std::string_view text, const ir_declaration& target,
                                           ir_program& program, source_span origin)
    -> generated_members;

// Moves each diagnostic inside a generated member to the declaration the
// member was generated from, and says it is in generated code.
auto locate_generated_diagnostics(const ir_program& program,
                                  std::vector<diagnostic>& diagnostics) -> void;

}
