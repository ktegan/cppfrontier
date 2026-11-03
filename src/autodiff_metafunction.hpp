#pragma once

#include <string>
#include <vector>

#include <cppfrontier/diagnostic.hpp>
#include <cppfrontier/ir.hpp>

namespace cppfrontier {

// `@autodiff`: for each use of the metaclass, in program order, generates the
// derivative of each function of the type, and of the functions and types those
// functions use, as upstream's reflect.h2 does. The derivatives are Cpp2 text
// parsed into ordinary IR members, so reflection and lowering treat them like
// user declarations.
auto apply_autodiff(ir_program& program, std::vector<diagnostic>& diagnostics) -> void;

// The runtime support headers, such as `"cpp2taylor.h"`, that the `@autodiff`
// uses in `declarations` need.
[[nodiscard]] auto autodiff_includes(const std::vector<ir_declaration>& declarations)
    -> std::vector<std::string>;

}
