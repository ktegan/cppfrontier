#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <cppfrontier/diagnostic.hpp>
#include <cppfrontier/ir.hpp>

namespace cppfrontier {

// `regex` and `regex_*` are the data members that `@regex` replaces.
[[nodiscard]] auto is_regex_member_name(std::string_view name) -> bool;

// `@regex`: removes each regex data member of `type` and returns the Cpp2 text of
// the matcher type and `cpp2::regex::regular_expression` object for each, in name
// order.
[[nodiscard]] auto apply_regex(ir_declaration& type, std::vector<diagnostic>& diagnostics)
    -> std::string;

}
