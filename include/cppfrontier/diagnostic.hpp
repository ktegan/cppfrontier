#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <cppfrontier/source.hpp>

namespace cppfrontier {

enum class diagnostic_severity : std::uint8_t { information, warning, error };

struct diagnostic final {
    std::string code;
    diagnostic_severity severity = diagnostic_severity::error;
    source_span span;
    std::string message;
};

[[nodiscard]] inline auto severity_name(diagnostic_severity severity) noexcept -> std::string_view {
    switch (severity) {
    case diagnostic_severity::information: return "information";
    case diagnostic_severity::warning: return "warning";
    case diagnostic_severity::error: return "error";
    }
    return "error";
}

[[nodiscard]] inline auto compare_by_source(const diagnostic& left, const diagnostic& right) noexcept -> bool {
    if (left.span.begin.line != right.span.begin.line) {
        return left.span.begin.line < right.span.begin.line;
    }
    if (left.span.begin.column != right.span.begin.column) {
        return left.span.begin.column < right.span.begin.column;
    }
    if (left.severity != right.severity) {
        return left.severity < right.severity;
    }
    if (left.code != right.code) return left.code < right.code;
    return left.message < right.message;
}
}
