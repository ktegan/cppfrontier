#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <cppfrontier/source.hpp>
#include <cppfrontier/token.hpp>

namespace cppfrontier {

struct translation_options;

[[nodiscard]] auto standard_include_directives(const source_text& source,
                                               const token_list& tokens,
                                               const translation_options& options)
    -> std::vector<std::string>;

[[nodiscard]] auto include_header(std::string_view line) -> std::string;

// Adds `#include "header"` after the `cpp2util.h` include of the generated
// `preamble`, for the runtime header of an experiment that the lowered Cpp1
// uses, such as `cpp2checked.h` for a `cpp2::checked_*` call.
auto add_runtime_include(std::string& preamble, std::string_view header) -> void;

}
