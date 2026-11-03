#pragma once

#include <cstdint>
#include <string>

#include <cppfrontier/experiments.hpp>

namespace cppfrontier {

enum class trace_sink_kind : std::uint8_t { none, stderr_stream };

struct trace_sink final {
    trace_sink_kind kind = trace_sink_kind::none;
};

// How the generated file gets the standard library: inferred `#include`s, or
// all of it through `cpp2util.h` (`CPP2_INCLUDE_STD` or `CPP2_IMPORT_STD`).
enum class standard_library_mode : std::uint8_t { inferred, include_all, import_all };

struct translation_options final {
    // The path written in `#line` directives.
    std::string source_path;
    bool include_standard_headers = true;
    standard_library_mode standard_library = standard_library_mode::inferred;
    bool check_division_by_zero = true;
    bool check_subscripts = true;
    bool check_null_dereference = true;
    // `<`, `<=`, `>` and `>=` through `cpp2::impl::cmp_*`, which rejects mixed-sign integers.
    bool check_comparisons = true;
    bool exceptions = true;
    bool rtti = true;
    // Rejects Cpp1 lines other than comments and imports.
    bool pure_cpp2 = false;
    bool line_directives = true;
    // A leading comment naming the translator and its version.
    bool emit_version_info = false;
    // When set, the output is wrapped in `#ifndef`/`#define` of this name.
    std::string header_guard;
    // The opt-in language experiments; a stage asks `experiments.enabled("name")`.
    experiment_set experiments;
};

}
