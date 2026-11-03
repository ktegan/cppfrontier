#pragma once

#include <cppfrontier/ir.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace cppfrontier::ir_detail {

struct ignore_statements final {
    template <typename Statement>
    auto operator()(const Statement&) const noexcept -> void {}
};

template <typename Statements, typename Visitor>
auto visit_statements(Statements& statements, Visitor&& visitor) -> void {
    for (auto& statement : statements) {
        auto& value = *statement;
        visitor(value);
        for_each_nested_list(value, [&](auto& nested) { visit_statements(nested, visitor); });
    }
}

template <typename Declarations, typename DeclarationVisitor, typename StatementVisitor>
auto visit_declarations(Declarations& declarations, DeclarationVisitor&& on_declaration,
                        StatementVisitor&& on_statement) -> void {
    for (auto& declaration : declarations) {
        if (declaration.function_statements) {
            visit_statements(declaration.function_statements->statements, on_statement);
        }
        visit_declarations(declaration.members, on_declaration, on_statement);
        on_declaration(declaration);
    }
}

template <typename Declarations, typename DeclarationVisitor>
auto visit_declarations(Declarations& declarations, DeclarationVisitor&& on_declaration) -> void {
    visit_declarations(declarations, on_declaration, ignore_statements{});
}


// The spans of the `unsafe` blocks in `declarations` (experiment `unsafe`).
template <typename Declarations>
auto unsafe_spans(const Declarations& declarations) -> std::vector<source_span> {
    auto result = std::vector<source_span>{};
    visit_declarations(declarations, [](const auto&) {}, [&](const statement& value) {
        if (value.kind == statement_kind::unsafe_block) result.push_back(value.span);
    });
    return result;
}

// Whether `span` is lexically inside one of `spans`.
inline auto inside_any(const std::vector<source_span>& spans, source_span span) -> bool {
    const auto key = [](source_position position) {
        return std::pair{position.line, position.column};
    };
    return std::any_of(spans.begin(), spans.end(), [&](const source_span& outer) {
        return key(outer.begin) <= key(span.begin) && key(span.end) <= key(outer.end);
    });
}

}
